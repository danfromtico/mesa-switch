# SPDX-License-Identifier: MIT
# Shared gate for baseline selection, publication and on-device permanent permission.

function Read-SwitchPolicyJson([string]$Path) {
    if (![IO.File]::Exists($Path)) { throw "Required policy or evidence file is missing: $Path" }
    $document = [IO.File]::ReadAllText($Path) | ConvertFrom-Json -ErrorAction Stop
    if ($null -eq $document -or $document -isnot [pscustomobject]) { throw "Expected a JSON object: $Path" }
    return $document
}

function Assert-SwitchHash([string]$Value) {
    if ($Value -notmatch '^[0-9a-f]{64}$') { throw 'Invalid SHA-256 in policy or evidence' }
}

function Test-SwitchRecordId($Value) {
    return (($Value -is [int] -or $Value -is [long]) -and $Value -gt 0)
}

function Assert-SwitchProfile($Profile) {
    if ($Profile -isnot [pscustomobject]) { throw 'Missing component profile' }
    $names = @($Profile.PSObject.Properties.Name)
    foreach ($name in @('tegragpurender.sys','TegraGpuD3DVk.dll','libvulkan_nouveau.dll','tg_d3d_arm64.dll','tg_d3d_x64.dll','tg_nvk_arm64.dll','tg_nvk_x64.dll')) {
        if ($name -notin $names) { throw "Required graphics component missing: $name" }
    }
    foreach ($entry in $Profile.PSObject.Properties) {
        if ($entry.Name -notmatch '^[a-z0-9_-]+\.(sys|dll)$') { throw "Unexpected graphics component name: $($entry.Name)" }
        Assert-SwitchHash $entry.Value
    }
}

function Test-SwitchSameProfile($First, $Second) {
    Assert-SwitchProfile $First
    Assert-SwitchProfile $Second
    if (@($First.PSObject.Properties).Count -ne @($Second.PSObject.Properties).Count) { return $false }
    foreach ($entry in $First.PSObject.Properties) {
        $other = $Second.PSObject.Properties[$entry.Name]
        if (!$other -or $other.Value -ne $entry.Value) { return $false }
    }
    return $true
}

function Assert-SwitchChangedComponentLoads($Accepted,$Candidate,$Evidence) {
    $desktopNames=@('tegragpurender.sys','TegraGpuD3DVk.dll','libvulkan_nouveau.dll','tg_d3d_arm64.dll','tg_nvk_arm64.dll')
    foreach ($entry in $Candidate.PSObject.Properties) {
        if ($entry.Name -in $desktopNames) { continue }
        $prior=if($Accepted){$Accepted.PSObject.Properties[$entry.Name]}
        if ($prior -and $prior.Value -eq $entry.Value) { continue }
        $measured=@($Evidence.cases|Where-Object {
            $_.before_boot -eq $Evidence.trial.boot_record -and $_.loaded_components -and
            $_.loaded_components.PSObject.Properties[$entry.Name].Value -eq $entry.Value
        })
        if (!$measured.Count) { throw "No test on the candidate boot loaded the changed component: $($entry.Name)" }
    }
}

function Assert-SwitchPassingCase($Case) {
    if ([string]::IsNullOrWhiteSpace($Case.name) -or $Case.passed -isnot [bool] -or !$Case.passed) { throw 'A passing named hardware case is required' }
    if (!(Test-SwitchRecordId $Case.before_boot) -or !(Test-SwitchRecordId $Case.after_boot) -or $Case.after_boot -ne $Case.before_boot) { throw 'A hardware case crossed a boot or lacks boot evidence' }
    Assert-SwitchHash $Case.evidence_sha256
}

