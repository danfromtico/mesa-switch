# SPDX-License-Identifier: MIT
# Pure control of the actual Pair.ps1 selection branch; no device or file writes.
param([string]$PairSource)
$ErrorActionPreference='Stop'
if(!$PairSource){$PairSource=Get-Content (Join-Path $PSScriptRoot '..\Pair.ps1') -Raw}
$tokens=$null;$errors=$null
$ast=[Management.Automation.Language.Parser]::ParseInput($PairSource,[ref]$tokens,[ref]$errors)
if($errors.Count){throw ($errors|Out-String)}
$functions=@($ast.FindAll({param($n) $n -is [Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq 'Assert-EmcReady'},$true))
if($functions.Count -ne 1){throw 'Missing unique EMC gate'}
. ([scriptblock]::Create($functions[0].Extent.Text))
$branches=@(foreach($n in $ast.FindAll({param($n) $n -is [Management.Automation.Language.IfStatementAst]},$true)){
 foreach($clause in $n.Clauses){if($clause.Item1.Extent.Text -match '^\$Action\s+-eq\s+''SelectCandidate''$'){$clause.Item2}}
})
if($branches.Count -ne 1){throw 'Missing unique selection branch'}
$text=$branches[0].Extent.Text
$selection=[scriptblock]::Create($text.Substring(1,$text.Length-2))

# These replace every command reached before Select-Package. Selection itself
# throws a sentinel, so later native activation code can never execute here.
function Read-Boot {'after'}
function Device-Property([string]$Name){43}
function Assert-RetainedFiles {}
function Assert-Power {}
function Assert-Closed {}
function Assert-File($Path,$Expected) {}
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
 [pscustomobject]@{Version=1;Flags=367;LastError=0;EnumerationError=0;ActmonError=0;QosError=0}
}
$config=[pscustomobject]@{CandidateAcpi=@{};CandidateKmd=@{}}
$root='model';$cases=@('healthy','actmon','qos','mailbox','transition','disabled','missing-field','missing-status','wrong-version','query-failure')
foreach($case in $cases){
 $script:Emc=Healthy;$script:ReadFailure=$false
 switch($case){
  'actmon' {$script:Emc.ActmonError=-3;$script:Emc.Flags=7}
  'qos' {$script:Emc.QosError=-1}
  'mailbox' {$script:Emc.Flags=$script:Emc.Flags -bor 16}
  'transition' {$script:Emc.Flags=$script:Emc.Flags -bor 128}
  'disabled' {$script:Emc.Flags=$script:Emc.Flags -band (-bnot 8)}
  'missing-field' {$script:Emc.PSObject.Properties.Remove('ActmonError')}
  'missing-status' {$script:Emc=$null}
  'wrong-version' {$script:Emc.Version=2}
  'query-failure' {$script:ReadFailure=$true}
 }
 $script:Selections=0;$script:Snapshots=@{};$script:SavedPhases=@()
 $state=[ordered]@{Phase='CandidateRebootRequested';Boot='before'}
 $message=$null
 try{& $selection}catch{$message=$_.Exception.Message}
 Assert ($null -ne $message) ($case+': expected a rejection or selection sentinel')
 Assert ($script:Snapshots.ContainsKey('before-candidate-selection.json')) ($case+': missing durable observation')
 if($case -eq 'healthy'){
  Assert ($message -eq 'MODEL_SELECTED' -and $script:Selections -eq 1) 'Healthy baseline did not reach selection'
  Assert ($state.Phase -eq 'SelectingCandidate') 'Healthy state differs'
 }else{
  Assert ($script:Selections -eq 0) ($case+': candidate selection must not run')
  Assert ($state.Phase -eq 'CandidatePlatformRejected') ($case+': rejected state not saved')
  Assert ($script:SavedPhases -notcontains 'SelectingCandidate') ($case+': premature state transition')
 }
}
Write-Output ('PASS: '+$cases.Count+' EMC selection controls; native operations replaced, no hardware validation')
