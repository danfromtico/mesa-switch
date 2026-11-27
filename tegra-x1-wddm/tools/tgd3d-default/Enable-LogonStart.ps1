# SPDX-License-Identifier: MIT
# Reuse the existing SYSTEM controller. An offline-installed RunOnce entry invokes
# this once at the next interactive logon, using the machine's existing admin
# token configuration. No UAC policy or private TaskCache records are changed.
param([switch]$StartThisSession)
. (Join-Path $PSScriptRoot 'Common.ps1')
$watch=[Diagnostics.Stopwatch]::StartNew()
$state=[ordered]@{Utc=[DateTime]::UtcNow.ToString('o');Status='Configuring';DelaySeconds=5;GpuStartRequested=$false}
try {
    Require-Administrator
    $m=Check-Package
    if($m.startup_mode -ne 'logon-default'){throw 'Expected a logon-default package'}
    if(Test-Path 'HKLM:\SYSTEM\CurrentControlSet\Services\tegragpurender\AllowNativeBoot'){
        throw 'Persistent native boot must be removed before configuring delayed activation'
    }
    Check-Baseline
    $backup=Check-Recovery
    $action=New-ScheduledTaskAction -Execute "$env:windir\System32\WindowsPowerShell\v1.0\powershell.exe" -Argument ('-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "'+$PSScriptRoot+'\Default-Start.ps1"')
    $trigger=New-ScheduledTaskTrigger -AtLogOn -User $backup.User
    $trigger.Delay='PT5S'
    $settings=New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -MultipleInstances IgnoreNew -ExecutionTimeLimit (New-TimeSpan -Minutes 3)
    Register-ScheduledTask -TaskName Tgd3dDefaultEnable -Action $action -Trigger $trigger -Settings $settings -User SYSTEM -RunLevel Highest -Force|Out-Null
    $task=Get-ScheduledTask Tgd3dDefaultEnable
    if(!$task.Settings.Enabled -or $task.Triggers[0].Delay -ne 'PT5S' -or
       $task.Actions[0].Arguments -ne $action.Arguments){throw 'Registered startup task differs'}
    $state.Status='Registered';$state.User=$backup.User
    Save-Json (Join-Path $PSScriptRoot 'logon-setup-state.json') $state
    Save-Json (Join-Path $PSScriptRoot 'default-installed.json') @{
        Utc=[DateTime]::UtcNow.ToString('o');Directory=$PSScriptRoot;Version=$m.kmd_version
        SyncMode=$m.sync_mode;User=$backup.User;StartupMode='logon-default'
        EnabledAfterNextLogon=$true;DelaySeconds=5;OsRebootRequested=$false
    }
    # Windows removes the !-prefixed RunOnce entry after this setup exits.
    # Do not edit the startup key while Windows is enumerating its entries.
    if($StartThisSession){
        # A task registered after logon does not retroactively receive that
        # trigger. Start it once here, after at least five seconds in this
        # logon bootstrap. Later boots use the scheduler's PT5S trigger.
        $remaining=5000-$watch.ElapsedMilliseconds
        if($remaining -gt 0){Start-Sleep -Milliseconds ([int]$remaining)}
        Start-ScheduledTask Tgd3dDefaultEnable
        $state.GpuStartRequested=$true
    }
    $state.End=[DateTime]::UtcNow.ToString('o')
    Save-Json (Join-Path $PSScriptRoot 'logon-setup-state.json') $state
} catch {
    $state.Status='Failed';$state.Error=$_.Exception.Message
    Save-Json (Join-Path $PSScriptRoot 'logon-setup-state.json') $state
    throw
}
