# SPDX-License-Identifier: MIT
# Real orchestration and identity comparison; all native I/O and restarts are
# replaced. These controls do not establish storage durability or boot loading.
param([string]$PairSource)
$ErrorActionPreference='Stop'
if(!$PairSource){$PairSource=Get-Content (Join-Path $PSScriptRoot '..\Pair.ps1') -Raw}
$tokens=$null;$errors=$null
$ast=[Management.Automation.Language.Parser]::ParseInput($PairSource,[ref]$tokens,[ref]$errors)
if($errors.Count){throw ($errors|Out-String)}
function Assert($Condition,[string]$Message){if(!$Condition){throw $Message}}
foreach($name in @('Same-Boot','Commit-BootTable')){
 $f=@($ast.FindAll({param($n) $n -is [Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq $name},$true))
 Assert ($f.Count -eq 1) ('Missing function '+$name)
 . ([scriptblock]::Create($f[0].Extent.Text))
}
function Branch([string]$Name){
 $found=@(foreach($n in $ast.FindAll({param($n) $n -is [Management.Automation.Language.IfStatementAst]},$true)){
  foreach($c in $n.Clauses){if($c.Item1.Extent.Text -eq ('$Action -eq '+"'"+$Name+"'")){$c.Item2}}
 })
 Assert ($found.Count -eq 1) ('Missing branch '+$Name)
 $s=$found[0].Extent.Text;[scriptblock]::Create($s.Substring(1,$s.Length-2))
}
Add-Type @'
using System;
public static class NvdecBootTableIo {
 public static int Mode,Flushes,Reads;
 public static byte[] Bytes;
 public static string FlushVolume(string path) {
  Flushes++;if(Mode==1)throw new Exception("MODEL_FLUSH");return "MODEL_VOLUME";
 }
 public static byte[] ReadUnbuffered(string path) {
  Reads++;if(Mode==2)throw new Exception("MODEL_READ");
  byte[] b=(byte[])Bytes.Clone();if(Mode==3)b[0]^=1;return b;
 }
}
'@
$bytes=[byte[]](0..47);[NvdecBootTableIo]::Bytes=$bytes
$sha=[Security.Cryptography.SHA256]::Create()
try{$digest=([BitConverter]::ToString($sha.ComputeHash($bytes))).Replace('-','').ToLowerInvariant()}finally{$sha.Dispose()}
$expected=@{Length=$bytes.Length;Sha256=$digest}
function Initialize-BootTableIo {}
function Assert-RetainedFiles {}
function Assert-Power {}
function Device-Property($Name){0}
function Join-Path($Path,$Child){$Child}
function File-Record($Path){$expected}
function Test-Path($Path){if($Path -like '*acpitabl.dat'){$script:SameTable}else{$script:ExistingReceipt}}
function Durable-Copy($From,$To){$script:Copies++;if($script:CopyFails){throw 'MODEL_COPY'}}
function Write-ActivationJson($Path,$Value){$script:Receipt=$Value;$script:ReceiptPhases+=@($Value.Phase)}
function Save-State($Value){$script:StatePhases+=@($Value.Phase)}
function Read-Boot {'2026-11-17T00:01:00Z'}
function Known-Package {@{Directory='MODEL_STORE';PublishedInf='oem0.inf'}}
function Find-StagedCandidate {Known-Package}
function Read-Json($Path){
 if($Path -eq 'pair-backup.json'){@{Boot=Read-Boot}}
 elseif($Path -eq 'candidate-staging.json'){@{Package=Known-Package}}
 else{throw 'Unexpected model read'}
}
function Close-Gate {$script:Gates++}
function shutdown.exe {
 Assert ($script:Receipt.Phase -eq 'Committed') 'Restart preceded successful commit'
 $script:Restarts++;$global:LASTEXITCODE=0
}
$root='MODEL_ROOT';$config=@{}
foreach($action in @('StageCandidate','Recover')){
 $branch=Branch $action
 foreach($case in @('copy-failure','flush-failure','read-failure','mismatch','existing-receipt','success','same-table')){
  $script:Copies=0;$script:Gates=0;$script:Restarts=0;$script:Receipt=$null
  $script:ReceiptPhases=@();$script:StatePhases=@()
  $script:SameTable=$case -eq 'same-table'
  $script:CopyFails=$case -in @('copy-failure','same-table');$script:ExistingReceipt=$case -eq 'existing-receipt'
  [NvdecBootTableIo]::Mode=switch($case){'flush-failure'{1};'read-failure'{2};'mismatch'{3};default{0}}
  [NvdecBootTableIo]::Flushes=0;[NvdecBootTableIo]::Reads=0
  $state=[ordered]@{Phase=if($action -eq 'StageCandidate'){'Prepared'}else{'CandidateObserved'};PackageStaged=$true}
  $errorText=$null;try{& $branch}catch{$errorText=$_.Exception.Message}
  $label=$action+'/'+$case
  Assert ($script:Gates -eq 1) ($label+': startup gate was not closed')
  if($case -in @('success','same-table')){
   Assert (!$errorText -and $script:Restarts -eq 1) ($label+': valid commit did not reach one restart: '+$errorText)
   Assert ($script:Receipt.Unbuffered.Sha256 -eq $digest) ($label+': readback identity missing')
   $phases=if($script:SameTable){'CheckingFile,FlushingVolume,ReadingUnbuffered,Committed'}else{'CheckingFile,Copying,FlushingVolume,ReadingUnbuffered,Committed'}
   Assert ($script:ReceiptPhases -join ',' -eq $phases) ($label+': commit phase order differs')
   Assert ($script:Copies -eq [int](!$script:SameTable) -and $script:Receipt.Reused -eq $script:SameTable) ($label+': identical table was rewritten or wrong table reused')
  }else{
   Assert ($errorText -and $script:Restarts -eq 0) ($label+': failed commit reached restart')
   Assert (!$state.RestartRequested) ($label+': failed commit marked restart requested')
   if($case -eq 'existing-receipt'){
    Assert ($script:Copies -eq 0 -and [NvdecBootTableIo]::Flushes -eq 0) ($label+': interrupted operation was repeated')
   }else{Assert ($script:Receipt.Phase -eq 'Failed') ($label+': failure receipt missing')}
   if($case -eq 'flush-failure'){Assert ([NvdecBootTableIo]::Reads -eq 0) ($label+': read proceeded after flush failure')}
  }
 }
}
Write-Output 'PASS: candidate/recovery commit, I/O failure, mismatch and interrupted-receipt controls; native effects replaced'
