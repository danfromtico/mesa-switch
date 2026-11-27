# SPDX-License-Identifier: MIT
# Rebase saved display preferences on the retained, gated .158 driver.
. (Join-Path $PSScriptRoot 'Common.ps1')
Require-Administrator
Check-Baseline
$null=Check-Recovery
$m=Read-Json (Join-Path $PSScriptRoot 'trial-components.json')
$helper=Join-Path $PSScriptRoot 'display-state.exe'
Check-File $helper $m.helpers.'display-state.exe'
if(!(Get-Process -Id $PID).SessionId) { throw 'Display preferences require an interactive session' }
$evidence=Join-Path $PSScriptRoot 'evidence'
$r=[ordered]@{Utc=[DateTime]::UtcNow.ToString('o');Verified=$false;Version=(Device-Property 'DriverVersion');Problem=(Device-Property 'ProblemCode');NativeGpuActive=$false}
try {
    & $helper wake > "$evidence\restore-wake.json" 2> "$evidence\restore-wake.err"
    $r.WakeExit=$LASTEXITCODE
    & $helper restore (Join-Path $PSScriptRoot 'user-display.bin') > "$evidence\restore-preference.json" 2> "$evidence\restore-preference.err"
    $r.RestoreExit=$LASTEXITCODE
    & $helper verify (Join-Path $PSScriptRoot 'user-display.bin') > "$evidence\restore-verify.json" 2> "$evidence\restore-verify.err"
    $r.VerifyExit=$LASTEXITCODE
    if($r.RestoreExit -ne 0 -or $r.VerifyExit -ne 0) { throw 'Display preference restoration failed' }
    $before=Read-Json (Join-Path $PSScriptRoot 'before-platform.json')
    $after=& (Join-Path $PSScriptRoot 'Read-PlatformState.ps1')
    $after|ConvertTo-Json|Set-Content (Join-Path $PSScriptRoot 'after-platform.json') -Encoding UTF8
    foreach($name in 'CpuActualKhz','CpuOverclockEnabled','FanVersion','DisplayPower','SleepPower') {
        if($after.$name -ne $before.$name) { throw ('Platform setting differs: '+$name) }
    }
    $r.PlatformUnchanged=$true
    $r.Verified=$true
    Unregister-ScheduledTask -TaskName 'Tgd3dTrialPreference' -Confirm:$false -ErrorAction SilentlyContinue
} catch { $r.Error=$_.Exception.Message;throw }
finally { $r|ConvertTo-Json|Set-Content (Join-Path $PSScriptRoot 'recovery-verified.json') -Encoding UTF8 }
