# SPDX-License-Identifier: MIT
# Reuses the .158/.161 closed-gate -> reboot -> one native start workflow.
. (Join-Path $PSScriptRoot 'Common.ps1')
Require-Administrator
$m=Check-Package
$backup=Check-Recovery
if((Device-Property 'DriverVersion') -eq '0.1.0.158' -and (Device-Property 'ProblemCode') -eq 0) {
    throw 'Native .158 is already active; continue with Start-NativeStage.ps1 instead of reinstalling or rebooting'
}
Check-Baseline
$reuseBoot=(Device-Property 'DriverVersion') -eq '0.1.0.158'
if(Get-ScheduledTask -TaskName 'TgGpuDisplay*','Tgd3dTrial*' -ErrorAction SilentlyContinue) { throw 'Another GPU trial or restore task is present' }
if(Test-Path (Join-Path $PSScriptRoot 'before-restart.json')) { throw 'Use a newly prepared trial directory' }
$permit="$env:windir\System32\drivers\TgGpuBootTest158.bin"
if(Test-Path $permit) { throw 'A startup permit already exists' }
$ps="$env:windir\System32\WindowsPowerShell\v1.0\powershell.exe"
# Recovery remains a manual tool. No timer or failure handler may reboot a
# usable .158 desktop merely because a test stage ended.
try {
    if((Device-Property 'DriverVersion') -ne '0.1.0.158') {
        & pnputil.exe /add-driver (Join-Path $PSScriptRoot 'package\tegragpurender.inf') /install|
            Out-File (Join-Path $PSScriptRoot 'bind-closed.log')
        if($LASTEXITCODE -notin @(0,3010)) { throw 'Candidate driver binding failed' }
    }
    if((Device-Property 'DriverVersion') -ne '0.1.0.158' -or (Device-Property 'Service') -ne 'tegragpurender') { throw 'Unexpected candidate binding' }
    Check-File "$env:windir\System32\drivers\tegragpurender.sys" $m.kmd.'tegragpurender.sys'
    $parameters='HKLM:\SYSTEM\CurrentControlSet\Services\tegragpurender\Parameters'
    $limit=[DateTime]::UtcNow.AddSeconds(15)
    do {
        $p=Get-ItemProperty $parameters -ErrorAction SilentlyContinue
        if([uint64]$p.StartGateStatus -eq 3221225635 -and $p.InitializeStatus -eq 0) { break }
        Start-Sleep -Milliseconds 250
    } while([DateTime]::UtcNow -lt $limit)
    if([uint64]$p.StartGateStatus -ne 3221225635 -or $p.InitializeStatus -ne 0 -or $p.TraceStatus -ne 0) { throw 'The candidate did not remain behind its startup gate' }
    $driver=[string](Device-Property 'Driver')
    $subkey='SYSTEM\CurrentControlSet\Control\Class\'+$driver
    $key=[Microsoft.Win32.Registry]::LocalMachine.OpenSubKey($subkey,$true)
    try {
        if($key.GetValueNames() -contains 'VulkanDriverName') { throw 'Existing ICD registration must be preserved' }
        [string[]]$old=@($key.GetValue('UserModeDriverName'))
        if($old.Count -ne 4 -or @($old|Where-Object {$_ -notlike '*\tegragpuno3d.dll'}).Count) { throw 'Unexpected original UMD slots' }
        foreach($stub in $old) { Check-File $stub $m.kmd.'tegragpuno3d.dll' }
        $umd=Join-Path $PSScriptRoot 'app\TegraGpuD3DVk.dll'
        $icd=Join-Path $PSScriptRoot 'app\nvk_windows_icd.json'
        @{Subkey=$subkey;Manifest=$icd;Previous='absent';PreviousUmd=$old;TemporaryUmd=$umd;Utc=[DateTime]::UtcNow.ToString('o')}|
            ConvertTo-Json|Set-Content (Join-Path $PSScriptRoot 'icd-registration.json') -Encoding UTF8
        [string[]]$names=$old.Clone();$names[1]=$names[2]=$umd
        $key.SetValue('UserModeDriverName',$names,[Microsoft.Win32.RegistryValueKind]::MultiString)
        $key.SetValue('VulkanDriverName',$icd,[Microsoft.Win32.RegistryValueKind]::String)
    } finally { if($key) { $key.Dispose() } }
    Copy-Item (Join-Path $PSScriptRoot 'package\tegragpuno3d.dll') (Join-Path $PSScriptRoot 'tegragpuno3d.dll')
    $user=[string]$backup.User
    $action=New-ScheduledTaskAction -Execute $ps -Argument ('-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "'+$PSScriptRoot+'\Observe-Desktop.ps1"')
    $trigger=New-ScheduledTaskTrigger -AtLogOn -User $user;$trigger.Delay='PT20S'
    $principal=New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive -RunLevel Limited
    $settings=New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit (New-TimeSpan -Seconds 40)
    Register-ScheduledTask -TaskName 'Tgd3dTrialObserve' -Action $action -Trigger $trigger -Principal $principal -Settings $settings|Out-Null
    $action=New-ScheduledTaskAction -Execute $ps -Argument ('-NoProfile -ExecutionPolicy Bypass -File "'+$PSScriptRoot+'\Automatic-Start.ps1"')
    $trigger=New-ScheduledTaskTrigger -AtLogOn -User $user;$trigger.Delay='PT60S'
    $settings=New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit (New-TimeSpan -Minutes 3)
    Register-ScheduledTask -TaskName 'Tgd3dTrialAutomatic' -Action $action -Trigger $trigger -Settings $settings -User SYSTEM -RunLevel Highest|Out-Null
    $data=[byte[]]::new(4096)
    for($i=4;$i -lt 52;$i++) { $data[$i]=255 }
    $file=[IO.FileStream]::new($permit,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::Read,4096,[IO.FileOptions]::WriteThrough)
    try { $file.Write($data,0,$data.Length);$file.Flush($true) } finally { $file.Dispose() }
    $table="$env:windir\System32\acpitabl.dat"
    if($reuseBoot) {
        # Check-Baseline already verified this boot's .158 SYS and ACPI.
        # The frozen KMD consumes its permit in StartDevice, not DriverEntry.
        Copy-Item $table (Join-Path $PSScriptRoot 'before-acpitabl.dat')
    } else {
        $pending=Join-Path $PSScriptRoot 'test-acpitabl.pending'
        Copy-Item (Join-Path $PSScriptRoot 'display158.aml') $pending
        [IO.File]::Replace($pending,$table,(Join-Path $PSScriptRoot 'before-acpitabl.dat'))
    }
    if((File-Hash $table) -ne '515D8D9C1612514CA2F197D6C96774A9265785A7E519921A195F0F8D257AA500') { throw 'ACPI staging differs' }
    Check-File (Join-Path $PSScriptRoot 'before-acpitabl.dat') $backup.Files.'rollback\acpitabl.dat'
    @{Utc=[DateTime]::UtcNow.ToString('o');Boot=(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToUniversalTime().ToString('o');RebootRequired=(!$reuseBoot)}|
        ConvertTo-Json|Set-Content (Join-Path $PSScriptRoot 'before-restart.json') -Encoding UTF8
    if($reuseBoot) {
        Start-ScheduledTask 'Tgd3dTrialObserve'
        Start-ScheduledTask 'Tgd3dTrialAutomatic'
    } else {
        & shutdown.exe /r /t 5
        if($LASTEXITCODE -ne 0) { throw 'Reboot request failed' }
    }
} catch {
    $_|Out-String|Set-Content (Join-Path $PSScriptRoot 'start-failed.txt') -Encoding UTF8
    throw
}
