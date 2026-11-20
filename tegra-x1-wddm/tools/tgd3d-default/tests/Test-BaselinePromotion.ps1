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
$scratch=Join-Path ([IO.Path]::GetTempPath()) ('switch-gate-'+[guid]::NewGuid().ToString('N'))
$null=[IO.Directory]::CreateDirectory($scratch)
try{
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
