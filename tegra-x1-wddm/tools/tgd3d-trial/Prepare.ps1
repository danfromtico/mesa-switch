# SPDX-License-Identifier: MIT
# Save the starting driver/display configuration; future recovery retains .158.
param([switch]$Installed)
. (Join-Path $PSScriptRoot 'Common.ps1')
Require-Administrator
$m=Check-Package
Check-Baseline
if(Get-ScheduledTask -TaskName 'TgGpuDisplay*','Tgd3dTrial*' -ErrorAction SilentlyContinue) { throw 'Another GPU trial or restore task is present' }
if(Get-ChildItem "$env:windir\System32\drivers\TgGpuBootTest*.bin" -ErrorAction SilentlyContinue) { throw 'A GPU startup permit already exists' }
if(Test-Path 'HKLM:\SYSTEM\CurrentControlSet\Services\tegragpurender\AllowDesktopStart') { throw 'A GPU startup grant already exists' }
if(Test-Path 'HKLM:\SYSTEM\CurrentControlSet\Services\tegragpurender\AllowNativeBoot') { throw 'Persistent native startup already exists' }
if(!$Installed) {
    $name='tgd3d-'+([string]$m.umd_source).Substring(0,8)+'-'+(Get-Date -Format 'yyyyMMdd-HHmmss')
    $dest=Join-Path $env:ProgramData ('SwitchWOA\GpuUpdate\'+$name)
    if(Test-Path $dest) { throw 'Trial destination already exists' }
    New-Item -ItemType Directory -Path $dest|Out-Null
    foreach($entry in $m.files.PSObject.Properties) {
        $target=Join-Path $dest $entry.Name
        New-Item -ItemType Directory -Path (Split-Path $target) -Force|Out-Null
        Copy-Item -LiteralPath (Package-Path $entry.Name) -Destination $target
    }
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'trial-components.json') -Destination $dest
    # SYSTEM rollback must never run scripts writable by an unelevated user.
    & icacls.exe $dest /inheritance:r /grant:r '*S-1-5-18:(OI)(CI)(F)' '*S-1-5-32-544:(OI)(CI)(F)' '*S-1-5-32-545:(OI)(CI)(RX)' /Q|Out-Null
    if($LASTEXITCODE -ne 0) { throw 'Trial ACL failed' }
    & icacls.exe $dest /setowner '*S-1-5-32-544' /T /Q|Out-Null
    if($LASTEXITCODE -ne 0) { throw 'Trial ownership failed' }
    & (Join-Path $dest 'Prepare.ps1') -Installed
    Write-Output ('PREPARED_DIRECTORY='+$dest)
    return
}
$os=Get-CimInstance Win32_OperatingSystem
$version=Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion'
@{Caption=$os.Caption;Version=$os.Version;Build=$os.BuildNumber;UBR=$version.UBR;Boot=$os.LastBootUpTime.ToUniversalTime().ToString('o')}|ConvertTo-Json|Set-Content (Join-Path $PSScriptRoot 'windows-version.json') -Encoding UTF8
$platform=& (Join-Path $PSScriptRoot 'Read-PlatformState.ps1')
if($m.kmd_variant -eq 'reviewed-dvfs-emc') {
    # The enclosing EMC installer verifies fresh CPU/BPMP status. CPU clocks
    # can vary in Handheld; this package must bind the matched platform owner.
    if($platform.FanVersion -ne $m.platform_fan_version -or $null -eq $platform.CpuActualKhz -or [int64]$platform.CpuActualKhz -le 0 -or
       @(Get-PnpDevice -PresentOnly|Where-Object{$_.InstanceId -like 'ACPI\SWOA0402\*' -and $_.Status -eq 'OK'}).Count -ne 1 -or
       @(Get-PnpDevice -PresentOnly|Where-Object{$_.InstanceId -like 'ACPI\SWOA0601\*' -and $_.Status -eq 'OK'}).Count -ne 1){throw 'Matched EMC platform is unavailable'}
} elseif($platform.CpuActualKhz -ne 1497600 -or $platform.CpuOverclockEnabled -ne 1 -or $platform.FanVersion -ne 13) { throw 'CPU/fan baseline differs' }
$platform|ConvertTo-Json|Set-Content (Join-Path $PSScriptRoot 'before-platform.json') -Encoding UTF8
if(Test-Path (Join-Path $PSScriptRoot 'recovery.json')) { throw 'This trial is already prepared' }
$backupDir=Join-Path $PSScriptRoot 'rollback'
New-Item -ItemType Directory -Path $backupDir|Out-Null
$oem=[string](Device-Property 'DriverInfPath')
$baselineVersion=[string](Device-Property 'DriverVersion')
$baselineName=if($baselineVersion -eq '0.1.0.158'){'tegragpurender'}else{'tegragpuprobe'}
if($oem -notmatch '^oem[0-9]+\.inf$') { throw 'Expected an exported OEM GPU package' }
& pnputil.exe /export-driver $oem $backupDir|Out-File (Join-Path $PSScriptRoot 'backup-driver.log')
if($LASTEXITCODE -ne 0) { throw 'Cannot export the current rollback driver' }
$infs=@(Get-ChildItem $backupDir -Recurse -Filter ($baselineName+'.inf'))
if($infs.Count -ne 1) { throw 'The starting driver INF was not exported exactly once' }
$probe=Join-Path $infs[0].DirectoryName ($baselineName+'.sys')
$expected=if($baselineVersion -eq '0.1.0.158'){File-Hash "$env:windir\System32\drivers\tegragpurender.sys"}else{'DE5E9CD492D35A73EA3D3F5F1723FD82F88922F6E91EF5966B430360F5EB5646'}
if((File-Hash $probe) -ne $expected) { throw 'Exported starting driver differs' }
Copy-Item "$env:windir\System32\acpitabl.dat" (Join-Path $backupDir 'acpitabl.dat')
$user=(Get-CimInstance Win32_ComputerSystem).UserName
if(!$user) { throw 'An interactive desktop is required to save its display preference' }
$evidence=Join-Path $PSScriptRoot 'evidence'
New-Item -ItemType Directory -Path $evidence|Out-Null
& icacls.exe $evidence /grant ('{0}:(OI)(CI)(M)' -f $user)|Out-Null
if($LASTEXITCODE -ne 0) { throw 'Evidence directory access failed' }
$snapshot=Join-Path $evidence 'user-display.bin'
$exitFile=Join-Path $evidence 'snapshot.exit'
# Publish a closed receipt after the child exits. File creation alone is not
# completion: redirection can expose an empty file before echo writes its code.
$cmd="@echo off`r`nsetlocal`r`nset `"ERRORLEVEL=`"`r`n`"$PSScriptRoot\display-state.exe`" save `"$snapshot`" > `"$evidence\snapshot.json`" 2> `"$evidence\snapshot.err`"`r`nset `"snapshotResult=%errorlevel%`"`r`n> `"$exitFile.tmp`" echo %snapshotResult%`r`nmove /y `"$exitFile.tmp`" `"$exitFile`" >nul`r`nexit /b %snapshotResult%`r`n"
[IO.File]::WriteAllText((Join-Path $PSScriptRoot 'snapshot.cmd'),$cmd,[Text.Encoding]::ASCII)
$action=New-ScheduledTaskAction -Execute "$env:windir\System32\cmd.exe" -Argument ('/d /c "'+$PSScriptRoot+'\snapshot.cmd"')
$principal=New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive -RunLevel Highest
$settings=New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit (New-TimeSpan -Seconds 100)
Register-ScheduledTask -TaskName 'Tgd3dTrialSnapshot' -Action $action -Principal $principal -Settings $settings|Out-Null
try {
    Start-ScheduledTask -TaskName 'Tgd3dTrialSnapshot'
    $limit=[DateTime]::UtcNow.AddSeconds(90)
    $complete=$false;$snapshotExit=0
    do {
        if(Test-Path $exitFile) {
            $receipt=[IO.File]::ReadAllText($exitFile).Trim()
            if($receipt -and [int]::TryParse($receipt,[ref]$snapshotExit)) {$complete=$true;break}
            if($receipt) {throw 'Malformed display preference completion receipt'}
        }
        Start-Sleep -Milliseconds 200
    }while([DateTime]::UtcNow -lt $limit)
    if(!$complete) {
        $task=Get-ScheduledTask 'Tgd3dTrialSnapshot'
        $info=Get-ScheduledTaskInfo 'Tgd3dTrialSnapshot'
        throw ('Display preference snapshot timed out: task='+$task.State+' result='+$info.LastTaskResult+'; inspect evidence/snapshot.*')
    }
    if($snapshotExit -ne 0) {throw ('Display preference snapshot exited '+$snapshotExit+'; inspect evidence/snapshot.err')}
    if(!(Test-Path $snapshot) -or (Get-Item $snapshot).Length -eq 0) {throw 'Display preference snapshot file is missing or empty'}
    Copy-Item $snapshot (Join-Path $PSScriptRoot 'user-display.bin')
} finally {
    Stop-ScheduledTask -TaskName 'Tgd3dTrialSnapshot' -ErrorAction SilentlyContinue
    Unregister-ScheduledTask -TaskName 'Tgd3dTrialSnapshot' -Confirm:$false
}
$files=@{}
foreach($file in Get-ChildItem $backupDir -Recurse -File) {
    $relative=$file.FullName.Substring($PSScriptRoot.Length+1)
    $files[$relative]=@{Length=$file.Length;Sha256=(File-Hash $file.FullName)}
}
$display=Get-Item (Join-Path $PSScriptRoot 'user-display.bin')
$files['user-display.bin']=@{Length=$display.Length;Sha256=(File-Hash $display.FullName)}
Save-Json (Join-Path $PSScriptRoot 'recovery.json') @{InstanceId=$DeviceId;Version=$baselineVersion;RecoveryTarget='0.1.0.158-gated';OriginalOem=$oem;User=$user;Utc=[DateTime]::UtcNow.ToString('o');Inf=$infs[0].FullName.Substring($PSScriptRoot.Length+1);Files=$files}
$null=Sync-Recovery
& (Join-Path $PSScriptRoot 'Enable-RuntimeAppContainerRead.ps1') -Directory (Join-Path $PSScriptRoot 'app')|
    Set-Content (Join-Path $PSScriptRoot 'runtime-access.json') -Encoding UTF8
'PREPARED: starting driver, ACPI and display preference saved; recovery retains gated .158; no GPU test started'
