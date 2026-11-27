# SPDX-License-Identifier: MIT
# Reuses the existing observer/start sequence. Stops after startup; no pixel/cube run.
$ErrorActionPreference='Stop';$p=$PSScriptRoot;$evidence=Join-Path $p 'evidence'
function Read-Live([string]$path) {
    $f=[IO.FileStream]::new($path,[IO.FileMode]::Open,[IO.FileAccess]::Read,([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
    $r=[IO.StreamReader]::new($f)
    try { $r.ReadToEnd() } finally { $r.Dispose() }
}
try {
    if(Test-Path "$p\automatic-start.json") { throw 'A native attempt is already recorded' }
    $boot=(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToUniversalTime()
    @{Utc=[DateTime]::UtcNow.ToString('o');Boot=$boot.ToString('o')}|ConvertTo-Json|Set-Content "$p\automatic-start.json" -Encoding UTF8
    $limit=[DateTime]::UtcNow.AddSeconds(45);$ready=$false
    do {
        try {
            $o=(Read-Live "$evidence\observed-interactive.json")|ConvertFrom-Json
            $ready=$o.Problem -eq 43 -and $o.Visible -and $o.PreferenceValid -and [DateTime]::Parse($o.Utc).ToUniversalTime() -gt $boot -and (Get-ScheduledTask 'Tgd3dTrialObserve').State -eq 'Ready'
        } catch { $ready=$false }
        if(!$ready) { Start-Sleep -Milliseconds 300 }
    } while(!$ready -and [DateTime]::UtcNow -lt $limit)
    if(!$ready) { throw 'Initial BasicDisplay observation unavailable' }
    & "$p\Start-Desktop.ps1"
    $d=Get-Content -Raw "$p\demand-result.json"|ConvertFrom-Json
    $limit=[DateTime]::UtcNow.AddSeconds(25)
    while([uint64]$d.Hardware -eq 4294967295 -and [DateTime]::UtcNow -lt $limit) {
        Start-Sleep -Milliseconds 200
        $params=Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\tegragpurender\Parameters'
        $d.Hardware=$params.HardwareStatus
        $d.Problem=(Get-PnpDeviceProperty -InstanceId 'ACPI\SWOA0F01\0' -KeyName DEVPKEY_Device_ProblemCode).Data
    }
    $d|ConvertTo-Json|Set-Content "$p\startup-completion.json" -Encoding UTF8
    if($d.Problem -ne 0 -or $d.Permit -ne 0 -or $d.Hardware -ne 0) { throw 'Native startup failed' }
    & (Join-Path $p 'Start-NativeStage.ps1') -Stage Static
    'Static stage complete; continue window/interaction on this .158 boot. Recovery is manual.'|
        Set-Content "$p\waiting-for-static-feedback.txt" -Encoding UTF8
} catch {
    $_|Out-String|Set-Content "$p\automatic-error.txt" -Encoding UTF8
    throw
}
