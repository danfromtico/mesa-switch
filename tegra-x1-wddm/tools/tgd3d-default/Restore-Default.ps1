# SPDX-License-Identifier: MIT
$ErrorActionPreference='Stop'
$settings=Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\tegragpurender\Tgd3dDefault'
$dir=[IO.Path]::GetFullPath([string]$settings.Directory)
$root=[IO.Path]::GetFullPath((Join-Path $env:ProgramData 'SwitchWOA\GpuUpdate')).TrimEnd('\')+'\'
if(!$dir.StartsWith($root,[StringComparison]::OrdinalIgnoreCase)){throw 'Installed directory is outside the protected package root'}
$manifest=[IO.File]::ReadAllText((Join-Path $dir 'trial-components.json'))|ConvertFrom-Json
if($manifest.kmd_variant){
    . (Join-Path $dir 'Common.ps1')
    Require-Administrator
    if(Get-ScheduledTask Tgd3dDefaultEnable -ErrorAction SilentlyContinue){Disable-ScheduledTask Tgd3dDefaultEnable|Out-Null}
    $grant='HKLM:\SYSTEM\CurrentControlSet\Services\tegragpurender\AllowDesktopStart'
    if(Test-Path $grant){Remove-Item $grant -Force}
    $native='HKLM:\SYSTEM\CurrentControlSet\Services\tegragpurender\AllowNativeBoot'
    if(Test-Path $native){Remove-Item $native -Force}
    $svc=[Microsoft.Win32.Registry]::LocalMachine.OpenSubKey('SYSTEM\CurrentControlSet\Services\tegragpurender',$true)
    try{$svc.Flush()}finally{$svc.Dispose()}
    $permit=Join-Path $env:windir 'System32\drivers\TgGpuBootTest158.bin'
    if(Test-Path $permit){Remove-Item $permit -Force}
    if((Device-Property 'ProblemCode') -ne 43){
        Save-Json (Join-Path $dir 'restore-staged.json') @{Utc=[DateTime]::UtcNow.ToString('o');NativeStartupDisabled=$true;ManualRestartRequired=$true;OsRebootRequested=$false}
        'Native startup is disabled. Restart Windows, then run RESTORE again to restore the package baseline .158 kernel.'
        return
    }
    & (Join-Path $dir 'Switch-Kmd.ps1') -Direction Baseline
    $subkey='SYSTEM\CurrentControlSet\Control\Class\'+(Device-Property 'Driver')
    $key=[Microsoft.Win32.Registry]::LocalMachine.OpenSubKey($subkey,$true)
    try{
        $slots=@($key.GetValue('UserModeDriverName'));$stub=$slots[0]
        Check-File $stub $manifest.baseline_kmd.'tegragpuno3d.dll'
        $key.SetValue('UserModeDriverName',[string[]]@($stub,$stub,$stub,$stub),[Microsoft.Win32.RegistryValueKind]::MultiString)
        $key.DeleteValue('VulkanDriverName',$false)
    }finally{$key.Dispose()}
    Save-Json (Join-Path $dir 'restore-result.json') @{Utc=[DateTime]::UtcNow.ToString('o');Version=(Device-Property 'DriverVersion');Problem=(Device-Property 'ProblemCode');KernelHash=(File-Hash "$env:windir\System32\drivers\tegragpurender.sys");OsRebootRequested=$false}
    'RESTORED: package baseline .158 kernel, native gate closed; no .59 installation or reboot requested.'
    return
}
& (Join-Path $dir 'Restore-AfterBoot.ps1')
