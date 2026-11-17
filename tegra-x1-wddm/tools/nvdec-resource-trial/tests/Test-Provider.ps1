# SPDX-License-Identifier: MIT
# Actual orchestration with explicit substitutes for every native operation.
param([string]$ProviderSource,[string]$PairSource)
$ErrorActionPreference='Stop'
if(!$ProviderSource){$ProviderSource=Get-Content (Join-Path $PSScriptRoot '..\Provider.ps1') -Raw}
$tokens=$null;$errors=$null
$ast=[Management.Automation.Language.Parser]::ParseInput($ProviderSource,[ref]$tokens,[ref]$errors)
if($errors.Count){throw ($errors|Out-String)}
. ([scriptblock]::Create($ProviderSource))
if(!$PairSource){$PairSource=Get-Content (Join-Path $PSScriptRoot '..\Pair.ps1') -Raw}
$pairAst=[Management.Automation.Language.Parser]::ParseInput($PairSource,[ref]$tokens,[ref]$errors)
if($errors.Count){throw ($errors|Out-String)}
$same=@($pairAst.FindAll({param($n) $n -is [Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq 'Same-Boot'},$true))
if($same.Count -ne 1){throw 'Missing boot comparator'}
. ([scriptblock]::Create($same[0].Extent.Text))
function Defer-ProviderInstall($Directory) {
 $script:Calls++
 if($script:HelperThrows){throw 'MODEL_HELPER_FAILURE'}
 @{Exit=$script:HelperExit;Output=$script:HelperOutput}
}
function Assert($condition,[string]$why){if(!$condition){throw $why}}
function Assert-RetainedFiles {}
function Assert-Power {}
function Assert-Closed {if(!$script:Closed){throw 'MODEL_GATE'}}
function Read-Boot {$script:Boot}
function Device-Property($Name){0}
function Provider-Property($Name){if($Name -eq 'DriverInfPath'){'oem1.inf'}else{0}}
function Join-Path($Path,$Child){$Child}
function Write-ActivationJson($Path,$Value){$script:Saved[$Path]=$Value}
function Save-State($Value){$script:Phases+=@($Value.Phase)}
function Test-Path($Path){$script:Saved.ContainsKey($Path)}
function Read-Json($Path){if(!$script:Saved.ContainsKey($Path)){throw 'MODEL_MISSING_RECEIPT'};$script:Saved[$Path]}
function File-Record($Path){$script:File}
function Assert-File($Path,$Expected){
 if($Path -like '*acpitabl.dat'){Assert ($script:Loaded -eq $Expected) 'MODEL_DISK_TABLE'}
 elseif($Path -like '*\INF\*'){Assert $script:RegistrationValid 'MODEL_INF'}
 else{Assert ($script:File.Sha256 -eq $Expected.Sha256) 'MODEL_FILE'}
}
function Assert-ProviderLoadedImage($Which){Assert $script:LiveMatches 'MODEL_LOADED_IMAGE'}
function Read-LoadedDsdt($Path){$script:Loaded}
function Assert-LoadedDsdt($Actual,$Expected){Assert ($Actual -eq $Expected) 'MODEL_TABLE'}
function Known-Store {@{Directory='store';PublishedInf='oem0.inf'}}
function Find-ProviderStore {if($script:Exists -or $script:Imported){Known-Store}else{$null}}
function pnputil.exe {
 $script:Imports++;Assert ($state.Phase -eq 'StagingProvider') 'Import lacked intent'
 $global:LASTEXITCODE=$script:ImportExit;$script:Imported=($script:ImportExit -eq 0);'MODEL_IMPORT'
}
function shutdown.exe {$script:Restarts++;$global:LASTEXITCODE=0}
function Reset {
 $script:Saved=@{};$script:Phases=@();$script:Exists=$false;$script:Imported=$false
 $script:Imports=0;$script:ImportExit=0;$script:Restarts=0;$script:Closed=$true;$script:Boot='2026-11-17T00:01:00Z'
 $script:File=@{Sha256='old';Length=10};$script:Loaded='new-table'
 $script:Calls=0;$script:HelperExit=0;$script:HelperThrows=$false
 $script:RegistrationValid=$true
 $script:LiveMatches=$true
 $script:HelperOutput='RESULT=STAGED_FOR_REBOOT live driver identity is not verified'
}
foreach($case in @('same','different-hash','different-length','empty','malformed')){
 $a=@{Length=104456;Sha256=('a'*64)};$b=$a.Clone()
 if($case -eq 'different-hash'){$b.Sha256='b'*64}
 if($case -eq 'different-length'){$b.Length++}
 if($case -eq 'empty'){$a=@{};$b=@{}}
 if($case -eq 'malformed'){$a.Sha256=$b.Sha256='short'}
 Assert ((Test-ProviderSameBinary $a $b) -eq ($case -eq 'same')) ('Provider byte identity reuse differs: '+$case)
}
foreach($case in @('corrected','unchanged-tool','running','mismatch','not-ended','not-loss','unknown-exit','missing-tool','wrong-target','other-boot','second-replacement')) {
 $boot='2026-11-17T10:05:30Z';$target=@{Sha256='candidate'};$tool=@{Sha256='new-observer'}
 $prior=@{Boot=$boot;Phase='Failed';Exit=5;Target=$target;Tool=@{Sha256='old-observer'};Stderr="StopTrace=0 events_lost=17 buffers_lost=0 session=NvdecImageIdentity-988`r`n"}
 if($case -eq 'unchanged-tool'){$prior.Tool=$tool}
 if($case -eq 'running'){$prior.Phase='Running'}
 if($case -eq 'mismatch'){$prior.Phase='Mismatch'}
 if($case -eq 'not-ended'){$prior.Stderr='StopTrace=5 events_lost=17 buffers_lost=0 session=NvdecImageIdentity-988'}
 if($case -eq 'not-loss'){$prior.Stderr='StopTrace=0 events_lost=0 buffers_lost=0 session=NvdecImageIdentity-988'}
 if($case -eq 'unknown-exit'){$prior.Exit=$null}
 if($case -eq 'missing-tool'){$prior.Tool=@{}}
 if($case -eq 'wrong-target'){$prior.Target=@{Sha256='other'}}
 if($case -eq 'other-boot'){$prior.Boot='2026-11-17T10:00:00Z'}
 $history=@($prior);if($case -eq 'second-replacement'){$history+=@($prior.Clone())}
 $allowed=Test-ProviderTraceReplacement $history $boot $target $tool
 Assert ([bool]$allowed -eq ($case -eq 'corrected')) ('Unsafe trace replacement eligibility: '+$case)
}
$root='model';$config=@{CandidateAcpi='new-table';BaselineAcpi='old-table';Provider=@{Candidate=@{Sha256='new';Length=20};Baseline=@{Sha256='old';Length=10}}}
foreach($case in @('reuse','import','failed','incomplete','reconcile')){
 Reset;$state=[ordered]@{Phase='Prepared';ProviderStaged=$false}
 if($case -eq 'reuse'){$script:Exists=$true}
 if($case -eq 'failed'){$script:ImportExit=5}
 if($case -in @('incomplete','reconcile')){
  $state.Phase='StagingProvider';$script:Saved['provider-staging.json']=[pscustomobject]@{ImportRequested=$true;Exit=if($case -eq 'reconcile'){0}else{$null}}
  $script:Exists=$true
 }
 $failure=$null;try{Invoke-ProviderAction 'StageProviderPackage'}catch{$failure=$_.Exception.Message}
 Assert ($script:Calls -eq 0 -and !$script:Restarts) 'Staging selected a driver or restarted'
 if($case -in @('failed','incomplete')){Assert ($failure -and !$state.ProviderStaged) 'Failed import was accepted'}
 else{Assert (!$failure -and $state.ProviderStaged) ('Staging failed: '+$failure)}
 Assert ($script:Imports -eq [int]($case -in @('import','failed'))) 'Import was repeated or skipped'
}
foreach($case in @('wrong-table','open-gate','select','reboot','failed','missing-result','helper-exception','interrupted')){
 Reset;$state=[ordered]@{Phase='CandidateRebootRequested';Boot='2026-11-17T00:00:00Z';ProviderStaged=$true}
 $script:Exists=$true;$script:Saved['provider-staging.json']=@{Package=Known-Store}
 if($case -eq 'wrong-table'){$script:Loaded='fallback-table'}
 if($case -eq 'open-gate'){$script:Closed=$false}
 if($case -eq 'select'){$script:File=$config.Provider.Candidate} # already-selected identity: no new call
 if($case -eq 'failed') {$script:HelperExit=1}
 if($case -eq 'missing-result') {$script:HelperOutput=''}
 if($case -eq 'helper-exception') {$script:HelperThrows=$true}
 if($case -eq 'interrupted') {$script:Saved['provider-candidate-selection.json']=@{Phase='Selecting'}}
 $failure=$null;try{Invoke-ProviderSelection 'Candidate'}catch{$failure=$_.Exception.Message}
 if($case -in @('wrong-table','open-gate','interrupted')){Assert ($failure -and $script:Calls -eq 0) 'Gate/table/incomplete selection reached installer'}
 elseif($case -eq 'select'){Assert (!$failure -and $script:Calls -eq 0) 'Identical provider was unnecessarily selected'}
 elseif($case -in @('failed','missing-result','helper-exception')){
  Assert ($failure -and $script:Saved['provider-candidate-selection.json'].Phase -eq 'Failed') 'Selection error was swallowed'
  $again=$null;try{Invoke-ProviderSelection 'Candidate'}catch{$again=$_.Exception.Message}
  Assert ($again -and $script:Calls -eq 1) 'Failed installer was repeated'
 }
 else{
  Assert ($failure -and $script:Saved['provider-candidate-selection.json'].Phase -eq 'RebootRequired') 'Required restart was hidden'
  Invoke-ProviderAction 'ProviderRestart';Assert ($script:Restarts -eq 1) 'Missing required restart'
  $again=$null;try{Invoke-ProviderAction 'ProviderRestart'}catch{$again=$_.Exception.Message}
  Assert ($again -and $script:Restarts -eq 1) 'Duplicate restart was issued'
  $script:Boot='2026-11-17T00:01:00.004Z'
  $early=$null;try{Invoke-ProviderSelection 'Candidate'}catch{$early=$_.Exception.Message}
  Assert ($early -and $script:Saved['provider-candidate-selection.json'].Phase -eq 'RebootRequired') 'CIM jitter was treated as the required reboot'
  $script:Boot='2026-11-17T00:02:00Z';$script:File=$config.Provider.Candidate
  $script:RegistrationValid=$false;$wrongInf=$null
  try{Invoke-ProviderSelection 'Candidate'}catch{$wrongInf=$_.Exception.Message}
  Assert ($wrongInf -and $script:Saved['provider-candidate-selection.json'].Phase -eq 'RebootRequired' -and $script:Calls -eq 1) 'Mixed INF/SYS identity was accepted'
  $script:RegistrationValid=$true
  $script:LiveMatches=$false;$wrongLive=$null
  try{Invoke-ProviderSelection 'Candidate'}catch{$wrongLive=$_.Exception.Message}
  Assert ($wrongLive -and $script:Saved['provider-candidate-selection.json'].Phase -eq 'RebootRequired' -and $script:Calls -eq 1) 'Disk identity without loaded-image identity was accepted'
  $script:Saved['provider-loaded-candidate.json']=@{Observations=@(@{Boot=$script:Boot;Phase='Mismatch';MatchesOther=$true;Target=$config.Provider.Candidate})}
  Invoke-ProviderAction 'ProviderLoadRestart'
  Assert ($script:Restarts -eq 2 -and $script:Calls -eq 1) 'Known loaded-image mismatch did not get exactly one reconciliation boot'
  $duplicate=$null;try{Invoke-ProviderAction 'ProviderLoadRestart'}catch{$duplicate=$_.Exception.Message}
  Assert ($duplicate -and $script:Restarts -eq 2) 'Loaded-image reconciliation boot repeated'
  $script:Boot='2026-11-17T00:03:00Z';$script:LiveMatches=$true
  Invoke-ProviderSelection 'Candidate'
  Assert ($script:Calls -eq 1 -and $script:Saved['provider-candidate-selection.json'].Phase -eq 'SelectedAfterBoot') 'Selection repeated after required restart'
 }
}
foreach($case in @('missing','stale','unknown','wrong-target','unrebooted','wrong-disk')) {
 Reset;$state=[ordered]@{Phase='CandidateRebootRequested';Boot='2026-11-17T00:00:00Z'}
 $script:File=$config.Provider.Candidate;$script:Boot='2026-11-17T00:02:00Z'
 $script:Saved['provider-candidate-selection.json']=@{Phase='RebootRequired';RestartRequested=$true;Boot='2026-11-17T00:01:00Z'}
 $ob=@{Boot=$script:Boot;Phase='Mismatch';MatchesOther=$true;Target=$config.Provider.Candidate}
 if($case -eq 'stale'){$ob.Boot='2026-11-17T00:01:00Z'}
 if($case -eq 'unknown'){$ob.MatchesOther=$false}
 if($case -eq 'wrong-target'){$ob.Target=$config.Provider.Baseline}
 if($case -eq 'unrebooted'){$script:Boot='2026-11-17T00:01:00Z'}
 if($case -eq 'wrong-disk'){$script:File=$config.Provider.Baseline}
 if($case -ne 'missing'){$script:Saved['provider-loaded-candidate.json']=@{Observations=@($ob)}}
 $bad=$null;try{Invoke-ProviderAction 'ProviderLoadRestart'}catch{$bad=$_.Exception.Message}
 Assert ($bad -and !$script:Restarts -and !$script:Calls) ('Unproven loaded-image restart permitted: '+$case)
}
Reset;$state=[ordered]@{Phase='BaselineRebootRequested';Boot='2026-11-17T00:00:00Z'};$script:Loaded='old-table'
Invoke-ProviderSelection 'Baseline'
Assert ($script:Calls -eq 0) 'Original provider was unnecessarily reinstalled'
'PASS: provider staging, deferred replacement, loaded-image gating, known-mismatch recovery and duplicate/unknown restart rejection (CPU only)'
