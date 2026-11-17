# SPDX-License-Identifier: MIT
# Execute the real orchestration branches with all native side effects replaced.
param([string]$PairSource)
$ErrorActionPreference='Stop'
if(!$PairSource){$PairSource=Get-Content (Join-Path $PSScriptRoot '..\Pair.ps1') -Raw}
$tokens=$null;$errors=$null
$ast=[Management.Automation.Language.Parser]::ParseInput($PairSource,[ref]$tokens,[ref]$errors)
if($errors.Count){throw ($errors|Out-String)}
$same=@($ast.FindAll({param($n) $n -is [Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq 'Same-Boot'},$true))
if($same.Count -ne 1){throw 'Missing boot comparator'}
. ([scriptblock]::Create($same[0].Extent.Text))
function Branch([string]$Name){
 $found=@(foreach($n in $ast.FindAll({param($n) $n -is [Management.Automation.Language.IfStatementAst]},$true)){
  foreach($c in $n.Clauses){if($c.Item1.Extent.Text -eq ('$Action -eq '+"'"+$Name+"'")){$c.Item2}}
 })
 if($found.Count -ne 1){throw ('Missing branch '+$Name)}
 $s=$found[0].Extent.Text;[scriptblock]::Create($s.Substring(1,$s.Length-2))
}
function Assert($Condition,[string]$Message){if(!$Condition){throw $Message}}
function Assert-RetainedFiles {}
function Assert-Power {}
function Device-Property($Name){0}
function Join-Path($Path,$Child){$Child}
function Write-ActivationJson($Path,$Value){$script:Receipts[$Path]=$Value}
function Save-State($Value){$script:Phases+=@($Value.Phase)}
function Known-Package {@{Directory='registered-store';PublishedInf='oem0.inf'}}
function Find-StagedCandidate {
 $script:Reads++
 if($script:Corrupt){throw 'MODEL_CORRUPT_STORE'}
 if($script:Existing -or ($script:Imports -eq 1 -and $script:Registered)){Known-Package}else{$null}
}
function pnputil.exe {
 $script:Imports++
 Assert ($script:Phases -contains 'StagingPackage') 'Import intent must precede the native operation'
 $global:LASTEXITCODE=$script:ImportExit
 'MODEL_IMPORT'
}
function Close-Gate {$script:Gates++;throw 'MODEL_GATE'}
function Reset-Model {
 $script:Imports=0;$script:Reads=0;$script:Gates=0;$script:Receipts=@{};$script:Phases=@()
 $script:Existing=$true;$script:Registered=$true;$script:Corrupt=$false;$script:ImportExit=0
}
$root='model';$package=Branch 'StagePackage'
$cases=@('reuse','import','import-failure','not-registered','corrupt-store','already-staged')
foreach($case in $cases){
 Reset-Model
 $state=[ordered]@{Phase='Prepared';PackageStaged=$false}
 switch($case){
  'import' {$script:Existing=$false}
  'import-failure' {$script:Existing=$false;$script:ImportExit=5}
  'not-registered' {$script:Existing=$false;$script:Registered=$false}
  'corrupt-store' {$script:Corrupt=$true}
  'already-staged' {$state.PackageStaged=$true}
 }
 $errorText=$null;try{& $package}catch{$errorText=$_.Exception.Message}
 Assert ($script:Gates -eq 0) ($case+': package action must not close native startup')
 if($case -in @('reuse','import')){
  Assert (!$errorText) ($case+': unexpected failure '+$errorText)
  Assert ($state.Phase -eq 'Prepared' -and $state.PackageStaged) ($case+': missing completed staging state')
  Assert ($script:Receipts['candidate-staging.json'].Package.Directory -eq 'registered-store') ($case+': missing registered package identity')
  Assert ($script:Imports -eq [int]($case -eq 'import')) ($case+': unnecessary/missing import')
  Assert ($script:Receipts['candidate-staging.json'].Reused -eq ($case -eq 'reuse')) ($case+': wrong reuse receipt')
 }else{
  Assert ($null -ne $errorText) ($case+': failure was swallowed')
  if($case -ne 'already-staged'){Assert (!$state.PackageStaged) ($case+': failed staging was marked complete')}
 }
}

function Read-Boot {'2026-11-17T00:01:00Z'}
function Read-Json($Path){
 if($Path -eq 'pair-backup.json'){@{Boot='2026-11-17T00:01:00Z'}}
 elseif($Path -eq 'candidate-staging.json'){@{Package=Known-Package}}
 else{throw 'Unexpected model read'}
}
$stage=Branch 'StageCandidate'
$stageCases=@('verified','not-staged','missing-package','changed-package','corrupt-package')
foreach($case in $stageCases){
 Reset-Model
 $state=[ordered]@{Phase='Prepared';PackageStaged=$true}
 switch($case){
  'not-staged' {$state.PackageStaged=$false}
  'missing-package' {$script:Existing=$false}
  'changed-package' {function Find-StagedCandidate {@{Directory='different-store';PublishedInf='oem9.inf'}}}
  'corrupt-package' {$script:Corrupt=$true}
 }
 $errorText=$null;try{& $stage}catch{$errorText=$_.Exception.Message}
 Assert ($script:Imports -eq 0) ($case+': table action must never import a package')
 if($case -eq 'verified'){
  Assert ($errorText -eq 'MODEL_GATE' -and $script:Gates -eq 1) 'Verified staging did not reach the gate'
 }else{
  Assert ($errorText -and $script:Gates -eq 0) ($case+': invalid staging reached native startup changes')
 }
 # Restore the model after the changed-directory case.
 function Find-StagedCandidate {
  $script:Reads++
  if($script:Corrupt){throw 'MODEL_CORRUPT_STORE'}
  if($script:Existing -or ($script:Imports -eq 1 -and $script:Registered)){Known-Package}else{$null}
 }
}

# Restore must check the loaded table before selecting/starting any KMD.
$f=@($ast.FindAll({param($n) $n -is [Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq 'Assert-LoadedDsdt'},$true))
if($f.Count -ne 1){throw 'Missing loaded-table assertion'}
. ([scriptblock]::Create($f[0].Extent.Text))
function Assert-Closed {}
function Assert-File($Path,$Expected) {}
function Read-LoadedDsdt($Path){$script:Loaded}
function Select-Package($Directory,$Expected,$Receipt){$script:Selections++;throw 'MODEL_SELECTED_BASELINE'}
$baseline=Branch 'SelectBaseline';$config=@{BaselineAcpi=@{Length=4848;Sha256='baseline'};BaselineKmd=@{}}
foreach($valid in @($true,$false)){
 $state=[ordered]@{Phase='BaselineRebootRequested';Boot='2026-11-17T00:00:00Z'}
 $script:Selections=0;$script:Loaded=@{Length=4848;Sha256=if($valid){'baseline'}else{'fallback'}}
 $errorText=$null;try{& $baseline}catch{$errorText=$_.Exception.Message}
 Assert ($script:Selections -eq [int]$valid) 'Restoration did not respect the loaded-table identity'
 if($valid){Assert ($errorText -eq 'MODEL_SELECTED_BASELINE') ('Unexpected restore branch failure '+$errorText)}
}
# Importing a package has not changed the boot table or gate. Do not reboot
# it as though a candidate boot had already happened, including on timeout.
$recover=Branch 'Recover'
foreach($phase in @('StagingPackage','CandidateObserved')){
 Reset-Model;$state=[ordered]@{Phase=$phase}
 $errorText=$null;try{& $recover}catch{$errorText=$_.Exception.Message}
 if($phase -eq 'StagingPackage'){
  Assert ($script:Gates -eq 0 -and $errorText -eq 'No eligible new recovery request') 'An in-progress import reached recovery reboot preparation'
 }else{
  Assert ($script:Gates -eq 1 -and $errorText -eq 'MODEL_GATE') 'Candidate recovery is unavailable'
 }
}
Write-Output 'PASS: 6 package, 5 boot-handoff, 2 restoration and 2 recovery controls; native operations replaced'

Assert (Same-Boot '2026-11-17T06:47:23.9484490Z' '2026-11-17T06:47:23.9527220Z') 'Observed same-boot drift was rejected'
Assert (!(Same-Boot '2026-11-17T06:47:23Z' '2026-11-17T06:48:23Z')) 'Distinct boot was accepted as the same boot'
