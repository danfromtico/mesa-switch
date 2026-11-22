# SPDX-License-Identifier: MIT
param([string]$PolicyDirectory=(Join-Path $PSScriptRoot '..'),
      [string]$DocsDirectory=(Join-Path $PSScriptRoot '../../../docs'))
$ErrorActionPreference='Stop'
. (Join-Path $PolicyDirectory 'Baseline-Promotion.ps1')
$ledger=Read-SwitchPolicyJson (Join-Path $DocsDirectory 'open-faults.json')
$base=Read-SwitchPolicyJson (Join-Path $DocsDirectory 'graphics-baseline.json')
function Clone($Object){return ($Object|ConvertTo-Json -Depth 12|ConvertFrom-Json)}
function Block([string]$Name,[scriptblock]$Test,[string]$Pattern){
    $caught=$null
    try{& $Test}catch{$caught=$_.Exception.Message}
    if(!$caught -or $caught -notmatch $Pattern){throw "Failed $Name, observed '$caught'"}
    Write-Output "PASS $Name"
}
Assert-SwitchFaultLedger $ledger
Assert-SwitchAcceptedBaseline $base
$empty=[pscustomobject]@{schema=1;faults=@();independent_qualifications=@()}
$sampleFault=[pscustomobject]@{id='TEST-FIRST-PRESENT';kind='restart';status='open';affected_scopes=@('presentation');trigger='first Present';
    cause='Unknown';evidence=@('qualified-case.json');daily_workaround='Use the verified path';close_when='Three original runs';resolution=$null}
$open=[pscustomobject]@{schema=1;faults=@($sampleFault);independent_qualifications=@()}
$candidate=Clone $base.components
$candidate.'tg_d3d_arm64.dll'='a'*64
if(Test-SwitchSameProfile $candidate $base.components){throw 'A changed leaf matched the accepted dispatcher'}
$directory='C:\ProgramData\SwitchWOA\GpuUpdate\candidate-contract'
$proof='b'*64
$case=[pscustomobject]@{name='independent hardware case';passed=$true;before_boot=110;after_boot=110;evidence_sha256='c'*64;evidence_file='case.json';output_verified=$true;normal_exit=$true;desktop_available=$true}
$boot=[pscustomobject]@{kind='controlled';explained=$true;cause='Scheduled one-time boot';boot_record=110;system_records=@(102,110);evidence_sha256='d'*64;evidence_file='boot.json'}
$e=[pscustomobject]@{schema=1;change_kind='presentation';source_commit='1'*40;base_commit='2'*40;changed_paths=@('src/nouveau/tgd3d/tgd3d_shader.cpp');directory=$directory;components=$candidate;
    trial=[pscustomobject]@{one_time=$true;power='Online';gpu_code=0;boot_record=110;loaded=$candidate};
    system_log=[pscustomobject]@{complete=$true;first_record=100;last_record=190;boot_records=@(110);abnormal_events=@();restarts=@($boot)};
    cases=@($case)}