function Assert-SwitchFaultLedger($Ledger) {
    if ($Ledger.schema -ne 1 -or $Ledger.faults -isnot [array] -or $Ledger.independent_qualifications -isnot [array]) { throw 'Invalid open-fault ledger schema' }
    $ids = @{}
    foreach ($fault in $Ledger.faults) {
        if ($fault.id -notmatch '^[A-Z0-9][A-Z0-9-]+$' -or $ids.ContainsKey($fault.id)) { throw 'Missing or duplicate fault identifier' }
        $ids[$fault.id] = $true
        if ($fault.status -cnotin @('open','closed') -or $fault.affected_scopes -isnot [array] -or !$fault.affected_scopes.Count -or $fault.evidence -isnot [array] -or !$fault.evidence.Count) { throw "Incomplete fault: $($fault.id)" }
        foreach ($field in @('kind','trigger','cause','daily_workaround','close_when')) {
            if ([string]::IsNullOrWhiteSpace($fault.$field)) { throw "Fault $($fault.id) lacks $field" }
        }
        foreach ($v in @($fault.affected_scopes)+@($fault.evidence)) {
            if ($v -isnot [string] -or [string]::IsNullOrWhiteSpace($v)) { throw "Empty fault reference or scope: $($fault.id)" }
        }
        if ($fault.status -ceq 'open') {
            if ($null -ne $fault.resolution) { throw "Open fault has a closure record: $($fault.id)" }
            continue
        }
        $fix = $fault.resolution
        if (!$fix -or [string]::IsNullOrWhiteSpace($fix.root_cause) -or $fix.root_cause_confirmed -isnot [bool] -or !$fix.root_cause_confirmed -or $fix.commit -notmatch '^[0-9a-f]{40}$' -or $fix.original_scenario_runs -isnot [array] -or $fix.original_scenario_runs.Count -lt 3) { throw "Closure evidence is incomplete: $($fault.id)" }
        Assert-SwitchHash $fix.causal_evidence_sha256
        if('presentation' -in $fault.affected_scopes -or 'wddm' -in $fault.affected_scopes){
            Assert-SwitchProfile $fix.components
        }else{
            if($fix.components -isnot [pscustomobject] -or !@($fix.components.PSObject.Properties.Name).Count){throw 'Missing repaired component identities'}
            foreach($entry in $fix.components.PSObject.Properties){Assert-SwitchHash $entry.Value}
        }
        foreach ($test in $fix.original_scenario_runs) {
            Assert-SwitchPassingCase $test
            foreach ($field in @('original_trigger','output_verified','normal_exit','desktop_available')) {
                if ($test.$field -isnot [bool] -or !$test.$field) { throw "Closure run lacks $field for $($fault.id)" }
            }
        }
        if (@($fix.original_scenario_runs.evidence_sha256 | Sort-Object -Unique).Count -ne $fix.original_scenario_runs.Count) { throw "Duplicate closure evidence: $($fault.id)" }
    }
    $approvals = @{}
    foreach ($approval in $Ledger.independent_qualifications) {
        Assert-SwitchHash $approval.evidence_sha256
        if ($approvals.ContainsKey($approval.evidence_sha256) -or $approval.independence -isnot [array] -or !$approval.independence.Count) { throw 'Invalid independent qualification' }
        $approvals[$approval.evidence_sha256] = $true
        $seen = @{}
        foreach ($item in $approval.independence) {
            if (!$ids.ContainsKey([string]$item.fault_id) -or $seen.ContainsKey([string]$item.fault_id) -or [string]::IsNullOrWhiteSpace($item.reason)) { throw 'Invalid independent fault evidence' }
            $seen[$item.fault_id] = $true
            Assert-SwitchHash $item.test_sha256
        }
    }
}

function Assert-SwitchAcceptedBaseline($Baseline) {
    if ($Baseline.schema -ne 1 -or $Baseline.source_commit -notmatch '^[0-9a-f]{40}$' -or [string]::IsNullOrWhiteSpace($Baseline.directory) -or [string]::IsNullOrWhiteSpace($Baseline.qualification)) { throw 'Invalid accepted graphics baseline' }
    Assert-SwitchProfile $Baseline.components
}

function Assert-SwitchFaultHistory($Previous,$Current) {
    Assert-SwitchFaultLedger $Previous
    Assert-SwitchFaultLedger $Current
    foreach ($fault in $Previous.faults) {
        if ($fault.id -cnotin @($Current.faults.id)) { throw "Fault disappeared without a retained closure: $($fault.id)" }
    }
}

