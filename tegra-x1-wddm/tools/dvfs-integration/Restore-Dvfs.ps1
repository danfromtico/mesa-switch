# SPDX-License-Identifier: MIT
# Restore the exact pre-integration platform/display pairing, retaining .158.
. (Join-Path $PSScriptRoot 'Merge-Common.ps1')
$backup=Read-Json (Join-Path $MergeRoot 'backup.json')
foreach($entry in $backup.Files.PSObject.Properties){Check-File (Join-Path $MergeRoot $entry.Name) $entry.Value}
$state=Join-Path $MergeRoot 'restore-state.json'
$boot=(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToUniversalTime().ToString('o')
$r=if(Test-Path $state){Read-Json $state}else{$null}
if($r -and $r.Phase -eq 'Restored'){return}
if($r -and $r.Phase -ne 'RebootRequested'){throw 'Recovery already failed; inspect its saved state before retrying'}
if(!$r) {
 Close-NativeGate
 Disable-ScheduledTask Tgd3dDvfsInstall -ErrorAction SilentlyContinue|Out-Null
 Copy-Durable (Join-Path $MergeRoot 'backup\bl33.bin') $backup.FirmwarePath
 Copy-Durable (Join-Path $MergeRoot 'backup\acpitabl.dat') "$env:windir\System32\acpitabl.dat"
 Copy-Durable (Join-Path $MergeRoot 'backup\boot.ini') 'D:\bootloader\ini\woa-lite-home.ini'
 Register-MergeTask 'Tgd3dDvfsRestore' 'Restore-Dvfs.ps1' $backup.User
 $r=[ordered]@{Utc=[DateTime]::UtcNow.ToString('o');Phase='RebootRequested';Boot=$boot}
 Save-Json $state $r
 & shutdown.exe /r /t 10
 if($LASTEXITCODE){throw 'Windows rejected the recovery restart'}
 return
}
if([Math]::Abs(([DateTime]::Parse($boot)-[DateTime]::Parse($r.Boot)).TotalSeconds) -le 2){throw 'Recovery restart has not completed; do not request another'}
Disable-ScheduledTask Tgd3dDvfsRestore|Out-Null
try {
 if((Device-Property 'ProblemCode') -ne 43){throw 'Recovery requires closed native startup'}
 foreach($item in $backup.Registry){$key=[Microsoft.Win32.Registry]::LocalMachine.CreateSubKey($item.Path);try{if($item.Present){$key.SetValue($item.Name,[int]$item.Value,[Microsoft.Win32.RegistryValueKind]::DWord)}else{$key.DeleteValue($item.Name,$false)}}finally{$key.Dispose()}}
 # Release the new Fan client's VDD_CORE handle before restoring that owner.
 $restoreOrder=@('switchfan','switchpwrbtn','switchbattery')
 if(@($backup.Drivers).Count -ne $restoreOrder.Count){throw 'Unexpected backup driver count'}
 foreach($name in $restoreOrder){
  $items=@($backup.Drivers|Where-Object Name -eq $name);if($items.Count -ne 1){throw ('Missing backup driver: '+$name)}
  $selected=Update-PlatformDriver $items[0].Name $items[0].HardwareId (Join-Path $MergeRoot $items[0].Inf) $items[0].Hash
  if($selected.RebootRequired){throw ('Recovery driver restart required: '+$items[0].Name)}
 }
 $install=Read-Json (Join-Path $MergeRoot 'install-state.json')
 if($install.CandidateDirectory){& (Join-Path $install.CandidateDirectory 'Switch-Kmd.ps1') -Direction Baseline|Out-File (Join-Path $MergeRoot 'restore-kmd.log')}
 $registration=Read-Json (Join-Path $MergeRoot 'backup\registration.json')
 if($registration.Subkey -ne ('SYSTEM\CurrentControlSet\Control\Class\'+(Device-Property 'Driver'))){throw 'Display class subkey changed'}
 $key=[Microsoft.Win32.Registry]::LocalMachine.OpenSubKey($registration.Subkey,$true)
 try{if(!$key){throw 'Missing GPU class registration key'};$key.SetValue('UserModeDriverName',[string[]]$registration.Umd,[Microsoft.Win32.RegistryValueKind]::MultiString);if($registration.Icd){$key.SetValue('VulkanDriverName',[string]$registration.Icd,[Microsoft.Win32.RegistryValueKind]::String)}else{$key.DeleteValue('VulkanDriverName',$false)}}finally{if($key){$key.Dispose()}}
 New-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\tegragpurender\Tgd3dDefault' Directory -PropertyType String -Value $backup.Directory -Force|Out-Null
 Register-ScheduledTask Tgd3dDefaultEnable -Xml ([IO.File]::ReadAllText((Join-Path $MergeRoot 'backup\default-task.xml'))) -Force|Out-Null
 Enable-ScheduledTask Tgd3dDefaultEnable|Out-Null
 Start-ScheduledTask Tgd3dDefaultEnable
 $deadline=[DateTime]::UtcNow.AddSeconds(110)
 do {Start-Sleep -Milliseconds 500;$task=Get-ScheduledTask Tgd3dDefaultEnable;if($task.State -eq 'Disabled' -or ($task.State -notin @('Running','Queued') -and (Device-Property 'ProblemCode') -eq 0)){break}}while([DateTime]::UtcNow -lt $deadline)
 $manifest=Read-Json (Join-Path $MergeRoot 'merge-manifest.json')
 foreach($entry in $manifest.Before.Files.PSObject.Properties){Check-File $entry.Name $entry.Value}
 if((Device-Property 'ProblemCode') -ne 0 -or !(Get-ScheduledTask Tgd3dDefaultEnable).Settings.Enabled){throw 'Recovery native activation failed'}
 $power=& (Join-Path $MergeRoot 'Power-State.ps1') -Mode 1 -ChargeLimit 85
 Save-Json (Join-Path $MergeRoot 'platform-restored.json') $power
 if($power.Power.PowerMode -ne 1 -or !$power.Power.CpuDfllActive -or $power.Power.CpuLastError -ne 0 -or $power.Power.GpuPolicyMode -ne 1 -or $power.Battery.LimitCenter -ne 85 -or $power.Battery.BandMin -ne 80 -or $power.Battery.BandMax -ne 90 -or $power.Battery.LastApplyStatus -ne 0){throw 'Recovery Handheld/85% checks failed'}
 $r.Phase='Restored';$r|Add-Member -NotePropertyName End -NotePropertyValue ([DateTime]::UtcNow.ToString('o')) -Force;$r|Add-Member -NotePropertyName AfterBoot -NotePropertyValue $boot -Force;Save-Json $state $r
}catch{$r.Phase='Failed';$r|Add-Member -NotePropertyName Error -NotePropertyValue $_.Exception.Message -Force;Save-Json $state $r;throw}