Assert-SwitchPromotion $empty $e $proof $candidate $directory
Write-Output 'PASS no open faults with tested native candidate'
Block 'presentation blocked with Code 0' {Assert-SwitchPromotion $open $e $proof $candidate $directory} 'Open faults block'
Block 'missing ledger' {Read-SwitchPolicyJson (Join-Path $DocsDirectory 'intentionally-absent-open-faults.json')} 'missing'
Block 'malformed empty ledger' {Assert-SwitchFaultLedger ([pscustomobject]@{schema=1})} 'schema'
$bad=Clone $empty;$bad.faults=@($sampleFault,$sampleFault)
Block 'duplicate fault identifier' {Assert-SwitchFaultLedger $bad} 'duplicate'
Block 'fault deletion' {Assert-SwitchFaultHistory $open $empty} 'disappeared'
$bad=Clone $open;$bad.faults[0].status='closed'
Block 'closure without causal evidence' {Assert-SwitchFaultLedger $bad} 'Closure'
$bad=Clone $e;$bad.trial.one_time=$false
Block 'permanent boot substituted for trial' {Assert-SwitchPromotion $empty $bad $proof $candidate $directory} 'one-time'
$bad=Clone $e;$bad.trial.loaded.'tg_d3d_arm64.dll'='f'*64
Block 'incorrect loaded UMD' {Assert-SwitchPromotion $empty $bad $proof $candidate $directory} 'Loaded graphics'
$bad=Clone $e;$bad.system_log.restarts[0].explained=$false
Block 'unexplained restart' {Assert-SwitchPromotion $empty $bad $proof $candidate $directory} 'unexplained'
$bad=Clone $e;$bad.system_log.abnormal_events=@([pscustomobject]@{id=41;record=111;boot_record=110})
Block 'unexpected event hidden behind planned restart' {Assert-SwitchPromotion $empty $bad $proof $candidate $directory} 'abnormal'
$bad=Clone $e;$bad.cases[0].after_boot=155
Block 'interrupted case marked passed' {Assert-SwitchPromotion $empty $bad $proof $candidate $directory} 'crossed a boot'
$bad=Clone $e;$bad.cases[0].before_boot=$true;$bad.cases[0].after_boot=$true
Block 'boolean boot record' {Assert-SwitchPromotion $empty $bad $proof $candidate $directory} 'boot evidence'
$bad=Clone $e;$bad.system_log.restarts[0].kind='diagnosed'
Block 'candidate boot camouflaged as a diagnosed restart' {Assert-SwitchPromotion $empty $bad $proof $candidate $directory} 'clean controlled'
Block 'matching dispatcher with changed candidate bytes' {Assert-SwitchPromotion $empty $e $proof $base.components $directory} 'artifacts differ'
$independent=Clone $e;$independent.change_kind='independent'
Block 'self-declared bypass' {Assert-SwitchPromotion $open $independent $proof $candidate $directory} 'ledger-bound'
$approved=Clone $open
$approved.independent_qualifications=@([pscustomobject]@{evidence_sha256=$proof;independence=@([pscustomobject]@{fault_id=$sampleFault.id;reason='Separate tested component and transport';test_sha256=$case.evidence_sha256})})
Assert-SwitchPromotion $approved $independent $proof $candidate $directory
Write-Output 'PASS independently qualified bypass with exact evidence'
$wifi=Clone $approved
$wifi.faults[0].affected_scopes=@('wifi')
Assert-SwitchPromotion $wifi $e $proof $candidate $directory
Write-Output 'PASS qualified presentation with a separately accounted Wi-Fi fault'
$mixed=Clone $candidate
$mixed.'tg_d3d_x64.dll'='6'*64
Block 'an unchanged DWM cannot qualify a changed x64 leaf' {Assert-SwitchChangedComponentLoads $base.components $mixed $e} 'No test'
$appCase=Clone $e
$appCase.cases[0]|Add-Member -NotePropertyName loaded_components -NotePropertyValue ([pscustomobject]@{'tg_d3d_x64.dll'=$mixed.'tg_d3d_x64.dll'})
Assert-SwitchChangedComponentLoads $base.components $mixed $appCase
Write-Output 'PASS changed x64 leaf measured in the candidate boot application'
$bad=Clone $independent;$bad.changed_paths=@('src/nouveau/tgd3d/tgd3d_dxgi.cpp')
Block 'presentation change labeled independent' {Assert-SwitchPromotion $approved $bad $proof $candidate $directory} 'Presentation source'
Block 'different evidence reuses an exception' {Assert-SwitchPromotion $approved $independent ('e'*64) $candidate $directory} 'ledger-bound'
$second=Clone $sampleFault;$second.id='TEST-ANOTHER-FAULT';$more=Clone $approved;$more.faults=@($sampleFault,$second)
Block 'new open fault invalidates an older independent qualification' {Assert-SwitchPromotion $more $independent $proof $candidate $directory} 'Missing independently'
$closed=Clone $open;$closed.faults[0].status='closed'
$runs=@(foreach($ch in @('3','4','5')){
    [pscustomobject]@{name='original scenario';passed=$true;before_boot=111;after_boot=111;original_trigger=$true;output_verified=$true;
        normal_exit=$true;desktop_available=$true;evidence_sha256=$ch*64}
})
$closed.faults[0].resolution=[pscustomobject]@{root_cause='Identified and repaired cause';root_cause_confirmed=$true;causal_evidence_sha256='1'*64;commit='2'*40;components=$candidate;original_scenario_runs=$runs}
Assert-SwitchFaultHistory $open $closed
Assert-SwitchPromotion $closed $e $proof $candidate $directory
Write-Output 'PASS closed fault with three separate original-scenario reports'
$bad=Clone $closed;$bad.faults[0].resolution.original_scenario_runs[1].evidence_sha256=$runs[0].evidence_sha256
Block 'duplicate closure runs' {Assert-SwitchFaultLedger $bad} 'Duplicate closure'
$verified=Clone $independent
$verified.change_kind='verified-fix'
$verified.changed_paths=@('src/nouveau/tgd3d/tgd3d_dxgi.cpp')
$repair=Clone $closed.faults[0]
$repair.id='TEST-ENCODED-COPY'
$repair.resolution.commit=$verified.source_commit
foreach($run in $repair.resolution.original_scenario_runs){$run.before_boot=$case.before_boot;$run.after_boot=$case.before_boot}
$verified.cases=@($case)+@($repair.resolution.original_scenario_runs)
$verified|Add-Member -NotePropertyName verified_fix -NotePropertyValue ([pscustomobject]@{
    fault_id=$repair.id;causal_evidence_sha256=$repair.resolution.causal_evidence_sha256;control_sha256=$case.evidence_sha256})
