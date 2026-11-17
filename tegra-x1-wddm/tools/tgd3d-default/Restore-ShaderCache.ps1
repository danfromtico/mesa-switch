# SPDX-License-Identifier: MIT
# Restore the prior UMD/ICD registration; leave the KMD and power untouched.
[CmdletBinding()]
param([switch]$Restart)
. (Join-Path $PSScriptRoot 'Common.ps1')
Require-Administrator
$b=Read-Json (Join-Path $PSScriptRoot 'cache-update-backup.json')
$userspace=if($b.PreviousUserspaceDirectory){[string]$b.PreviousUserspaceDirectory}else{[string]$b.PreviousDirectory}
Check-File ($userspace+'\app\TegraGpuD3DVk.dll') @{Length=(Get-Item ($userspace+'\app\TegraGpuD3DVk.dll')).Length;Sha256=$b.PreviousUmd}
if((File-Hash "$env:windir\System32\drivers\tegragpurender.sys") -ne $b.Kernel -or
   (File-Hash ($userspace+'\app\libvulkan_nouveau.dll')) -ne $b.PreviousIcd){throw 'Baseline changed; inspect before restoring'}
$settings='HKLM:\SYSTEM\CurrentControlSet\Services\tegragpurender\Tgd3dDefault'
$active=[string](Get-ItemProperty $settings).Directory
if($active -ne $b.Directory -and $active -ne $b.PreviousDirectory){throw 'A different update owns the startup configuration'}
$key=[Microsoft.Win32.Registry]::LocalMachine.OpenSubKey($b.Subkey,$true)
try{
    $key.SetValue('UserModeDriverName',[string[]]$b.UmdSlots,[Microsoft.Win32.RegistryValueKind]::MultiString)
    $key.SetValue('VulkanDriverName',[string]$b.Icd,[Microsoft.Win32.RegistryValueKind]::String);$key.Flush()
}finally{$key.Dispose()}
Set-ItemProperty $settings Directory $b.PreviousDirectory
Register-ScheduledTask Tgd3dDefaultEnable -Xml ([IO.File]::ReadAllText((Join-Path $PSScriptRoot 'cache-prior-task.xml'))) -Force|Out-Null
$r=@{Utc=[DateTime]::UtcNow.ToString('o');Directory=$b.PreviousDirectory;KernelChanged=$false;IcdChanged=($b.PSObject.Properties.Name -contains 'PairedIcdChanged' -and $b.PairedIcdChanged);OsRebootRequested=[bool]$Restart}
Save-Json (Join-Path $PSScriptRoot 'cache-restore.json') $r;$r|ConvertTo-Json
if($Restart){& shutdown.exe /r /t 10;if($LASTEXITCODE){throw 'Planned restore restart was not accepted'}}