function Assert-SwitchPromotion($Ledger, $Evidence, [string]$EvidenceSha256, $Candidate, [string]$Directory) {
    Assert-SwitchFaultLedger $Ledger
    Assert-SwitchHash $EvidenceSha256
    if ($Evidence.schema -ne 1 -or $Evidence.change_kind -cnotin @('presentation','independent') -or $Evidence.source_commit -notmatch '^[0-9a-f]{40}$' -or $Evidence.base_commit -notmatch '^[0-9a-f]{40}$' -or $Evidence.changed_paths -isnot [array] -or !$Evidence.changed_paths.Count -or $Evidence.directory -ne $Directory -or [string]::IsNullOrWhiteSpace($Directory)) { throw 'Invalid candidate identity or change scope' }
    if (!(Test-SwitchSameProfile $Evidence.components $Candidate)) { throw 'Candidate artifacts differ from the hardware evidence' }
    $trial = $Evidence.trial
    if (!$trial -or $trial.one_time -isnot [bool] -or !$trial.one_time -or ($trial.gpu_code -isnot [int] -and $trial.gpu_code -isnot [long]) -or $trial.gpu_code -ne 0 -or $trial.power -ne 'Online' -or !(Test-SwitchRecordId $trial.boot_record)) { throw 'The measured desktop must have Code 0 on a one-time AC boot' }
    foreach ($name in @('tegragpurender.sys','TegraGpuD3DVk.dll','libvulkan_nouveau.dll','tg_d3d_arm64.dll','tg_nvk_arm64.dll')) {
        if (!$trial.loaded -or $trial.loaded.PSObject.Properties[$name].Value -ne $Candidate.PSObject.Properties[$name].Value) { throw "Loaded graphics component not confirmed: $name" }
    }
    $sys = $Evidence.system_log
    if (!$sys -or $sys.complete -isnot [bool] -or !$sys.complete -or !(Test-SwitchRecordId $sys.first_record) -or !(Test-SwitchRecordId $sys.last_record) -or $sys.last_record -lt $sys.first_record -or $sys.restarts -isnot [array]) { throw 'Missing or incomplete system restart interval' }
    if ($trial.boot_record -lt $sys.first_record -or $trial.boot_record -gt $sys.last_record) { throw 'The verified boot is outside the system interval' }
    if ($sys.boot_records -isnot [array] -or $trial.boot_record -notin $sys.boot_records -or $sys.abnormal_events -isnot [array]) { throw 'Missing boot and abnormal-shutdown inventory' }
    if ($sys.boot_records.Count -ne $sys.restarts.Count -or @($sys.boot_records|Sort-Object -Unique).Count -ne $sys.boot_records.Count) { throw 'Every boot needs one explanation' }
    foreach ($boot in $sys.boot_records) {
        if (!(Test-SwitchRecordId $boot) -or @($sys.restarts|Where-Object boot_record -eq $boot).Count -ne 1) { throw "Unexplained boot $boot" }
    }
    foreach ($restart in $sys.restarts) {
        if ($restart.explained -isnot [bool] -or !$restart.explained -or [string]::IsNullOrWhiteSpace($restart.cause) -or !(Test-SwitchRecordId $restart.boot_record) -or $restart.boot_record -lt $sys.first_record -or $restart.boot_record -gt $sys.last_record) { throw 'An unexplained or unbounded restart blocks promotion' }
        if ($restart.kind -cnotin @('controlled','diagnosed') -or $restart.system_records -isnot [array] -or !$restart.system_records.Count) { throw 'A restart needs corroborating system records' }
        if($restart.boot_record -eq $trial.boot_record -and $restart.kind -cne 'controlled'){throw 'The candidate qualification must use a clean controlled trial boot'}
        if($restart.kind -ceq 'diagnosed'){
            $resolved=@($Ledger.faults|Where-Object {$_.id -ceq $restart.fault_id -and $_.status -ceq 'closed'})
            if($resolved.Count -ne 1){throw 'A diagnosed restart needs its closed causal fault record'}
        }
        Assert-SwitchHash $restart.evidence_sha256
    }
    foreach ($event in $sys.abnormal_events) {
        $match=@($sys.restarts|Where-Object {$_.kind -ceq 'diagnosed' -and $_.boot_record -eq $event.boot_record -and $event.record -in $_.system_records})
        if ($event.id -notin @(41,6008) -or $match.Count -ne 1) { throw 'Unexplained abnormal-shutdown evidence' }
    }
    if ($Evidence.cases -isnot [array] -or !$Evidence.cases.Count) { throw 'Missing independent hardware test results' }
    foreach ($test in $Evidence.cases) { Assert-SwitchPassingCase $test }
    if($Evidence.change_kind -ceq 'presentation'){
        foreach($test in $Evidence.cases){
            foreach($field in @('output_verified','normal_exit','desktop_available')){
                if($test.$field -isnot [bool] -or !$test.$field){throw "Presentation qualification lacks $field"}
            }
        }
    }
    if ($trial.boot_record -notin @($Evidence.cases.before_boot)) { throw 'The selected one-time desktop was not tested' }
    $open = @($Ledger.faults | Where-Object status -ceq 'open')
    if (!$open.Count) { return }
    $ids = $open.id -join ', '
    $presentationFaults=@($open|Where-Object {
        'presentation' -in $_.affected_scopes -or 'wddm' -in $_.affected_scopes -or 'gpu-synchronization' -in $_.affected_scopes
    })
    if ($Evidence.change_kind -ceq 'presentation' -and $presentationFaults.Count) {
        throw ('Open faults block presentation promotion: '+($presentationFaults.id -join ', '))
    }
    foreach($file in $Evidence.changed_paths){
        if ($file -like 'tegra-x1-wddm/docs/*') { continue }
        if ($Evidence.change_kind -ceq 'independent' -and $file -match '(^|/)(tgd3d_(dxgi[^/]*|entry|cmd|runtime_queue)\.cpp|present\.c|panel_(scanout|vidpn)\.c|submit(_ddi)?\.c|adapter_reset\.c|reset_backend\.c|[^/]*swapchain[^/]*|[^/]*presenter[^/]*)$|(^|/)wsi/') { throw "Presentation source cannot use an independent qualification: $file" }
    }
    $approval = @($Ledger.independent_qualifications | Where-Object evidence_sha256 -eq $EvidenceSha256)
    if ($approval.Count -ne 1) { throw "Open faults require a ledger-bound independent qualification: $ids" }
    foreach ($fault in $open) {
        $basis = @($approval[0].independence | Where-Object fault_id -ceq $fault.id)
        if ($basis.Count -ne 1 -or $basis[0].test_sha256 -notin @($Evidence.cases.evidence_sha256)) { throw "Missing independently tested separation from $($fault.id)" }
    }
}

