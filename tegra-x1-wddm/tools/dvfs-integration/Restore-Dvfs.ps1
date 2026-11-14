# SPDX-License-Identifier: MIT
# Restore the exact pre-integration platform/display pairing, retaining .158.
. (Join-Path $PSScriptRoot 'Merge-Common.ps1')
$backup=Read-Json (Join-Path $MergeRoot 'backup.json')
foreach($entry in $backup.Files.PSObject.Properties){Check-File (Join-Path $MergeRoot $entry.Name) $entry.Value}
$state=Join-Path $MergeRoot 'restore-state.json'
$boot=(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToUniversalTime().ToString('o')
$r=if(Test-Path $state){Read-Json $state}else{$null}
if($r -and $r.Phase -eq 'Restored'){return}
if(!$r -or $r.Phase -ne 'RebootRequested') {
 $fan=Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\switchfan\Parameters'
 if($fan.DriverVersion -in @(15,16,17,18)){$null=& (Join-Path $MergeRoot 'Power-State.ps1') -Mode 7 -PowerOnly}
 Close-NativeGate
 Disable-ScheduledTask Tgd3dDvfsInstall -ErrorAction SilentlyContinue|Out-Null
 Copy-Durable (Join-Path $MergeRoot 'backup\bl33.bin') $backup.FirmwarePath
 Copy-Durable (Join-Path $MergeRoot 'backup\acpitabl.dat') "$env:windir\System32\acpitabl.dat"
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
 foreach($item in $backup.Drivers){$selected=Update-PlatformDriver $item.Name $item.HardwareId (Join-Path $MergeRoot $item.Inf) $item.Hash;if($selected.RebootRequired){throw ('Recovery driver restart required: '+$item.Name)}}
 $install=Read-Json (Join-Path $MergeRoot 'install-state.json')
 if($install.CandidateDirectory){& (Join-Path $install.CandidateDirectory 'Switch-Kmd.ps1') -Direction Baseline|Out-File (Join-Path $MergeRoot 'restore-kmd.log')}
 foreach($item in $backup.Registry){$key=[Microsoft.Win32.Registry]::LocalMachine.CreateSubKey($item.Path);try{if($item.Present){$key.SetValue($item.Name,[int]$item.Value,[Microsoft.Win32.RegistryValueKind]::DWord)}else{$key.DeleteValue($item.Name,$false)}}finally{$key.Dispose()}}
 $registration=Read-Json (Join-Path $MergeRoot 'backup\registration.json');$key=[Microsoft.Win32.Registry]::LocalMachine.OpenSubKey($registration.Subkey,$true)
 try{$key.SetValue('UserModeDriverName',[string[]]$registration.Umd,[Microsoft.Win32.RegistryValueKind]::MultiString);if($registration.Icd){$key.SetValue('VulkanDriverName',[string]$registration.Icd,[Microsoft.Win32.RegistryValueKind]::String)}else{$key.DeleteValue('VulkanDriverName',$false)}}finally{$key.Dispose()}
 New-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\tegragpurender\Tgd3dDefault' Directory -PropertyType String -Value $backup.Directory -Force|Out-Null
 Register-ScheduledTask Tgd3dDefaultEnable -Xml ([IO.File]::ReadAllText((Join-Path $MergeRoot 'backup\default-task.xml'))) -Force|Out-Null
 Start-ScheduledTask Tgd3dDefaultEnable
 $deadline=[DateTime]::UtcNow.AddSeconds(100)
 do {Start-Sleep -Milliseconds 500;$fan=Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\switchfan\Parameters';$task=Get-ScheduledTask Tgd3dDefaultEnable;if($task.State -ne 'Running' -and (Device-Property 'ProblemCode') -eq 0){break}}while([DateTime]::UtcNow -lt $deadline)
 $manifest=Read-Json (Join-Path $MergeRoot 'merge-manifest.json')
 foreach($entry in $manifest.Before.Files.PSObject.Properties){Check-File $entry.Name $entry.Value}
 if((Device-Property 'ProblemCode') -ne 0 -or $fan.DriverVersion -ne 13 -or $fan.CpuActualKhz -ne 1497600 -or $fan.RamActualKhz -ne 1600000){throw 'Recovery platform checks failed'}
 $r.Phase='Restored';$r|Add-Member -NotePropertyName End -NotePropertyValue ([DateTime]::UtcNow.ToString('o')) -Force;$r|Add-Member -NotePropertyName AfterBoot -NotePropertyValue $boot -Force;Save-Json $state $r
}catch{$r.Phase='Failed';$r|Add-Member -NotePropertyName Error -NotePropertyValue $_.Exception.Message -Force;Save-Json $state $r;throw}
