# SPDX-License-Identifier: MIT
# Recovery rebasing with file/device operations replaced; no native installation.
param([string]$PairSource)
$ErrorActionPreference='Stop'
if(!$PairSource){$PairSource=Get-Content (Join-Path $PSScriptRoot '..\Pair.ps1') -Raw}
$tokens=$null;$errors=$null
$ast=[Management.Automation.Language.Parser]::ParseInput($PairSource,[ref]$tokens,[ref]$errors)
if($errors.Count){throw ($errors|Out-String)}
$f=@($ast.FindAll({param($n)$n -is [Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq 'Prepare-CandidateRecovery'},$true))
if($f.Count -ne 1){throw 'Missing recovery rebasing'}
. ([scriptblock]::Create($f[0].Extent.Text))
function Assert($Condition,$Message){if(!$Condition){throw $Message}}
function File-Record($Path){if(!$script:Files.ContainsKey($Path)){throw 'MODEL_FILE_MISSING'};$script:Files[$Path]}
function Assert-File($Path,$Expected){$a=File-Record $Path;if($a.Sha256 -ne $Expected.Sha256 -or $a.Length -ne $Expected.Length){throw 'MODEL_BASELINE_DIFFERENT'}}
function Durable-Copy($From,$To){$script:Copies[$To]=File-Record $From;if($script:CopyFails){throw 'MODEL_COPY_FAILED'}}
function Get-ChildItem($Path,[switch]$File){@('tegragpurender.sys','tegragpurender.inf','tegragpurender.cat')|ForEach-Object{[pscustomobject]@{Name=$_;FullName=Join-Path $Path $_}}}
function Device-Property($Name){if($script:BadInf){'not-registered'}else{'oem7.inf'}}
function Write-ActivationJson($Path,$Value){$script:Saved=$Value}
$root='C:\Model\trial';$candidate='C:\Model\trial\activation'
foreach($case in @('success','wrong-baseline','copy-failure','unknown-inf')){
 $script:Files=@{};$script:Copies=@{};$script:Saved=$null
 $script:CopyFails=$case -eq 'copy-failure';$script:BadInf=$case -eq 'unknown-inf'
 $new=@{Length=242184;Sha256=('b'*64)};$table=@{Length=4993;Sha256=('c'*64)}
 $old=@{Length=204808;Sha256=('a'*64)};$display=@{Length=100;Sha256=('d'*64)}
 $config=@{BaselineKmd=$new;BaselineAcpi=$table}
 foreach($n in @('tegragpurender.sys','tegragpurender.inf','tegragpurender.cat')){$script:Files[(Join-Path $root ('baseline-kmd\'+$n))]=$new}
 $script:Files[(Join-Path $root 'baseline-acpi.dat')]=$table
 if($case -eq 'wrong-baseline'){$script:Files[(Join-Path $root 'baseline-kmd\tegragpurender.sys')]=$old}
 $r=[pscustomobject]@{OriginalOem='oem118.inf';Utc='old';Files=[pscustomobject]@{'rollback\tegragpurender.sys'=$old;'rollback\acpitabl.dat'=$old;'user-display.bin'=$display}}
 $errorText=$null;try{Prepare-CandidateRecovery $r}catch{$errorText=$_.Exception.Message}
 if($case -eq 'success'){
  Assert (!$errorText -and $script:Saved.OriginalOem -eq 'oem7.inf') ('Recovery was not rebound: '+$errorText)
  Assert ($script:Saved.Files.'rollback\tegragpurender.sys'.Sha256 -eq $new.Sha256) 'Old KMD remained the embedded recovery target'
  Assert ($script:Saved.Files.'rollback\acpitabl.dat'.Sha256 -eq $table.Sha256) 'Old table remained the embedded recovery target'
  Assert ($script:Saved.Files.'user-display.bin'.Sha256 -eq $display.Sha256) 'Display preference artifact was changed'
  Assert ($script:Copies.Count -eq 4) 'Recovery package/table copy incomplete'
 }else{
  Assert ($errorText -and !$script:Saved) ('Incomplete recovery was published: '+$case)
  if($case -eq 'wrong-baseline'){Assert ($script:Copies.Count -eq 0) 'Mismatched package reached copying'}
 }
}
Write-Output 'PASS: current KMD/ACPI recovery rebasing and failure publication controls; native effects replaced'