function Read-SwitchEvidence([string]$Path) {
    if ([string]::IsNullOrWhiteSpace($Path) -or ![IO.File]::Exists($Path)) { throw 'New baseline promotion requires a hardware qualification file' }
    # Hash and parse the very same bytes for the exception in the fault ledger.
    $bytes = [IO.File]::ReadAllBytes($Path)
    $sha = [Security.Cryptography.SHA256]::Create()
    try { $hash = [BitConverter]::ToString($sha.ComputeHash($bytes)).Replace('-','').ToLowerInvariant() } finally { $sha.Dispose() }
    $text = [Text.Encoding]::UTF8.GetString($bytes).TrimStart([char]0xfeff)
    $data = $text | ConvertFrom-Json -ErrorAction Stop
    return [pscustomobject]@{ Data=$data; Sha256=$hash }
}

function Assert-SwitchCaseFiles($Evidence,[string]$Qualification) {
    $parent=[IO.Path]::GetDirectoryName([IO.Path]::GetFullPath($Qualification))
    foreach($record in @($Evidence.cases)+@($Evidence.system_log.restarts)){
        if ($record.evidence_file -notmatch '^[a-z0-9][a-z0-9_./-]*$' -or $record.evidence_file.Split('/') -contains '..') { throw 'A qualification record needs a relative evidence file' }
        $path=Join-Path $parent $record.evidence_file
        if (![IO.File]::Exists($path) -or (Get-FileHash -LiteralPath $path).Hash -ne $record.evidence_sha256) { throw "Missing or mismatched hardware evidence: $($record.evidence_file)" }
    }
}
