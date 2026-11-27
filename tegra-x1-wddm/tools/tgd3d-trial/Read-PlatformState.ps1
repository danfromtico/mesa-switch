# SPDX-License-Identifier: MIT
# Read-only: preserve the user-approved CPU/fan and sleep/display configuration.
$ErrorActionPreference='Stop'
$fan=Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\switchfan\Parameters'
$video=(& powercfg.exe /query SCHEME_CURRENT SUB_VIDEO VIDEOIDLE|Out-String)
if($LASTEXITCODE) { throw 'Display power query failed' }
$sleep=(& powercfg.exe /query SCHEME_CURRENT SUB_SLEEP STANDBYIDLE|Out-String)
if($LASTEXITCODE) { throw 'Sleep power query failed' }
[pscustomobject]@{Utc=[DateTime]::UtcNow.ToString('o');CpuActualKhz=$fan.CpuActualKhz;CpuOverclockEnabled=$fan.CpuOverclockEnabled;FanVersion=$fan.DriverVersion;DisplayPower=$video;SleepPower=$sleep}