$registered=Clone $approved
$registered.faults=@($sampleFault,$repair)
Assert-SwitchPromotion $registered $verified $proof $candidate $directory
Write-Output 'PASS a verified fix in a presentation file with its original retests and an independent working control'
$bad=Clone $verified;$bad.verified_fix.fault_id=$sampleFault.id
Block 'open causal fault cannot authorize a verified fix' {Assert-SwitchPromotion $registered $bad $proof $candidate $directory} 'closed causal'
$bad=Clone $verified;$bad.source_commit='f'*40
Block 'different source cannot reuse a closure' {Assert-SwitchPromotion $registered $bad $proof $candidate $directory} 'source, cause'
$bad=Clone $verified;$bad.cases=@($case)
Block 'a closed label cannot replace original retests' {Assert-SwitchPromotion $registered $bad $proof $candidate $directory} 'original retests'
$otherBoot=Clone $registered
$otherBoot.faults[1].resolution.original_scenario_runs[0].before_boot=90
$otherBoot.faults[1].resolution.original_scenario_runs[0].after_boot=90
Block 'same bytes cannot be assigned to a different trial boot' {Assert-SwitchPromotion $otherBoot $verified $proof $candidate $directory} 'original retests'
$bad=Clone $verified;$bad.verified_fix.causal_evidence_sha256='7'*64
Block 'wrong causal proof' {Assert-SwitchPromotion $registered $bad $proof $candidate $directory} 'source, cause'
$bad=Clone $verified;$bad.verified_fix.control_sha256='8'*64
Block 'missing working-path control' {Assert-SwitchPromotion $registered $bad $proof $candidate $directory} 'distinct working-path'

$bad=Clone $verified;$bad.verified_fix.control_sha256=$repair.resolution.original_scenario_runs[0].evidence_sha256
Block 'a repair run cannot also prove the working control' {Assert-SwitchPromotion $registered $bad $proof $candidate $directory} 'distinct working-path'
Block 'a verified fix still needs independent evidence for every open fault' {Assert-SwitchPromotion $registered $verified ('e'*64) $candidate $directory} 'ledger-bound'
$limited=Clone $e
$limited.change_kind='verified-fix'
$limited.changed_paths=@('src/nouveau/tgd3d/tgd3d_dxgi.cpp')
$limited.cases[0].output_verified=$false
$limited.cases[0]|Add-Member -NotePropertyName scenario -NotePropertyValue 'observed input word'
$limited.cases[0]|Add-Member -NotePropertyName original_trigger -NotePropertyValue $true
$limited.cases[0]|Add-Member -NotePropertyName loaded_components -NotePropertyValue $candidate
$limited|Add-Member -NotePropertyName verified_fix -NotePropertyValue ([pscustomobject]@{
    fault_id='TEST-INPUT-WORD';scope='observed input word';output_required=$false;case_sha256=$case.evidence_sha256;
    causal_evidence_sha256='1'*64;code_review_sha256='2'*64;build_sha256='3'*64;
    evidence=@([pscustomobject]@{evidence_file='cause.json';evidence_sha256='1'*64},
        [pscustomobject]@{evidence_file='review.json';evidence_sha256='2'*64},
        [pscustomobject]@{evidence_file='build.json';evidence_sha256='3'*64})})
