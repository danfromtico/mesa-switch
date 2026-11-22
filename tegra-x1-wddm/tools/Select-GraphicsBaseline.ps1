# SPDX-License-Identifier: MIT
[CmdletBinding()]
param([Parameter(Mandatory)][ValidateSet('CheckLedger','Check','Select','CheckPublication','Publish')][string]$Action,
      [string]$Qualification,[string]$RepositoryRoot)
$ErrorActionPreference='Stop'
$root=(Get-Item (Join-Path $PSScriptRoot '../..')).FullName
if($RepositoryRoot){
    if($Action -notin @('CheckLedger','CheckPublication')){throw 'External snapshots can only be checked read-only'}
    $root=(Get-Item -LiteralPath $RepositoryRoot -ErrorAction Stop).FullName
}
$selectionLock=$null
try {
if($Action -in @('Select','Publish')){
    $common=& git -C $root rev-parse --path-format=absolute --git-common-dir
    if($LASTEXITCODE){throw 'Cannot locate the Git baseline selection lock'}
    $selectionLock=[IO.File]::Open((Join-Path $common 'switch-graphics-baseline.lock'),[IO.FileMode]::OpenOrCreate,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None)
}
$docs=Join-Path $root 'tegra-x1-wddm/docs'
. (Join-Path $PSScriptRoot 'tgd3d-default/Baseline-Promotion.ps1')
$ledgerRel='tegra-x1-wddm/docs/open-faults.json'
$ledgerFile=Join-Path $root $ledgerRel
$policyBytes=Read-SwitchEvidence $ledgerFile
$ledger=$policyBytes.Data
Assert-SwitchFaultLedger $ledger
$old=$null
$revisions=@(& git -C $root log --first-parent --reverse --format=%H HEAD -- $ledgerRel)
if($LASTEXITCODE){throw 'Cannot validate the history of open faults'}
foreach($rev in $revisions){
    $lines=@(& git -C $root show ("${rev}:"+$ledgerRel))
    if($LASTEXITCODE){throw 'A previously tracked fault ledger was removed'}
    $record=($lines -join "`n")|ConvertFrom-Json -ErrorAction Stop
    if($old){Assert-SwitchFaultHistory $old $record}else{Assert-SwitchFaultLedger $record}
    $old=$record
}
if($old){Assert-SwitchFaultHistory $old $ledger}
$baselineRel='tegra-x1-wddm/docs/graphics-baseline.json'
$comparisons=@()
if($ledger.PSObject.Properties['baseline_comparisons']){$comparisons=@($ledger.baseline_comparisons)}
foreach($comparison in $comparisons){
    $before=$comparison.before_commit
    $distance=& git -C $root rev-list --count --first-parent "$before..HEAD"
    if($LASTEXITCODE){throw 'Cannot inspect historical baseline lineage'}
    $ancestor=& git -C $root rev-parse --verify "HEAD~$distance"
    if($LASTEXITCODE -or $ancestor -ne $before){throw 'Historical comparison must come from the first-parent baseline history'}
    $priorLines=@(& git -C $root show ("${before}:"+$baselineRel))
    if($LASTEXITCODE){throw 'Missing historical baseline for comparison'}
    $priorSelection=($priorLines -join "`n")|ConvertFrom-Json -ErrorAction Stop
    Assert-SwitchAcceptedBaseline $priorSelection
    if($priorSelection.qualification_sha256 -ne $comparison.previous_qualification_sha256){throw 'Historical comparison used a different selected baseline'}
    $priorPath=$priorSelection.qualification
    if($priorPath -notmatch '^tegra-x1-wddm/docs/[a-z0-9_./-]+\.json$' -or $priorPath.Split('/') -contains '..'){throw 'Invalid historical qualification path'}
    $previousReport=Read-SwitchEvidence (Join-Path $root $priorPath)
    if($previousReport.Sha256 -ne $comparison.previous_qualification_sha256){throw 'Historical qualification bytes changed'}
    $anchor=@($previousReport.Data.cases|Where-Object evidence_sha256 -eq $comparison.previous_control_sha256)
    if($anchor.Count -ne 1){throw 'Missing historical comparison control'}
    $control=$anchor[0].evidence_file
    if($control -notmatch '^[a-z0-9][a-z0-9_./-]*$' -or $control.Split('/') -contains '..'){throw 'Invalid historical control path'}
    $controlFile=Join-Path (Split-Path (Join-Path $root $priorPath)) $control
    if(!(Test-Path -LiteralPath $controlFile -PathType Leaf) -or (Get-FileHash -LiteralPath $controlFile).Hash -ne $comparison.previous_control_sha256){throw 'Historical control bytes changed'}
    $priorLines=@(& git -C $root show ("${before}:"+$ledgerRel))
    if($LASTEXITCODE){throw 'Missing historical fault ledger for comparison'}
    $priorLedger=($priorLines -join "`n")|ConvertFrom-Json -ErrorAction Stop
    $known=@($priorLedger.faults|Where-Object status -ceq 'open'|ForEach-Object { $_.id })
    foreach($id in $comparison.inherited_faults){if($id -cnotin $known){throw "Fault was not open on the previous baseline: $id"}}
}
foreach($fault in $ledger.faults){
    foreach($reference in $fault.evidence){
        $parts=$reference -split '#',2
        if($parts[0] -notmatch '^tegra-x1-wddm/docs/[a-z0-9_./-]+$' -or $parts[0].Split('/') -contains '..'){throw "Invalid fault evidence reference: $reference"}
        $file=Join-Path $root $parts[0]
        if(![IO.File]::Exists($file)){throw "Missing fault evidence: $reference"}
        if($parts.Count -eq 2 -and !(Read-SwitchPolicyJson $file).PSObject.Properties[$parts[1]]){throw "Missing fault evidence section: $reference"}
    }
    if ($fault.PSObject.Properties['verified_fix']) {
        $repair=$fault.verified_fix
        $proofFiles=[ordered]@{causal_evidence_file='causal_evidence_sha256';code_review_file='code_review_sha256';build_file='build_sha256'}
        foreach ($entry in $proofFiles.GetEnumerator()) {
            $path=Join-Path $root $repair.($entry.Key)
            if (![IO.File]::Exists($path) -or (Get-FileHash -LiteralPath $path).Hash -ne $repair.($entry.Value)) {
                throw "Missing or mismatched scoped repair evidence: $($fault.id)"
            }
        }
    }
    if($fault.status -ceq 'closed'){
        $reason=$fault.resolution.causal_evidence_file
        if($reason -notmatch '^tegra-x1-wddm/docs/[a-z0-9_./-]+$' -or $reason.Split('/') -contains '..' -or !(Test-Path -LiteralPath (Join-Path $root $reason) -PathType Leaf) -or (Get-FileHash -LiteralPath (Join-Path $root $reason)).Hash -ne $fault.resolution.causal_evidence_sha256){throw "Missing causal proof for $($fault.id)"}
        & git -C $root cat-file -e ($fault.resolution.commit+'^{commit}')
        if($LASTEXITCODE){throw "Missing source of the claimed fix for $($fault.id)"}
        foreach($run in $fault.resolution.original_scenario_runs){
            if($run.evidence_file -notmatch '^tegra-x1-wddm/docs/[a-z0-9_./-]+$' -or $run.evidence_file.Split('/') -contains '..'){throw 'Closure needs a repository evidence file'}
            $file=Join-Path $root $run.evidence_file
            if(![IO.File]::Exists($file) -or (Get-FileHash -LiteralPath $file).Hash -ne $run.evidence_sha256){throw "Unverified closure evidence for $($fault.id)"}
        }
    }
}
$selectedPath=Join-Path $docs 'graphics-baseline.json'
$selected=Read-SwitchPolicyJson $selectedPath
Assert-SwitchAcceptedBaseline $selected
if ($Action -in @('CheckLedger','CheckPublication')) {
    $bootstrap=[Text.Encoding]::UTF8.GetBytes(([IO.File]::ReadAllText($selectedPath)).Replace("`r`n","`n"))
    $alg=[Security.Cryptography.SHA256]::Create()
    try{$bootstrapHash=[BitConverter]::ToString($alg.ComputeHash($bootstrap)).Replace('-','')}finally{$alg.Dispose()}
    $bootstrapAccepted=$bootstrapHash -eq '58fc53664b0f5f85c13e428b5b43cae5f70b6e62532606417b111699d4f0d1dc'
    $relative=$selected.qualification
    if ($relative -notmatch '^tegra-x1-wddm/docs/[a-z0-9_./-]+\.json$' -or $relative.Split('/') -contains '..') { throw 'Invalid selected qualification path' }
    $qualificationPath=Join-Path $root $relative
    if ($bootstrapAccepted) {
        # The original qualification predates this schema. Preserve its exact accepted Git blob.
        $original=& git -C $root rev-parse --verify ($selected.source_commit+':'+$relative)
        if($LASTEXITCODE){throw 'Original baseline qualification is unavailable'}
        $actual=& git -C $root hash-object --no-filters $qualificationPath
        if($LASTEXITCODE -or $actual -ne $original){throw 'Previously accepted hardware evidence changed'}
    } elseif ($Action -eq 'CheckLedger') {
        $saved=Read-SwitchEvidence $qualificationPath
        if($saved.Sha256 -ne $selected.qualification_sha256 -or $saved.Data.source_commit -ne $selected.source_commit -or
           $saved.Data.directory -ne $selected.directory -or !(Test-SwitchSameProfile $saved.Data.components $selected.components)){
            throw 'Selected hardware qualification differs from the retained baseline'
        }
        Assert-SwitchCaseFiles $saved.Data $qualificationPath
    }
    if ($Action -eq 'CheckLedger') {
        return [pscustomobject]@{ledger_sha256=$policyBytes.Sha256;open_faults=@($ledger.faults|Where-Object status -ceq 'open').Count}
    }
    if($bootstrapAccepted){
        return [pscustomobject]@{action='ValidatePreviouslyAcceptedBaseline';source_commit=$selected.source_commit}
    }
}
$publishing=$Action -in @('Publish','CheckPublication')
if ($publishing) {
    $Qualification=$selected.qualification
}
if ([string]::IsNullOrWhiteSpace($Qualification) -or $Qualification -notmatch '^tegra-x1-wddm/docs/[a-zA-Z0-9_./-]+\.json$' -or $Qualification.Split('/') -contains '..') {
    throw 'Specify a repository-relative hardware qualification in tegra-x1-wddm/docs'
}
$e=Read-SwitchEvidence (Join-Path $root $Qualification)
$activeComparison=@($comparisons|Where-Object evidence_sha256 -eq $e.Sha256)
if($activeComparison.Count){
    $before=$activeComparison[0].before_commit
    & git -C $root merge-base --is-ancestor $e.Data.base_commit $before
    if($LASTEXITCODE){throw 'Comparison snapshot predates the selected baseline source'}
    & git -C $root merge-base --is-ancestor $e.Data.source_commit $before
    if($LASTEXITCODE -ne 1){throw 'Comparison snapshot must exclude the candidate source'}
    $beforeTime=& git -C $root show -s --format=%ct $before
    if($LASTEXITCODE){throw 'Missing comparison snapshot'}
    $sourceTime=& git -C $root show -s --format=%ct $e.Data.source_commit
    if($LASTEXITCODE -or [long]$beforeTime -ge [long]$sourceTime){throw 'The baseline fault snapshot must precede the candidate source'}
}
if ($publishing) {
    if ($selected.source_commit -ne $e.Data.source_commit -or $selected.qualification_sha256 -ne $e.Sha256) { throw 'Selected source or qualification digest differs' }
    $profile=$selected.components
    $directory=$selected.directory
} else {
    $profile=$e.Data.components
    $directory=$e.Data.directory
}
$head=& git -C $root rev-parse HEAD
if($LASTEXITCODE){throw 'Cannot identify the baseline source checkout'}
$priorBaseline=$selected
if($publishing){
    $selectedCommit=& git -C $root log -1 --first-parent --format=%H HEAD -- $baselineRel
    if($LASTEXITCODE -or !$selectedCommit){throw 'Cannot identify the committed baseline transition'}
    $parent=& git -C $root rev-parse --verify ($selectedCommit+'^')
    if($LASTEXITCODE){throw 'No previous baseline history for this selection'}
    $lines=@(& git -C $root show ("${parent}:"+$baselineRel))
    if($LASTEXITCODE){throw 'Previous accepted baseline manifest is unavailable'}
    $priorBaseline=($lines -join "`n")|ConvertFrom-Json -ErrorAction Stop
    Assert-SwitchAcceptedBaseline $priorBaseline
}
if($priorBaseline.source_commit -ne $e.Data.base_commit){throw 'Candidate evidence was collected relative to a different accepted source'}
if($priorBaseline.qualification -notmatch '^tegra-x1-wddm/docs/[a-z0-9_./-]+\.json$' -or $priorBaseline.qualification.Split('/') -contains '..'){throw 'Invalid prior qualification path'}
Assert-SwitchPromotion $ledger $e.Data $e.Sha256 $profile $directory $priorBaseline (Join-Path $root $priorBaseline.qualification)
Assert-SwitchChangedComponentLoads $priorBaseline.components $profile $e.Data
Assert-SwitchCaseFiles $e.Data (Join-Path $root $Qualification)
& git -C $root merge-base --is-ancestor $e.Data.base_commit $e.Data.source_commit
if($LASTEXITCODE){throw 'Invalid or unrelated source range'}
& git -C $root merge-base --is-ancestor $e.Data.source_commit $head
if($LASTEXITCODE){throw 'The tested source is absent from the selection checkout'}
$pathsChanged=@(& git -C $root diff --no-renames --name-only $e.Data.base_commit $e.Data.source_commit --)
if($LASTEXITCODE -or !$pathsChanged.Count -or (Compare-Object @($pathsChanged|Sort-Object -Unique) @($e.Data.changed_paths|Sort-Object -Unique))){throw 'Claimed files differ from the tested Git source range'}
if((Get-FileHash -LiteralPath $ledgerFile).Hash -ne $policyBytes.Sha256){throw 'Open-fault ledger changed during verification'}
if ($Action -eq 'Select') {
    $next=[ordered]@{schema=1;source_commit=$e.Data.source_commit;directory=$directory;components=$profile;qualification=$Qualification;qualification_sha256=$e.Sha256}
    $temporary=$selectedPath+'.'+[guid]::NewGuid().ToString('N')+'.tmp'
    try {
        [IO.File]::WriteAllText($temporary,($next|ConvertTo-Json -Depth 8)+[Environment]::NewLine,[Text.UTF8Encoding]::new($false))
        $backup=$temporary+'.bak'
        [IO.File]::Replace($temporary,$selectedPath,$backup)
        [IO.File]::Delete($backup)
    } finally {
        if ([IO.File]::Exists($temporary)) { [IO.File]::Delete($temporary) }
        if ($backup -and [IO.File]::Exists($backup)) { [IO.File]::Delete($backup) }
    }
}
if ($Action -eq 'Publish') {
    # A normal evidence-only push does not select a graphics baseline.
    $branch=& git -C $root branch --show-current
    if ($LASTEXITCODE -or $branch -ne 'tgd3d-vulkan') { throw 'Publish from the tgd3d-vulkan branch' }
    $paths=@('tegra-x1-wddm/docs/open-faults.json','tegra-x1-wddm/docs/graphics-baseline.json',$Qualification,
             'tegra-x1-wddm/tools/Select-GraphicsBaseline.ps1','tegra-x1-wddm/tools/tgd3d-default/Baseline-Promotion.ps1')
    $prefix=$Qualification.Substring(0,$Qualification.LastIndexOf('/')+1)
    $paths+=@(@($e.Data.cases)+@($e.Data.system_log.restarts)|ForEach-Object {$prefix+$_.evidence_file})
    if($e.Data.PSObject.Properties['baseline_cases']){
        $paths+=@($e.Data.baseline_cases | ForEach-Object {$prefix+$_.evidence_file})
    }
    if($e.Data.change_kind -ceq 'verified-fix' -and $e.Data.verified_fix.PSObject.Properties['scope']){
        $paths+=@($e.Data.verified_fix.evidence | ForEach-Object {$prefix+$_.evidence_file})
    }
    if($activeComparison.Count){
        $paths+=@($priorBaseline.qualification)
        $prior=Read-SwitchPolicyJson (Join-Path $root $priorBaseline.qualification)
        $control=@($prior.cases|Where-Object evidence_sha256 -eq $activeComparison[0].previous_control_sha256)
        $paths+=@($priorBaseline.qualification.Substring(0,$priorBaseline.qualification.LastIndexOf('/')+1)+$control[0].evidence_file)
    }
    foreach($path in $paths) {
        $expected=& git -C $root rev-parse --verify ("$head"+':'+$path)
        if ($LASTEXITCODE) { throw "Policy or evidence is absent from the publication commit: $path" }
        $actual=& git -C $root hash-object --path=$path (Join-Path $root $path)
        if ($LASTEXITCODE -or $expected -ne $actual) { throw "Working policy or evidence differs from the publication commit: $path" }
    }
    if((Get-FileHash -LiteralPath $ledgerFile).Hash -ne $policyBytes.Sha256 -or (Get-FileHash -LiteralPath (Join-Path $root $Qualification)).Hash -ne $e.Sha256){throw 'Checked evidence changed before publication'}
    & git -C $root push origin ("$head"+':refs/heads/tgd3d-vulkan')
    if ($LASTEXITCODE) { throw 'Baseline publication failed' }
}
[pscustomobject]@{action=$Action;source_commit=$e.Data.source_commit;qualification_sha256=$e.Sha256;ledger_sha256=$policyBytes.Sha256}
}finally{if($selectionLock){$selectionLock.Dispose()}}
