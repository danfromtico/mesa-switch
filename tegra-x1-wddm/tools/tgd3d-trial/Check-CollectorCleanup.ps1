# SPDX-License-Identifier: MIT
# Real owned processes; no driver installation, GPU work or reboot.
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'Common.ps1')
$ps="$env:windir\System32\WindowsPowerShell\v1.0\powershell.exe"
$child=Start-Process $ps -ArgumentList '-NoProfile -Command "Start-Sleep -Seconds 20"' -WindowStyle Hidden -PassThru
$null=$child.Handle
try {
    $record=@{Pid=$child.Id;StartTime=$child.StartTime.ToUniversalTime().ToString('o');Exe=$ps}
    $wrong=$record.Clone();$wrong.StartTime=$child.StartTime.ToUniversalTime().AddSeconds(-1).ToString('o')
    Stop-RecordedTrialProcess $wrong
    if($child.HasExited){throw 'A mismatching process identity was stopped'}
    'PASS: mismatching creation time preserved'
    Stop-RecordedTrialProcess $record
    if(!$child.WaitForExit(5000)){throw 'Owned process was not stopped'}
    'PASS: matching owned process stopped'
}finally{if(!$child.HasExited){$child.Kill()};$child.Dispose()}

# Return a real process object retained before exit to force the exact
# lookup/exit race, without relying on timing or on PID reuse.
$script:exited=Start-Process $ps -ArgumentList '-NoProfile -Command "exit 0"' -WindowStyle Hidden -PassThru
$null=$script:exited.Handle
$record=@{Pid=$script:exited.Id;StartTime=$script:exited.StartTime.ToUniversalTime().ToString('o');Exe=$ps}
if(!$script:exited.WaitForExit(10000)){$script:exited.Kill();throw 'Exit fixture did not finish'}
function Get-Process {
    [CmdletBinding()]param([int]$Id)
    $script:exited
}
try {
    Stop-RecordedTrialProcess $record
    'PASS: process exiting after lookup does not abort recovery'
}finally{Remove-Item Function:\Get-Process;$script:exited.Dispose()}
