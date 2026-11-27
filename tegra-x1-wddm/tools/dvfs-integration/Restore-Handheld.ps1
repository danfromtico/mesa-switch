# SPDX-License-Identifier: MIT
# Select the normal Switch Lite Handheld policy through the SwitchFan v2 ABI.
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$powerTool = Join-Path $PSScriptRoot 'Power-State.ps1'
$before = & $powerTool
$initial = $before.Power
if ($initial.IsLite -ne 1 -or $initial.CpuConfigured -ne 1 -or ($initial.Flags -band 0x10) -eq 0) {
    throw 'Handheld requires a configured Lite CPU and a live GPU counter'
}
function Test-Handheld($state) {
    $state.PowerMode -eq 1 -and $state.CpuDfllActive -eq 1 -and
        $state.CpuLastError -eq 0 -and $state.GpuPolicyMode -eq 1
}
$fixed = $null
$after = $before
if (!(Test-Handheld $initial)) {
    # SwitchFan may have entered mode 7 itself after a DFLL error. Explicitly
    # selecting mode 7 releases the failure latch before mode 1 can start.
    if ($initial.PowerMode -eq 7 -and $initial.CpuLastError -ne 0) {
        $fixed = & $powerTool -Mode 7
        if ($fixed.Power.PowerMode -ne 7 -or $fixed.Power.CpuLastError -ne 0) {
            throw 'SwitchFan did not clear the fixed-mode failure latch'
        }
    }
    $after = & $powerTool -Mode 1
    if (!(Test-Handheld $after.Power)) {
        throw 'SwitchFan did not activate Handheld CPU and GPU policies'
    }
}
if ($after.Battery.LimitCenter -ne $before.Battery.LimitCenter) {
    throw 'Battery charge limit changed during power policy recovery'
}
[pscustomobject]@{ Before=$before; Fixed=$fixed; After=$after }
