# SPDX-License-Identifier: MIT
# Pure control of the actual Pair.ps1 selection branch; no device or file writes.
param([string]$PairSource)
$ErrorActionPreference='Stop'
if(!$PairSource){$PairSource=Get-Content (Join-Path $PSScriptRoot '..\Pair.ps1') -Raw}
$tokens=$null;$errors=$null
$ast=[Management.Automation.Language.Parser]::ParseInput($PairSource,[ref]$tokens,[ref]$errors)
if($errors.Count){throw ($errors|Out-String)}
$functions=@($ast.FindAll({param($n) $n -is [Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -in @('Assert-EmcReady','Assert-LoadedDsdt','Same-Boot')},$true))
if($functions.Count -ne 3){throw 'Missing EMC/loaded-table gates'}
foreach($f in $functions){. ([scriptblock]::Create($f.Extent.Text))}
$branches=@(foreach($n in $ast.FindAll({param($n) $n -is [Management.Automation.Language.IfStatementAst]},$true)){
 foreach($clause in $n.Clauses){if($clause.Item1.Extent.Text -match '^\$Action\s+-eq\s+''SelectCandidate''$'){$clause.Item2}}
})
if($branches.Count -ne 1){throw 'Missing unique selection branch'}
$text=$branches[0].Extent.Text
$selection=[scriptblock]::Create($text.Substring(1,$text.Length-2))

# These replace every command reached before Select-Package. Selection itself
# throws a sentinel, so later native activation code can never execute here.
function Read-Boot {'2026-11-17T00:01:00Z'}
function Device-Property([string]$Name){43}
function Assert-RetainedFiles {}
function Assert-Power {}
function Assert-Closed {}
function Assert-File($Path,$Expected) {}
function Read-LoadedDsdt($Path){if($script:FirmwareFailure){throw 'MODEL_FIRMWARE_FAILURE'};$script:LoadedDsdt}
function Get-PnpDevice {if($script:Present){[pscustomobject]@{Present=$true}}}
function Join-Path($Path,$ChildPath){if($ChildPath -eq 'Power-State.ps1'){'Read-ModelPower'}else{$ChildPath}}
function Read-ModelPower([switch]$EmcOnly){
 if($script:ReadFailure){throw 'MODEL_QUERY_FAILURE'}
 if($EmcOnly){[pscustomobject]@{Emc=$script:Emc}}else{[pscustomobject]@{PowerMode=1}}
}
function Write-ActivationJson($Path,$Value){$script:Snapshots[$Path]=$Value}
function Save-State($Value){$script:SavedPhases+=@($Value.Phase)}
function Select-Package($Directory,$Expected,$Receipt){$script:Selections++;throw 'MODEL_SELECTED'}
function Assert($Condition,[string]$Message){if(!$Condition){throw $Message}}
function Healthy {
 # Fields from the verified baseline; fixtures do not call its IOCTL.
 [pscustomobject]@{Version=1;Flags=367;LastError=0;EnumerationError=0;ActmonError=0;QosError=0;SampleAgeMs=100;ActualKhz=1331200;BootKhz=2500000;TransitionCount=803}
}
$config=[pscustomobject]@{CandidateAcpi=@{Length=4981;Sha256='candidate'};CandidateKmd=@{}}
$root='model';$device='model-device'
$cases=@('healthy','closed-idle','closed-pending','actmon','actmon-invalid','qos','mailbox','transition',
 'missing-actmon-flag','missing-field','missing-status','wrong-version','query-failure',
 'stale','missing-age','missing-rate','pending-transitioned','pending-low-rate','pending-changed-rate',
 'pending-dynamic','pending-qos','last-error','missing-last-error','wrong-table','firmware-failure','absent-device')
foreach($case in $cases){
 $script:Emc=Healthy;$script:ReadFailure=$false;$script:FirmwareFailure=$false;$script:Present=$true
 $script:LoadedDsdt=@{Length=4981;Sha256='candidate'}
 if($case -like 'pending-*' -or $case -eq 'closed-pending'){
  $script:Emc.Flags=71;$script:Emc.LastError=-6;$script:Emc.ActualKhz=2500000;$script:Emc.TransitionCount=0
 }
 switch($case){
  'closed-idle' {$script:Emc.Flags=71}
  'actmon' {$script:Emc.ActmonError=-3;$script:Emc.Flags=7}
  'actmon-invalid' {$script:Emc.ActmonError=-1;$script:Emc.Flags=7}
  'qos' {$script:Emc.QosError=-1}
  'mailbox' {$script:Emc.Flags=$script:Emc.Flags -bor 16}
  'transition' {$script:Emc.Flags=$script:Emc.Flags -bor 128}
  'missing-actmon-flag' {$script:Emc.Flags=$script:Emc.Flags -band (-bnot 64)}
  'missing-field' {$script:Emc.PSObject.Properties.Remove('ActmonError')}
  'missing-status' {$script:Emc=$null}
  'wrong-version' {$script:Emc.Version=2}
  'query-failure' {$script:ReadFailure=$true}
  'stale' {$script:Emc.SampleAgeMs=6001}
  'missing-age' {$script:Emc.PSObject.Properties.Remove('SampleAgeMs')}
  'missing-rate' {$script:Emc.ActualKhz=0}
  'pending-transitioned' {$script:Emc.TransitionCount=1}
  'pending-low-rate' {$script:Emc.ActualKhz=1600000;$script:Emc.BootKhz=1600000}
  'pending-changed-rate' {$script:Emc.BootKhz=1600000}
  'pending-dynamic' {$script:Emc.Flags=$script:Emc.Flags -bor 8}
  'pending-qos' {$script:Emc.Flags=$script:Emc.Flags -bor 32}
  'last-error' {$script:Emc.LastError=-1}
  'missing-last-error' {$script:Emc.PSObject.Properties.Remove('LastError')}
  'wrong-table' {$script:LoadedDsdt.Sha256='firmware-fallback'}
  'firmware-failure' {$script:FirmwareFailure=$true}
  'absent-device' {$script:Present=$false}
 }
 $script:Selections=0;$script:Snapshots=@{};$script:SavedPhases=@()
 $state=[ordered]@{Phase='CandidateRebootRequested';Boot='2026-11-17T00:00:00Z'}
 $message=$null
 try{& $selection}catch{$message=$_.Exception.Message}
 Assert ($null -ne $message) ($case+': expected a rejection or selection sentinel')
 Assert ($script:Snapshots.ContainsKey('before-candidate-selection.json')) ($case+': missing durable observation')
 if($case -in @('healthy','closed-idle','closed-pending')){
  Assert ($message -eq 'MODEL_SELECTED' -and $script:Selections -eq 1) ($case+': did not reach selection: '+$message)
  Assert ($state.Phase -eq 'SelectingCandidate') ($case+': state differs')
 }else{
  Assert ($script:Selections -eq 0) ($case+': candidate selection must not run')
  Assert ($state.Phase -eq 'CandidatePlatformRejected') ($case+': rejected state not saved')
  Assert ($script:SavedPhases -notcontains 'SelectingCandidate') ($case+': premature state transition')
 }
}
# Exercise the actual post-activation branch, stopping at its native query.
$branches=@(foreach($n in $ast.FindAll({param($n) $n -is [Management.Automation.Language.IfStatementAst]},$true)){
 foreach($clause in $n.Clauses){if($clause.Item1.Extent.Text -match '^\$Action\s+-eq\s+''FinishCandidate''$'){$clause.Item2}}
})
if($branches.Count -ne 1){throw 'Missing unique finish branch'}
$text=$branches[0].Extent.Text;$finish=[scriptblock]::Create($text.Substring(1,$text.Length-2))
function Device-Property([string]$Name){0}
function Read-Json($Path){[pscustomobject]@{Status='Enabled'}}
function Test-Path($Path){$false}
function Join-Path($Path,$ChildPath){
 if($ChildPath -eq 'Power-State.ps1'){'Read-ModelPower'}
 elseif($ChildPath -eq 'nvdec-resource-query.exe'){'Read-ModelQuery'}else{$ChildPath}
}
function Read-ModelQuery {$script:Queries++;throw 'MODEL_QUERIED'}
$afterCases=@('healthy','not-dynamic','pending','actmon','wrong-table')
foreach($case in $afterCases){
 $script:Emc=Healthy;$script:ReadFailure=$false;$script:FirmwareFailure=$false
 $script:LoadedDsdt=@{Length=4981;Sha256='candidate'}
 switch($case){
  'not-dynamic' {$script:Emc.Flags=71}
  'pending' {$script:Emc.LastError=-6}
  'actmon' {$script:Emc.ActmonError=-3}
  'wrong-table' {$script:LoadedDsdt.Sha256='firmware-fallback'}
 }
 $script:Queries=0;$script:Snapshots=@{};$script:SavedPhases=@()
 $state=[ordered]@{Phase='CandidateActivationRequested';ActivationBoot='2026-11-17T00:01:00Z'}
 $message=$null;try{& $finish}catch{$message=$_.Exception.Message}
 Assert ($script:Snapshots.ContainsKey('after-candidate-activation.json')) ($case+': missing active observation')
 if($case -eq 'healthy'){
  Assert ($message -eq 'MODEL_QUERIED' -and $script:Queries -eq 1) ('Healthy active state did not reach query: '+$message)
 }else{
  Assert ($script:Queries -eq 0) ($case+': hardware query must not run')
  Assert ($state.Phase -eq 'CandidatePlatformRejectedAfterActivation') ($case+': post-activation rejection not saved')
 }
}
Write-Output ('PASS: '+$cases.Count+' selection and '+$afterCases.Count+' active controls; native operations replaced, no hardware validation')