$inputFault=Clone $sampleFault
$inputFault.id='TEST-INPUT-WORD'
$inputFault|Add-Member -NotePropertyName verified_fix -NotePropertyValue ([pscustomobject]@{
    scope='observed input word';root_cause='The validated runtime input was rejected';root_cause_confirmed=$true;
    independent_review_passed=$true;build_signed=$true;commit=$limited.source_commit;components=$candidate;
    component='tg_d3d_arm64.dll';output_required=$false;
    causal_evidence_sha256='1'*64;code_review_sha256='2'*64;build_sha256='3'*64;
    causal_evidence_file='tegra-x1-wddm/docs/cause.json';code_review_file='tegra-x1-wddm/docs/review.json';build_file='tegra-x1-wddm/docs/build.json'})
$limitedLedger=Clone $approved
$limitedLedger.faults+=@($inputFault)
$limitedLedger.independent_qualifications[0].independence+=@([pscustomobject]@{
    fault_id=$inputFault.id;reason='The native case covers the registered input repair';test_sha256=$case.evidence_sha256})
Assert-SwitchPromotion $limitedLedger $limited $proof $candidate $directory
if($limitedLedger.faults[1].status -cne 'open' -or $limitedLedger.faults[1].resolution){throw 'Scoped promotion changed fault closure'}
Write-Output 'PASS registered input repair with one matching native case and retained open fault'
$bad=Clone $limitedLedger;$bad.faults[1].verified_fix.root_cause_confirmed=$false
Block 'unconfirmed cause cannot authorize a scoped repair' {Assert-SwitchPromotion $bad $limited $proof $candidate $directory} 'confirmed cause'
$bad=Clone $limited;$bad.source_commit='f'*40
Block 'scoped repair requires the registered source' {Assert-SwitchPromotion $limitedLedger $bad $proof $candidate $directory} 'source or components'
$bad=Clone $limited;$bad.verified_fix.code_review_sha256='4'*64
Block 'scoped repair requires the registered independent review' {Assert-SwitchPromotion $limitedLedger $bad $proof $candidate $directory} 'review or build evidence'
$bad=Clone $limited;$bad.cases[0].scenario='different input'
Block 'another input cannot qualify a scoped repair' {Assert-SwitchPromotion $limitedLedger $bad $proof $candidate $directory} 'matching original input'
$bad=Clone $limited;$bad.cases[0].before_boot=90;$bad.cases[0].after_boot=90
Block 'old receipt cannot replace the candidate test' {Assert-SwitchPromotion $limitedLedger $bad $proof $candidate $directory} 'matching original input'
$bad=Clone $limited;$bad.cases[0].loaded_components.'tg_d3d_arm64.dll'='5'*64
Block 'scoped repair case must load the repaired DLL' {Assert-SwitchPromotion $limitedLedger $bad $proof $candidate $directory} 'did not load'
$bad=Clone $limited;$bad.system_log.abnormal_events=@([pscustomobject]@{id=41;record=111;boot_record=110})
Block 'scoped repair cannot hide an abnormal shutdown' {Assert-SwitchPromotion $limitedLedger $bad $proof $candidate $directory} 'abnormal'
$bad=Clone $limitedLedger;$bad.independent_qualifications=@()
Block 'scoped repair still accounts for every open fault' {Assert-SwitchPromotion $bad $limited $proof $candidate $directory} 'ledger-bound'
$bad=Clone $limitedLedger;$bad.faults[1].verified_fix.code_review_file='../review.json'
Block 'scoped repair proof must be inside the repository docs' {Assert-SwitchFaultLedger $bad} 'repository-relative'
$bad=Clone $limitedLedger;$bad.faults[1].status='closed';$bad.faults[1].resolution=Clone $repair.resolution
Block 'closed fault uses its closure qualification' {Assert-SwitchPromotion $bad $limited $proof $candidate $directory} 'registered open'
$bad=Clone $limited;$bad.verified_fix.case_sha256=$bad.verified_fix.code_review_sha256
Block 'static review cannot serve as the native receipt' {Assert-SwitchPromotion $limitedLedger $bad $proof $candidate $directory} 'native case must be distinct'
$visualLedger=Clone $limitedLedger;$visualLedger.faults[1].verified_fix.output_required=$true
$visual=Clone $limited;$visual.verified_fix.output_required=$true
Block 'visual repair requires output evidence' {Assert-SwitchPromotion $visualLedger $visual $proof $candidate $directory} 'output_verified'
$bad=Clone $limited;$bad.verified_fix.output_required=$true
Block 'qualification must preserve the registered output scope' {Assert-SwitchPromotion $limitedLedger $bad $proof $candidate $directory} 'registered scope'
$upper=Clone $limited;$upper.verified_fix.case_sha256=$upper.verified_fix.case_sha256.ToUpperInvariant()
$upper.cases[0].loaded_components.'tg_d3d_arm64.dll'=$upper.cases[0].loaded_components.'tg_d3d_arm64.dll'.ToUpperInvariant()
Assert-SwitchPromotion $limitedLedger $upper $proof $candidate $directory
Write-Output 'PASS Windows uppercase hash receipts'
$scratch=Join-Path ([IO.Path]::GetTempPath()) ('switch-gate-'+[guid]::NewGuid().ToString('N'))
$null=[IO.Directory]::CreateDirectory($scratch)
try{
    $priorFile=Join-Path $scratch 'previous-qualification.json'
    $priorReport=Clone $e
    $priorReport.source_commit=$verified.base_commit
    $priorReport.components=$base.components
    $priorReport.cases[0].evidence_sha256='8'*64
    [IO.File]::WriteAllText($priorFile,($priorReport|ConvertTo-Json -Depth 12))
    $previous=Clone $base
    $previous.source_commit=$verified.base_commit
    $previous.qualification_sha256=(Get-FileHash $priorFile).Hash.ToLowerInvariant()
    $compared=Clone $registered
    $compared.independent_qualifications=@()
    $compared|Add-Member -NotePropertyName baseline_comparisons -NotePropertyValue @([pscustomobject]@{
        evidence_sha256=$proof;before_commit='2'*40;previous_qualification_sha256=$previous.qualification_sha256;
        previous_control_sha256=$priorReport.cases[0].evidence_sha256;inherited_faults=@($sampleFault.id)})
    Assert-SwitchPromotion $compared $verified $proof $candidate $directory $previous $priorFile
    Write-Output 'PASS verified driver fix with a previously working control and an inherited open fault'
    $mixedScope=Clone $limitedLedger
    $mixedScope.independent_qualifications=@()
    $mixedScope|Add-Member -NotePropertyName baseline_comparisons -NotePropertyValue @((Clone $compared.baseline_comparisons[0]))
    $mixedScope.baseline_comparisons[0].inherited_faults=@($mixedScope.faults.id)
    Block 'scoped repair cannot enter baseline comparison accounting' {Assert-SwitchPromotion $mixedScope $limited $proof $candidate $directory $previous $priorFile} 'independent_qualifications'
    $pair=Clone $case
    $pair.before_boot=90
    $pair.after_boot=90
    $pair.evidence_sha256='6'*64
    $pair.evidence_file='baseline-case.json'
    $pair|Add-Member -NotePropertyMembers @{source_commit=$previous.source_commit;components=$previous.components;scenario='direct DIB'}
    $scoped=Clone $e
    $scoped|Add-Member -NotePropertyName baseline_cases -NotePropertyValue @($pair)
    $scoped.cases[0]|Add-Member -NotePropertyMembers @{
        scenario=$pair.scenario;baseline_case_sha256=$pair.evidence_sha256
        comparison_result='no-regression';comparison_reason='Matched visible output, input and desktop return'}
    $comparisonLedger=Clone $compared
    $comparisonLedger.baseline_comparisons[0]|Add-Member -NotePropertyName fault_assessments -NotePropertyValue @(
        [pscustomobject]@{fault_id=$sampleFault.id;relation='disjoint';reason='The DIB case presents using GDI';test_sha256=$case.evidence_sha256})
    Assert-SwitchPromotion $comparisonLedger $scoped $proof $candidate $directory $previous $priorFile
    Write-Output 'PASS matched presentation comparison with an inherited open fault'
    $bad=Clone $scoped;$bad.baseline_cases[0].components.'tg_nvk_arm64.dll'='0'*64
    Block 'baseline comparison used a different NVK' {Assert-SwitchPromotion $comparisonLedger $bad $proof $candidate $directory $previous $priorFile} 'wrong profile or boot'
    $bad=Clone $scoped;$bad.cases[0].scenario='unrelated scenario'
    Block 'candidate and baseline scenarios differ' {Assert-SwitchPromotion $comparisonLedger $bad $proof $candidate $directory $previous $priorFile} 'does not match'
    $bad=Clone $scoped;$bad.cases[0].comparison_result='regression'
    Block 'candidate comparison regressed' {Assert-SwitchPromotion $comparisonLedger $bad $proof $candidate $directory $previous $priorFile} 'Missing passing comparison'
    $bad=Clone $scoped;$bad.baseline_cases[0].evidence_sha256=$case.evidence_sha256;$bad.cases[0].baseline_case_sha256=$case.evidence_sha256
    Block 'same receipt claimed on both boots' {Assert-SwitchPromotion $comparisonLedger $bad $proof $candidate $directory $previous $priorFile} 'distinct hardware receipt'
    $bad=Clone $comparisonLedger;$bad.baseline_comparisons[0].fault_assessments=@()
    Block 'open fault with no assessment' {Assert-SwitchPromotion $bad $scoped $proof $candidate $directory $previous $priorFile} 'Every inherited fault'
    $bad=Clone $compared;$bad.faults+=@($second)
    Block 'a new fault was not inherited' {Assert-SwitchPromotion $bad $verified $proof $candidate $directory $previous $priorFile} 'New open fault'
    $bad=Clone $compared;$bad.baseline_comparisons[0].previous_control_sha256='9'*64
    Block 'previous control must belong to the accepted qualification' {Assert-SwitchPromotion $bad $verified $proof $candidate $directory $previous $priorFile} 'Missing working control'
    $bad=Clone $previous;$bad.qualification_sha256='9'*64
    Block 'unaccepted previous qualification' {Assert-SwitchPromotion $compared $verified $proof $candidate $directory $bad $priorFile} 'differs from the accepted'
    Block 'presentation without paired hardware evidence cannot inherit faults' {Assert-SwitchPromotion $compared $e $proof $candidate $directory $previous $priorFile} 'Missing matched baseline cases'
    $bad=Clone $verified;$bad.system_log.abnormal_events=@([pscustomobject]@{id=41;record=111;boot_record=110})
    Block 'inherited faults cannot mask a new restart' {Assert-SwitchPromotion $compared $bad $proof $candidate $directory $previous $priorFile} 'abnormal'
    $bad=Clone $verified
    $extra=Clone $boot;$extra.kind='diagnosed';$extra.boot_record=105;$extra.system_records=@(105);$extra|Add-Member -NotePropertyName fault_id -NotePropertyValue $repair.id
    $bad.system_log.boot_records=@(105,110);$bad.system_log.restarts=@($extra,$boot)
    Block 'diagnosed restart still blocks a no-regression comparison' {Assert-SwitchPromotion $compared $bad $proof $candidate $directory $previous $priorFile} 'abnormal candidate interval'
    . (Join-Path $PolicyDirectory 'Baseline-Device.ps1')
    $accepted=Clone $previous
    $accepted.source_commit=$verified.source_commit;$accepted.qualification_sha256=$proof
    $accepted.components=$candidate;$accepted.directory=$directory
    $qualified=[pscustomobject]@{Data=$verified;Sha256=$proof}
    $bookmark=[pscustomobject]@{first_record=200}
    $events=@([pscustomobject]@{Id=1074;ProviderName='User32';RecordId=205},
              [pscustomobject]@{Id=12;ProviderName='Microsoft-Windows-Kernel-General';RecordId=210})
    $actual=Get-SwitchReactivationEvidence $accepted $qualified $bookmark $events 215
    if($actual.trial.boot_record -ne 210 -or $actual.system_log.first_record -ne 200 -or $actual.system_log.last_record -ne 215 -or
       $actual.system_log.abnormal_events.Count -or $actual.system_log.restarts[0].kind -ne 'controlled' -or
       (Compare-Object @(205,210) @($actual.system_log.restarts[0].system_records))){throw 'Wrong later activation interval'}
    $bad=Clone $accepted;$bad.qualification_sha256='a'*64
    Block 'later activation requires the selected qualification' {Get-SwitchReactivationEvidence $bad $qualified $bookmark $events 215} 'exact repository-selected'
    $abnormal=$events+@([pscustomobject]@{Id=6008;ProviderName='EventLog';RecordId=211})
    Block 'later activation cannot accept an abnormal shutdown' {Get-SwitchReactivationEvidence $accepted $qualified $bookmark $abnormal 215} 'controlled restart'
    Block 'later activation cannot use an earlier grant' {Get-SwitchReactivationEvidence $accepted $qualified ([pscustomobject]@{first_record=102}) $events 215} 'Invalid interval'
    Write-Output 'PASS separate hardware qualification and actual reactivation boots'
    [IO.File]::WriteAllText((Join-Path $scratch 'case.json'),'test case record')
    [IO.File]::WriteAllText((Join-Path $scratch 'boot.json'),'test system record')
    $copy=Clone $e
    $copy.cases[0].evidence_sha256=(Get-FileHash (Join-Path $scratch 'case.json')).Hash
    $copy.system_log.restarts[0].evidence_sha256=(Get-FileHash (Join-Path $scratch 'boot.json')).Hash
    $inputFile=Join-Path $scratch 'qualification.json'
    [IO.File]::WriteAllText($inputFile,($copy|ConvertTo-Json -Depth 12))
    $parsed=Read-SwitchEvidence $inputFile
    if($parsed.Sha256 -ne (Get-FileHash $inputFile).Hash){throw 'Qualification hash mismatch'}
    Assert-SwitchCaseFiles $parsed.Data $inputFile
    $scopedFiles=Clone $copy
    $scopedFiles.change_kind='verified-fix'
    $scopedFiles|Add-Member -NotePropertyName verified_fix -NotePropertyValue (Clone $limited.verified_fix)
    foreach($record in $scopedFiles.verified_fix.evidence){
        $path=Join-Path $scratch $record.evidence_file
        [IO.File]::WriteAllText($path,$record.evidence_file)
        $record.evidence_sha256=(Get-FileHash $path).Hash.ToLowerInvariant()
    }
    Assert-SwitchCaseFiles $scopedFiles $inputFile
    [IO.File]::WriteAllText((Join-Path $scratch 'review.json'),'changed independent review')
    Block 'scoped repair review bytes are bound to the qualification' {Assert-SwitchCaseFiles $scopedFiles $inputFile} 'mismatched hardware'
    [IO.File]::WriteAllText((Join-Path $scratch 'case.json'),'different case record')
    Block 'mismatched hardware evidence bytes' {Assert-SwitchCaseFiles $parsed.Data $inputFile} 'mismatched hardware'
    [IO.File]::WriteAllText($inputFile,'{bad')
    Block 'unreadable JSON evidence' {Read-SwitchEvidence $inputFile} '.'
    if($env:OS -eq 'Windows_NT'){
        . (Join-Path $PolicyDirectory 'Baseline-Device.ps1')
        Initialize-SwitchGrantRename
        $sub='Software\SwitchGraphicsPolicyTest-'+[guid]::NewGuid().ToString('N')
        $svc=[Microsoft.Win32.Registry]::CurrentUser.CreateSubKey($sub)
        try{
            Write-SwitchPolicyText (Join-Path $scratch 'last-accepted.json') ($base|ConvertTo-Json -Depth 6)
            $next=Clone $base;$next.directory=$directory;$next.components=$candidate
            $pending=[pscustomobject]@{transaction=[guid]::NewGuid().ToString('N');approved=$next}
            Write-SwitchPolicyText (Join-Path $scratch 'pending-approval.json') ($pending|ConvertTo-Json -Depth 8)
            Complete-SwitchGrantReceipt $scratch $svc
            if((Read-SwitchPolicyJson (Join-Path $scratch 'last-accepted.json')).directory -ne $base.directory){throw 'An uncommitted pending grant approved new bytes'}
            Publish-SwitchPreparedGrant $svc $scratch $next 'test-pinned-images' ('7'*64)
            $key=$svc.OpenSubKey('AllowNativeBoot')
            try{if(!$key -or $key.GetValue('ApprovedDirectory') -ne $directory -or !$key.GetValue('ApprovalTransaction')){throw 'Incomplete named registry grant'}}finally{if($key){$key.Dispose()}}
            Complete-SwitchGrantReceipt $scratch $svc
            Complete-SwitchGrantReceipt $scratch $svc
            if((Test-Path (Join-Path $scratch 'pending-approval.json')) -or !(Test-SwitchSameProfile (Read-SwitchPolicyJson (Join-Path $scratch 'last-accepted.json')).components $candidate)){throw 'Recovery failed to finalize the committed grant'}
            Write-Output 'PASS Windows atomic registry grant and interrupted receipt recovery'
        }finally{$svc.Dispose();[Microsoft.Win32.Registry]::CurrentUser.DeleteSubKeyTree($sub)}
    }
}finally{[IO.Directory]::Delete($scratch,$true)}
