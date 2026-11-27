# SPDX-License-Identifier: MIT
. (Join-Path $PSScriptRoot 'Merge-Common.ps1')
$m=Merge-Check
Sync-PackageFiles $MergeRoot $m.Files 'merge-manifest.json'
$state=Join-Path $MergeRoot 'install-state.json'
if(Test-Path $state){throw 'Installation was already staged; inspect its recorded state'}
$active=[IO.Path]::GetFullPath((Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\tegragpurender\Tgd3dDefault').Directory)
if($active -ne $m.Before.Directory -or (Device-Property 'ProblemCode') -ne 0){throw 'Active GPU baseline differs'}
foreach($entry in $m.Before.Files.PSObject.Properties){Check-File $entry.Name $entry.Value}
$user=(Get-CimInstance Win32_ComputerSystem).UserName
if(!$user){throw 'No interactive logon to preserve'}
if(!(Get-ScheduledTask Tgd3dDefaultEnable).Settings.Enabled){throw 'Default login activation is disabled'}
$power=& (Join-Path $MergeRoot 'Power-State.ps1')
if($power.Power.PowerMode -ne 1 -or $power.Battery.LimitCenter -ne 85 -or $power.Battery.BandMin -ne 80 -or $power.Battery.BandMax -ne 90 -or $power.Battery.LastApplyStatus -ne 0){throw 'Expected the current Handheld/85% baseline before staging'}
foreach($name in @('switchpwrbtn','switchfan','switchbattery')){Stage-PlatformDriver $name}
$backup=Join-Path $MergeRoot 'backup';New-Item -ItemType Directory $backup|Out-Null
$r=[ordered]@{Utc=[DateTime]::UtcNow.ToString('o');Phase='BackingUp';User=$user;Directory=$active;Boot=(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToUniversalTime().ToString('o');RestartRequested=$false}
Save-Json $state $r
$mutating=$false
try {
 Export-ScheduledTask Tgd3dDefaultEnable|Set-Content (Join-Path $backup 'default-task.xml') -Encoding Unicode
 $subkey='SYSTEM\CurrentControlSet\Control\Class\'+(Device-Property 'Driver');$key=[Microsoft.Win32.Registry]::LocalMachine.OpenSubKey($subkey)
 try{if(!$key){throw 'Missing GPU class registration key'};$registration=@{Subkey=$subkey;Umd=@($key.GetValue('UserModeDriverName'));Icd=$key.GetValue('VulkanDriverName')}}finally{if($key){$key.Dispose()}}
 Save-Json (Join-Path $backup 'registration.json') $registration
 Copy-Durable $m.Before.FirmwarePath (Join-Path $backup 'bl33.bin')
 Copy-Durable "$env:windir\System32\acpitabl.dat" (Join-Path $backup 'acpitabl.dat')
 Copy-Durable 'D:\bootloader\ini\woa-lite-home.ini' (Join-Path $backup 'boot.ini')
 $drivers=@()
 foreach($item in @(@{Name='switchpwrbtn';HardwareId='ACPI\SWOA0601'},@{Name='switchfan';HardwareId='ACPI\SWOA0402'},@{Name='switchbattery';HardwareId='ACPI\SWOA0314'})) {
  $devices=@(Get-PnpDevice -PresentOnly|Where-Object{$_.InstanceId -like ($item.HardwareId+'\*')})
  if($devices.Count -ne 1){throw ('Expected one '+$item.Name+' device')}
  $id=$devices[0].InstanceId;$oem=(Get-PnpDeviceProperty -InstanceId $id -KeyName DEVPKEY_Device_DriverInfPath).Data
  $dest=Join-Path $backup $item.Name;New-Item -ItemType Directory $dest|Out-Null
  & pnputil.exe /export-driver $oem $dest|Out-File (Join-Path $backup ($item.Name+'-export.log'))
  if($LASTEXITCODE){throw ('Cannot export '+$item.Name)}
  $infs=@(Get-ChildItem $dest -Recurse -Filter ($item.Name+'.inf'));if($infs.Count -ne 1){throw 'Exported INF differs'}
  $sys=Join-Path $infs[0].DirectoryName ($item.Name+'.sys')
  if((File-Hash $sys) -ne (File-Hash "$env:windir\System32\drivers\$($item.Name).sys")){throw 'Exported SYS differs from installed baseline'}
  $drivers+=@{Name=$item.Name;HardwareId=$item.HardwareId;InstanceId=$id;Inf=$infs[0].FullName.Substring($MergeRoot.Length+1);Hash=File-Hash $sys}
 }
 $registry=@()
 foreach($path in @('SYSTEM\CurrentControlSet\Services\tegragpurender\Parameters','SYSTEM\CurrentControlSet\Services\switchbattery\Parameters')) {
  $key=[Microsoft.Win32.Registry]::LocalMachine.OpenSubKey($path)
  try{foreach($name in @('GpuDvfsMode','GpuDvfsMinKhz','GpuDvfsMaxKhz','ChargeLimit')){if(($path -like '*switchbattery*') -eq ($name -eq 'ChargeLimit')){$present=$key -and $key.GetValueNames() -contains $name;$registry+=@{Path=$path;Name=$name;Present=$present;Value=if($present){$key.GetValue($name)}else{$null}}}}}finally{if($key){$key.Dispose()}}
 }
 $files=@{};foreach($file in Get-ChildItem $backup -Recurse -File){$f=[IO.FileStream]::new($file.FullName,[IO.FileMode]::Open,[IO.FileAccess]::ReadWrite,[IO.FileShare]::Read);try{$f.Flush($true)}finally{$f.Dispose()};$files[$file.FullName.Substring($MergeRoot.Length+1)]=@{Length=$file.Length;Sha256=File-Hash $file.FullName}}
 Save-Json (Join-Path $MergeRoot 'backup.json') @{Files=$files;Drivers=$drivers;Registry=$registry;Power=$power;User=$user;Directory=$active;FirmwarePath=$m.Before.FirmwarePath}
 $mutating=$true
 Close-NativeGate
 Register-MergeTask 'Tgd3dDvfsInstall' 'Install-AfterBoot.ps1' $user
 Copy-Durable (Join-Path $MergeRoot 'firmware\bl33.bin') $m.Before.FirmwarePath
 if($m.Files.PSObject.Properties.Name -contains 'firmware\boot.ini') {
  Copy-Durable (Join-Path $MergeRoot 'firmware\boot.ini') 'D:\bootloader\ini\woa-lite-home.ini'
 }
 Copy-Durable (Join-Path $MergeRoot 'gpu\display158.aml') "$env:windir\System32\acpitabl.dat"
 $r.Phase='StagedRebootRequested';$r.RestartRequested=$true;Save-Json $state $r
 & shutdown.exe /r /t 10
 if($LASTEXITCODE){$r.RestartRequested=$false;throw 'Windows rejected the planned restart'}
}catch{
 $r.Phase='Failed';$r.Error=$_.Exception.Message;Save-Json $state $r
 # Only run Restore after backup.json is durably published and gate/boot mutation begins.
 if($mutating){& (Join-Path $MergeRoot 'Restore-Dvfs.ps1')}
 throw
}
