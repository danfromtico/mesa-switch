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
Add-Type @'
using System;
public static class NvdecProviderDriverApi {
 public static int Calls;
 public static bool Result=true, Reboot=false;
 public static bool Update(IntPtr p,string id,string inf,uint flags,out bool reboot) {
  if(p!=IntPtr.Zero || id!="ACPI\\SWOA0402" || flags!=5) throw new Exception("MODEL_ARGUMENT");
  ++Calls; reboot=Reboot;return Result;
 }
}
'@
function Assert($condition,[string]$why){if(!$condition){throw $why}}
function Assert-RetainedFiles {}
function Assert-Power {}
function Assert-Closed {if(!$script:Closed){throw 'MODEL_GATE'}}
function Read-Boot {$script:Boot}
function Device-Property($Name){0}
function Provider-Property($Name){0}
function Join-Path($Path,$Child){$Child}
function Write-ActivationJson($Path,$Value){$script:Saved[$Path]=$Value}
function Save-State($Value){$script:Phases+=@($Value.Phase)}
function Test-Path($Path){$script:Saved.ContainsKey($Path)}
function Read-Json($Path){if(!$script:Saved.ContainsKey($Path)){throw 'MODEL_MISSING_RECEIPT'};$script:Saved[$Path]}
function File-Record($Path){$script:File}
function Assert-File($Path,$Expected){Assert ($script:File.Sha256 -eq $Expected.Sha256) 'MODEL_FILE'}
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
 [NvdecProviderDriverApi]::Calls=0;[NvdecProviderDriverApi]::Result=$true;[NvdecProviderDriverApi]::Reboot=$false
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
 Assert ([NvdecProviderDriverApi]::Calls -eq 0 -and !$script:Restarts) 'Staging selected a driver or restarted'
 if($case -in @('failed','incomplete')){Assert ($failure -and !$state.ProviderStaged) 'Failed import was accepted'}
 else{Assert (!$failure -and $state.ProviderStaged) ('Staging failed: '+$failure)}
 Assert ($script:Imports -eq [int]($case -in @('import','failed'))) 'Import was repeated or skipped'
}
foreach($case in @('wrong-table','open-gate','select','reboot','failed')){
 Reset;$state=[ordered]@{Phase='CandidateRebootRequested';Boot='2026-11-17T00:00:00Z';ProviderStaged=$true}
 $script:Exists=$true;$script:Saved['provider-staging.json']=@{Package=Known-Store}
 if($case -eq 'wrong-table'){$script:Loaded='fallback-table'}
 if($case -eq 'open-gate'){$script:Closed=$false}
 if($case -eq 'select'){$script:File=$config.Provider.Candidate} # already-selected identity: no new call
 if($case -eq 'reboot') {[NvdecProviderDriverApi]::Reboot=$true}
 if($case -eq 'failed') {[NvdecProviderDriverApi]::Result=$false}
 $failure=$null;try{Invoke-ProviderSelection 'Candidate'}catch{$failure=$_.Exception.Message}
 if($case -in @('wrong-table','open-gate')){Assert ($failure -and [NvdecProviderDriverApi]::Calls -eq 0) 'Gate/table failure reached selection'}
 elseif($case -eq 'select'){Assert (!$failure -and [NvdecProviderDriverApi]::Calls -eq 0) 'Identical provider was unnecessarily selected'}
 elseif($case -eq 'failed'){Assert ($failure -and $script:Saved['provider-candidate-selection.json'].Phase -eq 'Failed') 'Selection error was swallowed'}
 else{
  Assert ($failure -and $script:Saved['provider-candidate-selection.json'].Phase -eq 'RebootRequired') 'Required restart was hidden'
  Invoke-ProviderAction 'ProviderRestart';Assert ($script:Restarts -eq 1) 'Missing required restart'
  $again=$null;try{Invoke-ProviderAction 'ProviderRestart'}catch{$again=$_.Exception.Message}
  Assert ($again -and $script:Restarts -eq 1) 'Duplicate restart was issued'
  $script:Boot='2026-11-17T00:01:00.004Z'
  $early=$null;try{Invoke-ProviderSelection 'Candidate'}catch{$early=$_.Exception.Message}
  Assert ($early -and $script:Saved['provider-candidate-selection.json'].Phase -eq 'RebootRequired') 'CIM jitter was treated as the required reboot'
  $script:Boot='2026-11-17T00:02:00Z';$script:File=$config.Provider.Candidate
  Invoke-ProviderSelection 'Candidate'
  Assert ([NvdecProviderDriverApi]::Calls -eq 1 -and $script:Saved['provider-candidate-selection.json'].Phase -eq 'SelectedAfterBoot') 'Selection repeated after required restart'
 }
}
Reset;$state=[ordered]@{Phase='BaselineRebootRequested';Boot='2026-11-17T00:00:00Z'};$script:Loaded='old-table'
Invoke-ProviderSelection 'Baseline'
Assert ([NvdecProviderDriverApi]::Calls -eq 0) 'Original provider was unnecessarily reinstalled'
'PASS: provider staging, incomplete-operation guards, loaded-table gating and bounded recovery restart (CPU only)'
