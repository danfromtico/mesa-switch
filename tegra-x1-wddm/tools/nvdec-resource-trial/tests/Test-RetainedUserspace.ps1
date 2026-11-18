# SPDX-License-Identifier: MIT
# Registration binding control; file operations are replaced, not native proof.
param([string]$PairSource)
$ErrorActionPreference='Stop'
if(!$PairSource){$PairSource=Get-Content (Join-Path $PSScriptRoot '..\Pair.ps1') -Raw}
$tokens=$null;$errors=$null
$ast=[Management.Automation.Language.Parser]::ParseInput($PairSource,[ref]$tokens,[ref]$errors)
if($errors.Count){throw ($errors|Out-String)}
$f=@($ast.FindAll({param($n)$n -is [Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq 'Assert-UserspaceRegistration'},$true))
if($f.Count -ne 1){throw 'Missing registration binding'}
. ([scriptblock]::Create($f[0].Extent.Text))
function Assert($Condition,$Message){if(!$Condition){throw $Message}}
function Assert-File($Path,$Expected){
 $script:Checks++
 Assert ($Path -eq $script:ExpectedPath -and !$script:Corrupt) 'MODEL_FILE_DIFFERENT'
}
foreach($case in @('original','retained','wrong-path','missing-identity','different-bytes','corrupt-file')){
 $active='C:\Model\activation';$record=@{Length=3274240;Sha256=('a'*64)}
 $manifest=@{files=@{'app\TegraGpuD3DVk.dll'=$record}}
 $config=@{BaselineFiles=@{}}
 if($case -ne 'original'){$config.BaselineUserspaceDirectory='C:\Model\userspace'}
 $directory=if($config.BaselineUserspaceDirectory){$config.BaselineUserspaceDirectory}else{$active}
 $path=Join-Path $directory 'app\TegraGpuD3DVk.dll'
 $config.BaselineFiles[$path]=$record.Clone()
 $registration=@{Umd=@('unchanged',$path,$path,'unchanged');Icd='unchanged'}
 if($case -eq 'wrong-path'){$registration.Umd[1]=$registration.Umd[2]='C:\Model\unexpected.dll'}
 if($case -eq 'missing-identity'){$config.BaselineFiles=@{}}
 if($case -eq 'different-bytes'){$config.BaselineFiles[$path].Sha256='b'*64}
 $script:ExpectedPath=$path;$script:Checks=0;$script:Corrupt=$case -eq 'corrupt-file'
 $errorText=$null;try{Assert-UserspaceRegistration $registration $manifest}catch{$errorText=$_.Exception.Message}
 if($case -in @('original','retained')){Assert (!$errorText -and $script:Checks -eq 1) ('Valid binding rejected: '+$case+'/'+$errorText)}
 else{Assert ($null -ne $errorText) ('Invalid binding accepted: '+$case)}
}
Write-Output 'PASS: original/retained userspace path and identity rejection controls; file I/O replaced'

$fn=$ast.Find({param($n)$n -is [Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq 'Prepare-VideoUmd'},$true)
. ([scriptblock]::Create($fn.Extent.Text))
$root='C:\Model\inputs';$candidate='C:\Model\candidate'
$config=@{CandidateUmd=@{Image=@{Sha256='a'};Control=@{Sha256='b'};RetainedIcd=@{Sha256='c'};Commit='video';Source='frozen + video'}}
function Read-Json($Path){
 if($Path -eq (Join-Path $root 'preparation.json')){return $receipt}
 [pscustomobject]@{TemporaryUmd='old-umd';Manifest='old-manifest'}
}
function Assert-File($Path,$Record){Assert ($Record.Sha256 -in 'a','b','c') 'Unknown file record'}
function Durable-Copy($From,$To){$script:Copied+=@($To)}
function Write-ActivationJson($Path,$Value){$script:Registration=$Value}
foreach($case in 'passed','no-exit','wrong-control'){
 $receipt=@{Phase='Ready';ControlExit=0;Control='b'}
 if($case -eq 'no-exit'){$receipt.ControlExit=$null}
 if($case -eq 'wrong-control'){$receipt.Control='other'}
 $m=[pscustomobject]@{app=[pscustomobject]@{'TegraGpuD3DVk.dll'=0;'tgd3d-contracts.exe'=0};
   files=[pscustomobject]@{'app\TegraGpuD3DVk.dll'=0;'app\tgd3d-contracts.exe'=0};umd_patch_commits=@('base')}
 $script:Copied=@();$script:Registration=$null;$failure=$null
 try{Prepare-VideoUmd $m}catch{$failure=$_.Exception.Message}
 if($case -eq 'passed'){
  Assert (!$failure -and $script:Copied.Count -eq 2) ('Paired preparation failed: '+$failure)
  Assert ($m.app.'TegraGpuD3DVk.dll'.Sha256 -eq 'a' -and $m.files.'app\tgd3d-contracts.exe'.Sha256 -eq 'b') 'Component record differs'
  Assert (($m.umd_patch_commits -join ',') -eq 'base,video') 'Patch ancestry differs'
  Assert ($script:Registration.TemporaryUmd -eq (Join-Path $candidate 'app\TegraGpuD3DVk.dll')) 'Candidate registration differs'
 }else{Assert ($failure -and !$script:Copied.Count -and !$script:Registration) 'Invalid control receipt reached staging'}
}
Write-Output 'PASS: paired video component records, candidate registration and incomplete control receipts'
