# SPDX-License-Identifier: MIT
# Bounded resource-only KMD/ACPI trial. The installed activation implementation
# and exact original userspace registration are retained. No NVDEC DMA starts.
param([Parameter(Mandatory=$true)]
 [ValidateSet('Prepare','StageCandidate','SelectCandidate','FinishCandidate','Recover','SelectBaseline','FinishBaseline','Inspect')]
 [string]$Action)
$ErrorActionPreference='Stop';$ProgressPreference='SilentlyContinue'
[Console]::OutputEncoding=[Text.UTF8Encoding]::new($false)
$root=$PSScriptRoot
$config=Get-Content (Join-Path $root 'pair-config.json') -Raw|ConvertFrom-Json
$active=$config.BaselineDirectory
$candidate=Join-Path $root 'activation'
$device='ACPI\SWOA0F01\0'
$service='HKLM:\SYSTEM\CurrentControlSet\Services\tegragpurender'
$statePath=Join-Path $root 'pair-state.json'
. (Join-Path $active 'Activation-State.ps1')
$lock=Enter-ActivationStateLock $statePath
function Read-Json([string]$Path){[IO.File]::ReadAllText($Path)|ConvertFrom-Json}
function Save-State($Value){
 $Value['Updated']=[DateTime]::UtcNow.ToString('o');$Value['LastAction']=$Action
 Write-ActivationJson $statePath $Value
}
function Device-Property([string]$Name){(Get-PnpDeviceProperty -InstanceId $device -KeyName ('DEVPKEY_Device_'+$Name)).Data}
function File-Record([string]$Path){@{Length=(Get-Item -LiteralPath $Path).Length;Sha256=(Get-FileHash -LiteralPath $Path).Hash}}
function Assert-File([string]$Path,$Expected){$v=File-Record $Path;if($v.Length -ne $Expected.Length -or $v.Sha256 -ne $Expected.Sha256){throw ('File differs: '+$Path)}}
function Durable-Copy([string]$From,[string]$To){
 $null=New-Item -ItemType Directory -Path (Split-Path $To) -Force
 Copy-Item -LiteralPath $From -Destination $To -Force
 $f=[IO.FileStream]::new($To,[IO.FileMode]::Open,[IO.FileAccess]::ReadWrite,[IO.FileShare]::Read)
 try{$f.Flush($true)}finally{$f.Dispose()}
 Assert-File $To (File-Record $From)
}
function Task-Root {$s=New-Object -ComObject Schedule.Service;$s.Connect();$s.GetFolder('\')}
function Read-Boot { (Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToUniversalTime().ToString('o') }
function Assert-Input {
 foreach($entry in $config.InputFiles.PSObject.Properties){Assert-File (Join-Path $root $entry.Name) $entry.Value}
 $verified=Read-Json (Join-Path $root 'signature-result.json')
 if($verified.Phase -ne 'Verified' -or @($verified.Files).Count -ne 26 -or @($verified.Files|Where-Object Exit -ne 0).Count){throw 'Both complete signed packages require native verification'}
 foreach($f in $verified.Files){Assert-File (Join-Path $root ($f.Directory+'\'+$f.Name)) @{Length=$f.Bytes;Sha256=$f.Sha256}}
}
function Assert-RetainedFiles([switch]$Baseline) {
 foreach($entry in $config.BaselineFiles.PSObject.Properties){
  if(!$Baseline -and $entry.Name -in @("$env:windir\System32\drivers\tegragpurender.sys","$env:windir\System32\acpitabl.dat")){continue}
  Assert-File $entry.Name $entry.Value
 }
}
function Assert-Power {
 Add-Type -AssemblyName System.Windows.Forms
 if([string][Windows.Forms.SystemInformation]::PowerStatus.PowerLineStatus -ne 'Online'){throw 'External power is not online'}
}
function Close-Gate {
 $folder=Task-Root;$task=$folder.GetTask('Tgd3dDefaultEnable')
 if($task.State -in @(2,4)){throw 'Existing activation is still running or queued'}
 $task.Enabled=$false
 $k=[Microsoft.Win32.Registry]::LocalMachine.OpenSubKey('SYSTEM\CurrentControlSet\Services\tegragpurender',$true)
 try{$k.DeleteSubKey('AllowNativeBoot',$false);$k.DeleteSubKey('AllowDesktopStart',$false);$k.Flush()}finally{$k.Dispose()}
 Remove-Item "$env:windir\System32\drivers\TgGpuBootTest158.bin" -Force -ErrorAction SilentlyContinue
}
function Assert-Closed {
 if((Device-Property 'ProblemCode') -ne 43 -or (Task-Root).GetTask('Tgd3dDefaultEnable').Enabled){throw 'Native startup must be closed after the planned boot'}
 $p=Get-ItemProperty ($service+'\Parameters')
 if([uint64]$p.StartGateStatus -ne 3221225635 -or [uint64]$p.HardwareStatus -ne 4294967295){throw 'Closed-gate status differs'}
 if((Test-Path ($service+'\AllowNativeBoot')) -or (Test-Path ($service+'\AllowDesktopStart')) -or (Test-Path "$env:windir\System32\drivers\TgGpuBootTest158.bin")){throw 'A native start is already armed'}
}
function Select-Package([string]$Directory,$Expected,[string]$Receipt) {
 Assert-Closed
 if(Test-Path $Receipt){throw 'Package selection already attempted; inspect its receipt'}
 if((File-Record "$env:windir\System32\drivers\tegragpurender.sys").Sha256 -eq $Expected.Sha256){return}
 if(!('NvdecPairDriverApi' -as [type])){Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class NvdecPairDriverApi {
 [DllImport("newdev.dll",EntryPoint="UpdateDriverForPlugAndPlayDevicesW",CharSet=CharSet.Unicode,SetLastError=true)]
 [return:MarshalAs(UnmanagedType.Bool)] public static extern bool Update(IntPtr p,string id,string inf,uint flags,out bool reboot);
}
'@}
 $r=[ordered]@{Begin=[DateTime]::UtcNow.ToString('o');Phase='Selecting';Before=File-Record "$env:windir\System32\drivers\tegragpurender.sys";Target=$Expected}
 Write-ActivationJson $Receipt $r
 $reboot=$false;$ok=[NvdecPairDriverApi]::Update([IntPtr]::Zero,'ACPI\SWOA0F01',(Join-Path $Directory 'tegragpurender.inf'),5,[ref]$reboot)
 $errorCode=if($ok){0}else{[Runtime.InteropServices.Marshal]::GetLastWin32Error()}
 $r.Phase=if($ok){'Selected'}else{'Failed'};$r.ErrorCode=$errorCode;$r.RebootRequired=$reboot;$r.End=[DateTime]::UtcNow.ToString('o')
 $r.Actual=File-Record "$env:windir\System32\drivers\tegragpurender.sys";$r.Code=Device-Property 'ProblemCode';Write-ActivationJson $Receipt $r
 if(!$ok -or $reboot){throw ('Driver selection failed or requires another boot: '+$errorCode)}
 Assert-File "$env:windir\System32\drivers\tegragpurender.sys" $Expected;Assert-Closed
}
function Restore-Registration($Backup){
 $subkey='SYSTEM\CurrentControlSet\Control\Class\'+(Device-Property 'Driver')
 if($subkey -ne $Backup.Registration.Subkey){throw 'Display class key changed; inspect before writing registration'}
 $k=[Microsoft.Win32.Registry]::LocalMachine.OpenSubKey($subkey,$true)
 try{$k.SetValue('UserModeDriverName',[string[]]$Backup.Registration.Umd,[Microsoft.Win32.RegistryValueKind]::MultiString);$k.SetValue('VulkanDriverName',[string]$Backup.Registration.Icd,[Microsoft.Win32.RegistryValueKind]::String);$k.Flush()}finally{$k.Dispose()}
}
function Install-DefaultTask([string]$Xml,[string]$Directory) {
 $folder=Task-Root;$null=$folder.RegisterTask('Tgd3dDefaultEnable',$Xml,6,'SYSTEM',$null,5,$null)
 New-ItemProperty ($service+'\Tgd3dDefault') Directory -PropertyType String -Value $Directory -Force|Out-Null
 $task=$folder.GetTask('Tgd3dDefaultEnable');$task.Enabled=$true
 [xml]$actual=$task.Xml
 if($actual.Task.Actions.Exec.Arguments -notlike ('*"'+$Directory+'\Default-Start.ps1"*') -or $actual.Task.Triggers.LogonTrigger.Delay -ne 'PT5S'){throw 'Default task action/delay differs'}
}
try {
 $identity=[Security.Principal.WindowsIdentity]::GetCurrent()
 if(!([Security.Principal.WindowsPrincipal]::new($identity)).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)){throw 'Administrator context required'}
 $state=$null
 if(Test-Path $statePath){
  $state=[ordered]@{}
  foreach($entry in (Read-Json $statePath).PSObject.Properties){$state[$entry.Name]=$entry.Value}
 }
 if($Action -eq 'Inspect') {
  $r=[ordered]@{Utc=[DateTime]::UtcNow.ToString('o');State=$state;Code=Device-Property 'ProblemCode';Active=[string](Get-ItemProperty ($service+'\Tgd3dDefault')).Directory;Kmd=File-Record "$env:windir\System32\drivers\tegragpurender.sys";Acpi=File-Record "$env:windir\System32\acpitabl.dat";Dwm=@(Get-Process dwm -ErrorAction SilentlyContinue|ForEach-Object{@{Id=$_.Id;Start=$_.StartTime.ToUniversalTime().ToString('o')}})}
  foreach($name in @('default-state.json','default-attempt.json')){if(Test-Path (Join-Path $candidate $name)){$r[$name]=Read-Json (Join-Path $candidate $name)}}
  $r|ConvertTo-Json -Depth 10 -Compress;return
 }
 Assert-Input
 if($Action -eq 'Prepare') {
  if($state -or (Test-Path $candidate)){throw 'Preparation already attempted; inspect it'}
  Assert-RetainedFiles -Baseline;Assert-Power
  if((Device-Property 'ProblemCode') -ne 0 -or [string](Get-ItemProperty ($service+'\Tgd3dDefault')).Directory -ne $active){throw 'Current baseline is not active'}
  $folder=Task-Root;$task=$folder.GetTask('Tgd3dDefaultEnable')
  if(!$task.Enabled -or $task.State -ne 3){throw 'Default activation is not settled'}
  $original=Read-Json (Join-Path $active 'trial-components.json')
  foreach($entry in $original.files.PSObject.Properties){Assert-File (Join-Path $active $entry.Name) $entry.Value}
  # Historical manifests can contain repeated separators. Canonicalize keys
  # before replacing only the candidate package/table records.
  $normalized=[ordered]@{}
  foreach($entry in $original.files.PSObject.Properties){
   $name=$entry.Name.Replace('\\','\')
   if($normalized.Contains($name)){throw 'Duplicate normalized manifest path'}
   $normalized[$name]=$entry.Value
  }
  $original.files=[pscustomobject]$normalized
  $recovery=Read-Json (Join-Path $active 'recovery.json')
  foreach($entry in $recovery.Files.PSObject.Properties){Assert-File (Join-Path $active $entry.Name) $entry.Value}
  $subkey='SYSTEM\CurrentControlSet\Control\Class\'+(Device-Property 'Driver');$k=[Microsoft.Win32.Registry]::LocalMachine.OpenSubKey($subkey)
  try{$registration=@{Subkey=$subkey;Umd=@($k.GetValue('UserModeDriverName'));Icd=$k.GetValue('VulkanDriverName')}}finally{$k.Dispose()}
  if($registration.Umd.Count -ne 4 -or $registration.Umd[1] -ne (Join-Path $active 'app\TegraGpuD3DVk.dll') -or $registration.Umd[2] -ne $registration.Umd[1]){throw 'Userspace registration differs'}
  $backup=[ordered]@{Utc=[DateTime]::UtcNow.ToString('o');Boot=Read-Boot;Directory=$active;TaskXml=$task.Xml;Registration=$registration;User=$recovery.User}
  $backup.Power=& (Join-Path $root 'Power-State.ps1');$backup.Fan=& (Join-Path $root 'Power-State.ps1') -FanOnly;$backup.Emc=& (Join-Path $root 'Power-State.ps1') -EmcOnly
  if($backup.Power.Power.PowerMode -ne 1 -or $backup.Power.Battery.LimitCenter -ne 85 -or $backup.Fan.Fan.Mode -ne 1){throw 'Handheld85/fan policy differs'}
  Write-ActivationJson (Join-Path $root 'pair-backup.json') $backup
  $state=[ordered]@{Phase='Preparing';Begin=$backup.Utc;Boot=$backup.Boot;RestartRequested=$false};Save-State $state
  foreach($entry in $original.files.PSObject.Properties){Durable-Copy (Join-Path $active $entry.Name) (Join-Path $candidate $entry.Name)}
  foreach($entry in $recovery.Files.PSObject.Properties){Durable-Copy (Join-Path $active $entry.Name) (Join-Path $candidate $entry.Name)}
  foreach($name in @('recovery.json','icd-registration.json')){Durable-Copy (Join-Path $active $name) (Join-Path $candidate $name)}
  foreach($f in Get-ChildItem (Join-Path $root 'package') -File){Durable-Copy $f.FullName (Join-Path $candidate ('package\'+$f.Name));$record=File-Record $f.FullName;$original.kmd.($f.Name)=$record;$original.files.('package\'+$f.Name)=$record}
  Durable-Copy (Join-Path $root 'candidate-acpi.dat') (Join-Path $candidate 'display158.aml')
  $original.helpers.'display158.aml'=File-Record (Join-Path $candidate 'display158.aml');$original.files.'display158.aml'=$original.helpers.'display158.aml'
  $original.kmd_source=$config.CandidateSource
  $original|Add-Member NoteProperty nvdec_resource_scope 'Startup resource snapshot only; decode unavailable' -Force
  Write-ActivationJson (Join-Path $candidate 'trial-components.json') $original
  & icacls.exe $candidate /inheritance:r /grant:r '*S-1-5-18:(OI)(CI)(F)' '*S-1-5-32-544:(OI)(CI)(F)' '*S-1-5-32-545:(OI)(CI)(RX)' /Q|Out-Null
  if($LASTEXITCODE){throw 'Candidate ACL failed'}
  $check=& { . (Join-Path $candidate 'Common.ps1');Check-Package }
  if($check.kmd.'tegragpurender.sys'.Sha256 -ne $config.CandidateKmd.Sha256){throw 'Candidate activation package differs'}
  $state.Phase='Prepared';$state.End=[DateTime]::UtcNow.ToString('o');Save-State $state
 }
 elseif($Action -eq 'StageCandidate') {
  if(!$state -or $state.Phase -ne 'Prepared'){throw 'Expected a prepared, never-staged pair'}
  Assert-RetainedFiles -Baseline;Assert-Power
  $backup=Read-Json (Join-Path $root 'pair-backup.json')
  if((Read-Boot) -ne $backup.Boot -or (Device-Property 'ProblemCode') -ne 0){throw 'Prepared native state changed'}
  & pnputil.exe /add-driver (Join-Path $root 'package\tegragpurender.inf')|Out-File (Join-Path $root 'candidate-staged.log')
  if($LASTEXITCODE){throw 'Candidate driver-store staging failed'}
  $state.Phase='ClosingGate';Save-State $state
  Close-Gate
  Durable-Copy (Join-Path $root 'candidate-acpi.dat') "$env:windir\System32\acpitabl.dat"
  $state.Phase='CandidateRebootRequested';$state.Boot=Read-Boot;$state.RestartRequested=$true;Save-State $state
  & shutdown.exe /r /t 10 /d p:0:0 /c 'Load NVDEC optional resources with native start closed'
  if($LASTEXITCODE){throw 'Planned restart was rejected'}
 }
 elseif($Action -eq 'SelectCandidate') {
  if(!$state -or $state.Phase -ne 'CandidateRebootRequested' -or (Read-Boot) -eq $state.Boot){throw 'Expected the completed candidate ACPI boot'}
  Assert-RetainedFiles;Assert-Power;Assert-Closed
  Assert-File "$env:windir\System32\acpitabl.dat" $config.CandidateAcpi
  # Capture the inherited platform state before the new KMD can read its
  # optional resources. This separates an earlier boot/ACTMON failure from
  # a candidate activation regression; the snapshot does not set policy.
  Write-ActivationJson (Join-Path $root 'before-candidate-selection.json') @{
   Utc=[DateTime]::UtcNow.ToString('o');Code=Device-Property 'ProblemCode'
   Power=(& (Join-Path $root 'Power-State.ps1'))
   Emc=(& (Join-Path $root 'Power-State.ps1') -EmcOnly)
  }
  $state.Phase='SelectingCandidate';Save-State $state
  Select-Package (Join-Path $root 'package') $config.CandidateKmd (Join-Path $root 'candidate-selection.json')
  $backup=Read-Json (Join-Path $root 'pair-backup.json');Restore-Registration $backup
  [xml]$taskDocument=$backup.TaskXml
  if(@($taskDocument.Task.Actions.Exec).Count -ne 1){throw 'Unexpected original default task actions'}
  $taskDocument.Task.Actions.Exec.Arguments='-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "'+$candidate+'\Default-Start.ps1"'
  Install-DefaultTask $taskDocument.OuterXml $candidate
  $state.Phase='CandidateActivationRequested';$state.ActivationBoot=Read-Boot;Save-State $state
  $null=(Task-Root).GetTask('Tgd3dDefaultEnable').Run($null)
 }
 elseif($Action -eq 'FinishCandidate') {
  if(!$state -or $state.Phase -ne 'CandidateActivationRequested' -or (Read-Boot) -ne $state.ActivationBoot){throw 'Unexpected activation state or boot'}
  $a=Read-Json (Join-Path $candidate 'default-state.json')
  if($a.Status -ne 'Enabled' -or (Test-Path (Join-Path $candidate 'default-attempt.json')) -or (Device-Property 'ProblemCode') -ne 0){throw 'Candidate activation has not completed'}
  Assert-RetainedFiles;Assert-File "$env:windir\System32\drivers\tegragpurender.sys" $config.CandidateKmd;Assert-File "$env:windir\System32\acpitabl.dat" $config.CandidateAcpi
  $result=& (Join-Path $root 'nvdec-resource-query.exe');$exit=$LASTEXITCODE
  [IO.File]::WriteAllText((Join-Path $root 'resource-query.txt'),($result -join "`n"),[Text.UTF8Encoding]::new($false))
  if($exit){throw ('Resource query failed: '+$exit)}
  $query=($result -join "`n")|ConvertFrom-Json
  if($query.matching_adapters -ne 1 -or $query.supported_queries -ne 1){throw 'Unexpected resource query identity'}
  $state.Query=$query;$state.Phase='CandidateObserved';$state.End=[DateTime]::UtcNow.ToString('o');Save-State $state
 }
 elseif($Action -eq 'Recover') {
  if(!$state -or $state.Phase -in @('Prepared','Preparing','BaselineRebootRequested','SelectingBaseline','BaselineActivationRequested','Restored')){throw 'No eligible new recovery request'}
  $state.Phase='ClosingForRecovery';Save-State $state
  Close-Gate
  Durable-Copy (Join-Path $root 'baseline-acpi.dat') "$env:windir\System32\acpitabl.dat"
  $state.Phase='BaselineRebootRequested';$state.Boot=Read-Boot;$state.RestartRequested=$true;Save-State $state
  & shutdown.exe /r /t 10 /d p:0:0 /c 'Restore the retained E95 and original ACPI pair'
  if($LASTEXITCODE){throw 'Recovery restart was rejected'}
 }
 elseif($Action -eq 'SelectBaseline') {
  if(!$state -or $state.Phase -ne 'BaselineRebootRequested' -or (Read-Boot) -eq $state.Boot){throw 'Expected completed recovery ACPI boot'}
  Assert-Closed;Assert-File "$env:windir\System32\acpitabl.dat" $config.BaselineAcpi
  $state.Phase='SelectingBaseline';Save-State $state
  Select-Package (Join-Path $root 'baseline-kmd') $config.BaselineKmd (Join-Path $root 'baseline-selection.json')
  $backup=Read-Json (Join-Path $root 'pair-backup.json');Restore-Registration $backup
  Install-DefaultTask $backup.TaskXml $active
  $state.Phase='BaselineActivationRequested';$state.ActivationBoot=Read-Boot;Save-State $state
  $null=(Task-Root).GetTask('Tgd3dDefaultEnable').Run($null)
 }
 elseif($Action -eq 'FinishBaseline') {
  if(!$state -or $state.Phase -ne 'BaselineActivationRequested' -or (Read-Boot) -ne $state.ActivationBoot){throw 'Unexpected recovery activation state'}
  $a=Read-Json (Join-Path $active 'default-state.json')
  if($a.Status -ne 'Enabled' -or (Test-Path (Join-Path $active 'default-attempt.json')) -or (Device-Property 'ProblemCode') -ne 0){throw 'Baseline activation has not completed'}
  Assert-RetainedFiles -Baseline
  $state.Phase='Restored';$state.End=[DateTime]::UtcNow.ToString('o');Save-State $state
 }
 $state|ConvertTo-Json -Depth 12 -Compress
}catch{
 $errorRecord=@{Utc=[DateTime]::UtcNow.ToString('o');Action=$Action;State=$state;Error=$_.Exception.Message;Detail=($_|Out-String)}
 Write-ActivationJson (Join-Path $root ('error-'+$Action+'.json')) $errorRecord
 throw
}finally{$lock.Dispose()}
