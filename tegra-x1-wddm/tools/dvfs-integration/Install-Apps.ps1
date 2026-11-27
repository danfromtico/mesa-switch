# SPDX-License-Identifier: MIT
# Run in the existing interactive user's session, not the SYSTEM profile.
$ErrorActionPreference='Stop'
$root=$PSScriptRoot
if([Environment]::UserName -eq 'SYSTEM'){throw 'Install the apps in the interactive user profile'}
& (Join-Path $root 'apps\Pacecar\Install-PacecarSwitch.ps1')
& (Join-Path $root 'apps\SwitchPowerTray\Install-SwitchPowerTray.cmd')
if($LASTEXITCODE){throw 'Power tray install failed'}
$pc=Join-Path $env:LOCALAPPDATA 'SwitchWOA\Pacecar\Pacecar.Switch.exe'
$tray=Join-Path $env:LOCALAPPDATA 'SwitchWOA\SwitchPowerTray\SwitchPowerTray.exe'
$shell=New-Object -ComObject WScript.Shell
$shortcut=$shell.CreateShortcut((Join-Path ([Environment]::GetFolderPath('Desktop')) 'Switch Power.lnk'))
$shortcut.TargetPath=$tray;$shortcut.WorkingDirectory=Split-Path $tray;$shortcut.Save()
Start-Process -FilePath $pc -WorkingDirectory (Split-Path $pc)
[ordered]@{Utc=[DateTime]::UtcNow.ToString('o');User=[Environment]::UserName;Pacecar=$pc;Tray=$tray;Desktop=[Environment]::GetFolderPath('Desktop')}|ConvertTo-Json|Set-Content (Join-Path $root 'evidence\apps-installed.json') -Encoding UTF8
