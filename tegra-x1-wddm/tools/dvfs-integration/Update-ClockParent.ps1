# SPDX-License-Identifier: MIT
# Paired Fan/firmware correction; preserves the installed display package.
param([ValidateSet('Stage','AfterBoot','Restore','RestoreAfterBoot')][string]$Action='Stage')
$FixRoot=$PSScriptRoot
. (Join-Path $FixRoot '..\dvfsmerge-20261114\Merge-Common.ps1')
$manifest=Read-Json (Join-Path $FixRoot 'manifest.json')
$statePath=Join-Path $FixRoot 'state.json'
$taskName='Tgd3dDvfsClockParent'
$boot=(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToUniversalTime().ToString('o')
function Assert-Preserved {
 foreach($entry in $manifest.Preserved.PSObject.Properties){Check-File $entry.Name $entry.Value}
 if((Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\tegragpurender\Tgd3dDefault').Directory -ne $manifest.Directory){throw 'Active display directory changed'}
}
function Register-Next([string]$Next,[string]$User) {
 $a=New-ScheduledTaskAction -Execute "$env:windir\System32\WindowsPowerShell\v1.0\powershell.exe" -Argument ('-NoProfile -NonInteractive -ExecutionPolicy Bypass -File "'+$FixRoot+'\Update-ClockParent.ps1" -Action '+$Next)
 $t=New-ScheduledTaskTrigger -AtLogOn -User $User;$t.Delay='PT15S'
 $s=New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -MultipleInstances IgnoreNew -ExecutionTimeLimit (New-TimeSpan -Minutes 8)
 Register-ScheduledTask $taskName -Action $a -Trigger $t -Settings $s -User SYSTEM -RunLevel Highest -Force|Out-Null
}
function Assert-Fixed($p) {
 if($p.Power.PowerMode -ne 7 -or $p.Power.CpuDfllActive -or $p.Power.CpuActualKhz -ne 1497600 -or $p.Power.RamActualKhz -ne 1600000){throw 'Expected fixed CPU1497.6/RAM1600 before activation'}
}
if($Action -in @('Stage','Restore')) {
 if($Action -eq 'Stage' -and (Test-Path $statePath)){throw 'Correction already staged; inspect state before any new action'}
 Sync-PackageFiles $FixRoot $manifest.Files 'manifest.json'
 Assert-Preserved
 if((Device-Property 'ProblemCode') -ne 0){throw 'Expected current native display before the planned platform restart'}
 $p=& (Join-Path $MergeRoot 'Power-State.ps1') -Mode 7;Assert-Fixed $p
 $user=(Get-CimInstance Win32_ComputerSystem).UserName;if(!$user){throw 'No interactive logon'}
 if($Action -eq 'Stage') {
  Check-File $manifest.FirmwarePath $manifest.BeforeFirmware
  Check-File "$env:windir\System32\drivers\switchfan.sys" $manifest.BeforeFan
  $backup=Join-Path $FixRoot 'backup';New-Item -ItemType Directory $backup|Out-Null
  Copy-Durable $manifest.FirmwarePath (Join-Path $backup 'bl33.bin')
  $devices=@(Get-PnpDevice -PresentOnly|Where-Object InstanceId -like 'ACPI\SWOA0402\*')
  if($devices.Count -ne 1){throw 'Expected one SwitchFan device'}
  $oem=(Get-PnpDeviceProperty -InstanceId $devices[0].InstanceId -KeyName DEVPKEY_Device_DriverInfPath).Data
  & pnputil.exe /export-driver $oem $backup|Out-File (Join-Path $backup 'export.log')
  if($LASTEXITCODE){throw 'Cannot export the active Fan package'}
  $infs=@(Get-ChildItem $backup -Recurse -Filter switchfan.inf);if($infs.Count -ne 1){throw 'Exported Fan package differs'}
  Check-File (Join-Path $infs[0].DirectoryName 'switchfan.sys') $manifest.BeforeFan
  $files=@{};foreach($f in Get-ChildItem $backup -Recurse -File){$files[$f.FullName.Substring($FixRoot.Length+1)]=@{Length=$f.Length;Sha256=File-Hash $f.FullName}}
  Save-Json (Join-Path $FixRoot 'backup.json') @{Files=$files;Inf=$infs[0].FullName;User=$user}
  Sync-PackageFiles $FixRoot ([pscustomobject]$files) 'backup.json'
  $firmware=Join-Path $FixRoot 'bl33.bin';$next='AfterBoot'
 }else {
  $backup=Read-Json (Join-Path $FixRoot 'backup.json')
  foreach($entry in $backup.Files.PSObject.Properties){Check-File (Join-Path $FixRoot $entry.Name) $entry.Value}
  $firmware=Join-Path $FixRoot 'backup\bl33.bin';$next='RestoreAfterBoot'
 }
 $r=[ordered]@{Phase='Prepared';Action=$Action;Boot=$boot;User=$user;Utc=[DateTime]::UtcNow.ToString('o');AfterBoot=$null;Before=$p;After=$null;Selection=$null;Modules=@();Error=$null;End=$null}
 Save-Json $statePath $r
 Close-NativeGate
 Register-Next $next $user
 Copy-Durable $firmware $manifest.FirmwarePath
 $r.Phase='RebootRequested';Save-Json $statePath $r
 & shutdown.exe /r /t 10
 if($LASTEXITCODE){throw 'Windows rejected the planned platform restart'}
 return
}
$r=Read-Json $statePath
if($r.Phase -ne 'RebootRequested'){return}
if([Math]::Abs(([DateTime]::Parse($boot)-[DateTime]::Parse($r.Boot)).TotalSeconds) -le 2){throw 'Planned restart not yet complete'}
Disable-ScheduledTask $taskName|Out-Null
$r.AfterBoot=$boot
try {
 Assert-Preserved
 if((Device-Property 'ProblemCode') -ne 43){throw 'Platform selection requires the closed GPU startup gate'}
 if($Action -eq 'AfterBoot') {
  Check-File $manifest.FirmwarePath $manifest.Files.'bl33.bin'
  $inf=Join-Path $FixRoot 'package\switchfan.inf';$sys=$manifest.Files.'package\switchfan.sys';$version=if($manifest.FanVersion){$manifest.FanVersion}else{16}
 }else {
  $backup=Read-Json (Join-Path $FixRoot 'backup.json')
  Check-File $manifest.FirmwarePath $manifest.BeforeFirmware
  $inf=$backup.Inf;$sys=$manifest.BeforeFan;$version=if($manifest.BeforeFanVersion){$manifest.BeforeFanVersion}else{15}
 }
 $r.Phase='SelectingFan';Save-Json $statePath $r
 $r.Selection=Update-PlatformDriver 'switchfan' 'ACPI\SWOA0402' $inf $sys.Sha256
 if($r.Selection.RebootRequired){throw 'Fan selection requested an additional restart; inspect before proceeding'}
 $fan=Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\switchfan\Parameters'
 if($fan.DriverVersion -ne $version){throw 'Runtime Fan version differs'}
 $p=& (Join-Path $MergeRoot 'Power-State.ps1') -Mode 7;Assert-Fixed $p
 if(!$p.Power.CpuDfllAvailable){throw 'Firmware/DFLL handoff unavailable'}
 Save-Json (Join-Path $FixRoot 'before-native.json') $p
 $r.Phase='Activating';Save-Json $statePath $r
 $start=[DateTime]::UtcNow
 Enable-ScheduledTask Tgd3dDefaultEnable|Out-Null
 Start-ScheduledTask Tgd3dDefaultEnable
 Disable-ScheduledTask Tgd3dDefaultEnable|Out-Null
 $limit=[DateTime]::UtcNow.AddSeconds(110);$ok=$false
 do {
  Start-Sleep -Seconds 1
  # The activation task writes this file in place with write-through. A
  # transient sharing violation/partial JSON is not an activation failure.
  try{$a=Read-Json (Join-Path $manifest.Directory 'default-state.json')}catch{continue}
  if($a.End -and $a.Status -eq 'Enabled' -and [DateTime]::Parse($a.End).ToUniversalTime() -ge $start -and (Device-Property 'ProblemCode') -eq 0){$ok=$true;break}
  if($a.Status -eq 'Failed' -and [DateTime]::Parse($a.Utc).ToUniversalTime() -ge $start){throw ('Native activation failed: '+$a.Error)}
 }while([DateTime]::UtcNow -lt $limit)
 if(!$ok){throw 'Native activation did not complete'}
 Assert-Preserved
 $r.Modules=@([Diagnostics.Process]::GetProcessesByName('dwm')|ForEach-Object{$process=$_;foreach($module in $process.Modules){if($module.ModuleName -in @('TegraGpuD3DVk.dll','libvulkan_nouveau.dll')){@{Pid=$process.Id;Name=$module.ModuleName;Path=$module.FileName;Sha256=File-Hash $module.FileName}}}})
 foreach($name in @('TegraGpuD3DVk.dll','libvulkan_nouveau.dll')){if(!($r.Modules|Where-Object{$_.Name -eq $name -and $_.Path -eq (Join-Path $manifest.Directory ('app\'+$name))})){throw ('DWM module missing: '+$name)}}
 $r.After=& (Join-Path $MergeRoot 'Power-State.ps1');Assert-Fixed $r.After
 $r.Phase=if($Action -eq 'AfterBoot'){'NativeEnabled'}else{'Restored'}
 $r.End=[DateTime]::UtcNow.ToString('o');Save-Json $statePath $r
}catch{$r.Phase='Failed';$r.Error=$_.Exception.Message;Save-Json $statePath $r;throw}
