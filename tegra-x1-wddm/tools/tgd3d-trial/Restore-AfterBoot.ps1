# SPDX-License-Identifier: MIT
# Keep the frozen .158 installed, close native startup, restore the desktop.
. (Join-Path $PSScriptRoot 'Common.ps1')
Require-Administrator
$dir=$PSScriptRoot
$task='Tgd3dTrialRestore'
Start-Transcript -Path (Join-Path $dir 'restore-transcript.log') -Append
try {
    # Restore immediately. Dump capture is disabled at the owner's request.
    foreach($name in 'Tgd3dDefaultEnable','Tgd3dTrialAutomatic','Tgd3dTrialObserve','Tgd3dTrialNative') {
        if(Get-ScheduledTask -TaskName $name -ErrorAction SilentlyContinue) {
            Stop-ScheduledTask -TaskName $name -ErrorAction SilentlyContinue
            Unregister-ScheduledTask -TaskName $name -Confirm:$false
        }
    }
    $grant='HKLM:\SYSTEM\CurrentControlSet\Services\tegragpurender\AllowDesktopStart'
    if(Test-Path $grant) { Remove-Item $grant -Force }
    # Keep the protected installed-directory pointer so RESTORE.cmd can be
    # retried if a later display/backup check fails. The startup task above
    # is already removed; this pointer does not enable the GPU.
    $permit="$env:windir\System32\drivers\TgGpuBootTest158.bin"
    if(Test-Path $permit) {
        Copy-Item $permit (Join-Path $dir 'startup-contract.bin') -Force
        Remove-Item $permit -Force
    }
    try { & (Join-Path $dir 'Stop-DxgBootTrace.ps1') -TraceName 'Tgd3dTrialDxg' }
    catch { Write-Output ('Trace cleanup: '+$_.Exception.Message) }
    if(Get-EtwTraceSession -Name 'Tgd3dTrialUmd' -ErrorAction SilentlyContinue) {
        try { Stop-EtwTraceSession -Name 'Tgd3dTrialUmd' }
        catch { Write-Output ('UMD trace cleanup: '+$_.Exception.Message) }
    }
    foreach($name in 'umd-live-process.json','trace-flush-process.json') {
        $path=Join-Path $dir $name
        if(Test-Path $path) {
            $record=Read-Json $path
            Stop-RecordedTrialProcess $record
        }
    }
    # Candidate files need not still be intact to recover. Validate the backups.
    $backup=Check-Recovery
    $registration=Join-Path $dir 'icd-registration.json'
    if(Test-Path $registration) {
        $icd=Read-Json $registration
        $key=[Microsoft.Win32.Registry]::LocalMachine.OpenSubKey([string]$icd.Subkey,$true)
        if($key) {
            try {
                if($key.GetValue('VulkanDriverName') -eq [string]$icd.Manifest) { $key.DeleteValue('VulkanDriverName',$false) }
                if(@($key.GetValue('UserModeDriverName')) -contains [string]$icd.TemporaryUmd) {
                    $key.SetValue('UserModeDriverName',[string[]]$icd.PreviousUmd,[Microsoft.Win32.RegistryValueKind]::MultiString)
                }
            } finally { $key.Dispose() }
        }
    }
    # The owner's new baseline is .158. Do not uninstall it or bind the
    # archived .59. Without the permit/grant the next boot keeps native GPU
    # startup closed (expected Code 43), leaving BasicDisplay available.
    $m=Read-Json (Join-Path $dir 'trial-components.json')
    Check-File (Join-Path $dir 'package\tegragpurender.sys') $m.kmd.'tegragpurender.sys'
    if($m.kmd.'tegragpurender.sys'.Sha256 -ne '46D1757674FF156028CE5B729BA53671981FFAB09948FCAF125751C46BB10088') { throw 'Retained .158 hash differs' }
    if((Device-Property 'DriverVersion') -ne '0.1.0.158') {
        & pnputil.exe /add-driver (Join-Path $dir 'package\tegragpurender.inf') /install
        if($LASTEXITCODE -notin @(0,3010)) { throw 'Retaining .158 failed' }
    }
    if((Device-Property 'DriverVersion') -ne '0.1.0.158' -or
       (File-Hash "$env:windir\System32\drivers\tegragpurender.sys") -ne $m.kmd.'tegragpurender.sys'.Sha256) { throw 'Installed .158 differs' }
    $table="$env:windir\System32\acpitabl.dat"
    Check-File (Join-Path $dir 'display158.aml') $m.helpers.'display158.aml'
    if((File-Hash $table) -eq 'B829B50944A90745FB6F6F7F44F453A6B8573FE4019A95FEFD0013A983703518') {
        $pending=Join-Path $dir 'restore-acpitabl.pending'
        Copy-Item (Join-Path $dir 'display158.aml') $pending -Force
        [IO.File]::Replace($pending,$table,(Join-Path $dir 'before-retained158-acpitabl.dat'))
    }
    if((File-Hash $table) -ne '515D8D9C1612514CA2F197D6C96774A9265785A7E519921A195F0F8D257AA500') { throw 'Retained .158 ACPI differs' }
    $action=New-ScheduledTaskAction -Execute "$env:windir\System32\WindowsPowerShell\v1.0\powershell.exe" -Argument ('-NoProfile -ExecutionPolicy Bypass -File "'+$dir+'\restore-preference.ps1"')
    $trigger=New-ScheduledTaskTrigger -AtLogOn -User $backup.User;$trigger.Delay='PT20S'
    $principal=New-ScheduledTaskPrincipal -UserId $backup.User -LogonType Interactive -RunLevel Highest
    $settings=New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit (New-TimeSpan -Minutes 1)
    Register-ScheduledTask -TaskName 'Tgd3dTrialPreference' -Action $action -Trigger $trigger -Principal $principal -Settings $settings -Force|Out-Null
    @{Utc=[DateTime]::UtcNow.ToString('o');Version=(Device-Property 'DriverVersion');Table=(File-Hash $table);RebootVerificationPending=$true}|
        ConvertTo-Json|Set-Content (Join-Path $dir 'restore-staged.json') -Encoding UTF8
    & shutdown.exe /r /t 4
    if($LASTEXITCODE -ne 0) { throw 'Recovery reboot request failed' }
    Unregister-ScheduledTask -TaskName $task -Confirm:$false -ErrorAction SilentlyContinue
} catch {
    $_|Out-String|Set-Content (Join-Path $dir 'restore-error.txt') -Encoding UTF8
    throw
} finally { Stop-Transcript }
