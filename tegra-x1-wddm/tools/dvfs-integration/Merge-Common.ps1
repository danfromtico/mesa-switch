# SPDX-License-Identifier: MIT
$ErrorActionPreference='Stop';$ProgressPreference='SilentlyContinue'
$MergeRoot=$PSScriptRoot
. (Join-Path $MergeRoot 'gpu\Common.ps1')
Require-Administrator
function Merge-Check {
 $m=Read-Json (Join-Path $MergeRoot 'merge-manifest.json')
 foreach($entry in $m.Files.PSObject.Properties){Check-File (Join-Path $MergeRoot $entry.Name) $entry.Value}
 $m
}
function Copy-Durable([string]$Source,[string]$Target) {
 Copy-Item -LiteralPath $Source -Destination $Target -Force
 $f=[IO.FileStream]::new($Target,[IO.FileMode]::Open,[IO.FileAccess]::ReadWrite,[IO.FileShare]::Read)
 try{$f.Flush($true)}finally{$f.Dispose()}
 if((File-Hash $Source) -ne (File-Hash $Target)){throw ('Copy differs: '+$Target)}
}
function Sync-PackageFiles([string]$Directory,$Records,[string]$ManifestName) {
 # A cached hash is not proof of persistence across a reset. Flush every
 # checked stream, including NTFS resident files and the manifest itself.
 foreach($name in @($Records.PSObject.Properties.Name)+@($ManifestName)) {
  $path=Join-Path $Directory $name
  $f=[IO.FileStream]::new($path,[IO.FileMode]::Open,[IO.FileAccess]::ReadWrite,[IO.FileShare]::Read)
  try{$f.Flush($true)}finally{$f.Dispose()}
 }
 foreach($entry in $Records.PSObject.Properties){Check-File (Join-Path $Directory $entry.Name) $entry.Value}
}
function Close-NativeGate {
 if(Get-ScheduledTask Tgd3dDefaultEnable -ErrorAction SilentlyContinue){Disable-ScheduledTask Tgd3dDefaultEnable|Out-Null}
 $s=[Microsoft.Win32.Registry]::LocalMachine.OpenSubKey('SYSTEM\CurrentControlSet\Services\tegragpurender',$true)
 try{$s.DeleteSubKey('AllowNativeBoot',$false);$s.DeleteSubKey('AllowDesktopStart',$false);$s.Flush()}finally{$s.Dispose()}
 Remove-Item "$env:windir\System32\drivers\TgGpuBootTest158.bin" -Force -ErrorAction SilentlyContinue
}
function Register-MergeTask([string]$Name,[string]$File,[string]$User) {
 $action=New-ScheduledTaskAction -Execute "$env:windir\System32\WindowsPowerShell\v1.0\powershell.exe" -Argument ('-NoProfile -NonInteractive -ExecutionPolicy Bypass -File "'+$MergeRoot+'\'+$File+'"')
 $trigger=New-ScheduledTaskTrigger -AtLogOn -User $User;$trigger.Delay='PT15S'
 $settings=New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -MultipleInstances IgnoreNew -ExecutionTimeLimit (New-TimeSpan -Minutes 12)
 Register-ScheduledTask -TaskName $Name -Action $action -Trigger $trigger -Settings $settings -User SYSTEM -RunLevel Highest -Force|Out-Null
}
function Stage-PlatformDriver([string]$Name) {
 # Stage only: no /install, device restart or policy mutation. Windows may
 # reuse an existing package for identical INF metadata despite a changed SYS.
 # Prove the exact candidate reached DriverStore before changing boot files.
 $directory=Join-Path $MergeRoot ('platform\'+$Name)
 $inf=Join-Path $directory ($Name+'.inf');$sys=Join-Path $directory ($Name+'.sys')
 & pnputil.exe /add-driver $inf|Out-File (Join-Path $MergeRoot ($Name+'-stage.log'))
 if($LASTEXITCODE){throw ('Cannot stage platform driver: '+$Name)}
 $expectedInf=File-Hash $inf;$expectedSys=File-Hash $sys
 $store=Join-Path $env:windir 'System32\DriverStore\FileRepository'
 $matches=@(Get-ChildItem -LiteralPath $store -Directory -Filter ($Name+'.inf_arm64_*')|Where-Object {
  $storedInf=Join-Path $_.FullName ($Name+'.inf');$storedSys=Join-Path $_.FullName ($Name+'.sys')
  (Test-Path $storedSys) -and (File-Hash $storedInf) -eq $expectedInf -and (File-Hash $storedSys) -eq $expectedSys
 })
 if($matches.Count -ne 1){throw ('Exact platform package missing/ambiguous in DriverStore: '+$Name)}
 Save-Json (Join-Path $MergeRoot ($Name+'-staged.json')) @{
  Utc=[DateTime]::UtcNow.ToString('o');Directory=$matches[0].FullName;InfSha256=$expectedInf;SysSha256=$expectedSys;Installed=$false
 }
}
function Update-PlatformDriver([string]$Name,[string]$HardwareId,[string]$Inf,[string]$Expected) {
 if((Device-Property 'ProblemCode') -ne 43){throw 'Platform update requires the closed GPU gate'}
 if(!('DvfsDriverSelection' -as [type])) {
 Add-Type @'
using System;using System.Runtime.InteropServices;
public static class DvfsDriverSelection {
 [DllImport("newdev.dll",EntryPoint="UpdateDriverForPlugAndPlayDevicesW",CharSet=CharSet.Unicode,SetLastError=true)]
 [return:MarshalAs(UnmanagedType.Bool)] public static extern bool Update(IntPtr parent,string hardwareId,string inf,uint flags,out bool reboot);
}
'@
 }
 $live=Join-Path $env:windir ('System32\drivers\'+$Name+'.sys')
 if((File-Hash $live) -eq $Expected){return [pscustomobject]@{Name=$Name;AlreadySelected=$true;RebootRequired=$false}}
 $reboot=$false;$ok=[DvfsDriverSelection]::Update([IntPtr]::Zero,$HardwareId,$Inf,5,[ref]$reboot)
 $errorCode=if($ok){0}else{[Runtime.InteropServices.Marshal]::GetLastWin32Error()}
 $result=[pscustomobject]@{Name=$Name;Utc=[DateTime]::UtcNow.ToString('o');Success=$ok;Error=$errorCode;RebootRequired=$reboot;Hash=File-Hash $live}
 Save-Json (Join-Path $MergeRoot ($Name+'-selection.json')) $result
 if(!$ok){throw ('Driver selection failed '+$Name+': '+$errorCode)}
 if(!$reboot -and $result.Hash -ne $Expected){throw ('Selected driver hash differs: '+$Name)}
 $result
}
