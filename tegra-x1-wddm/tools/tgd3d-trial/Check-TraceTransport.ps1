# SPDX-License-Identifier: MIT
# Real Windows ETW transport, synthetic payloads; no D3D/Vulkan/GPU device.
param([string]$LegacyEtl)
. (Join-Path $PSScriptRoot 'Common.ps1')
Require-Administrator
Check-Baseline
$p=$PSScriptRoot;$name='Tgd3dTraceControl'+$PID
$m=Read-Json "$p\trial-components.json"
foreach($binary in 'tgd3d-trace-export.exe','tgd3d-trace-control.exe') { Check-File "$p\app\$binary" $m.app.$binary }
$started=$false;$consumerProcess=$null
try {
    $consumer=& "$p\Start-NativeUmdTrace.ps1" -Directory $p -TraceName $name -CreateSession
    $started=$true
    $consumerProcess=Get-Process -Id $consumer.Pid
    $fixture=Start-Process "$p\app\tgd3d-trace-control.exe" -Wait -PassThru -NoNewWindow -RedirectStandardOutput "$p\fixture.json" -RedirectStandardError "$p\fixture.err"
    if($fixture.ExitCode) { throw 'Synthetic ETW emitter failed' }
} finally {
    if($started) { Stop-EtwTraceSession -Name $name }
    if($consumerProcess -and !$consumerProcess.WaitForExit(10000)) { $consumerProcess.Kill();throw 'Live consumer did not stop' }
}
$fixtureResult=Read-Json "$p\fixture.json"
$live=@(Get-Content "$p\umd-live.jsonl"|ForEach-Object {$_|ConvertFrom-Json})
$events=@($live|Where-Object {$_.id})
$states=@($live|Where-Object {$_.event -eq 'TraceState'})
if($events.Count -ne 16 -or !$states.Count) { throw 'Incomplete real-time records/buffers' }
foreach($id in 14,15,16,17) {
    $pair=@($events|Where-Object {$_.id -eq $id})
    if($pair.Count -ne 2 -or $pair[0].status -ne '8000000a' -or $pair[1].status -ne '00000000') { throw 'Call entry/return pair differs' }
}
$waitBegin=@($events|Where-Object {$_.id -eq 30 -and $_.kind -eq 4})
$waitEnd=@($events|Where-Object {$_.id -eq 30 -and $_.kind -eq 3})
if($waitBegin.Count -ne 1 -or $waitEnd.Count -ne 1 -or
    $waitBegin[0].status -ne '8000000a' -or $waitEnd[0].status -ne '80004005' -or
    $waitBegin[0].value[0] -ne 3 -or $waitEnd[0].value[0] -ne 3 -or
    $waitEnd[0].value[2] -ne 2 -or $waitEnd[0].completed -ne 4) { throw 'Incomplete fence was misclassified' }
foreach($e in $events) {
    if($e.version -ne 2 -or $e.pid -ne $fixtureResult.pid -or $e.process_start -ne $fixtureResult.process_start -or
       $e.qpc_stamp -lt $fixtureResult.begin_qpc -or $e.qpc_stamp -gt $fixtureResult.end_qpc) { throw 'Raw timestamp or process identity differs' }
}
foreach($s in $states) {
    if($s.clock -ne 'qpc' -or $s.qpc_frequency -ne [Diagnostics.Stopwatch]::Frequency -or $s.query_status -ne 0 -or
       $s.invalid -ne 0 -or $s.events_lost -ne 0 -or $s.realtime_buffers_lost -ne 0 -or $s.log_buffers_lost -ne 0) { throw 'Live trace state differs' }
}
$offline=Start-Process "$p\app\tgd3d-trace-export.exe" -ArgumentList ('"'+$p+'\umd.etl"') -Wait -PassThru -NoNewWindow -RedirectStandardOutput "$p\offline.jsonl" -RedirectStandardError "$p\offline.log"
$offlineRows=@(Get-Content "$p\offline.jsonl"|ForEach-Object {$_|ConvertFrom-Json})
if($offline.ExitCode -ne 0 -or $offlineRows.Count -ne 16) { throw 'Offline v2 export failed' }
for($i=0;$i -lt 16;$i++) {
    foreach($field in 'id','version','pid','process_start','image','status','kind','sequence','completed','arg0','arg1','message') {
        if($events[$i].$field -ne $offlineRows[$i].$field) { throw ('Live/offline payload mismatch: '+$field) }
    }
    foreach($field in 'qpc','value') {
        if(($events[$i].$field -join ',') -ne ($offlineRows[$i].$field -join ',')) { throw ('Live/offline timing payload mismatch: '+$field) }
    }
}
$legacyCount=$null
if($LegacyEtl) {
    $legacy=Start-Process "$p\app\tgd3d-trace-export.exe" -ArgumentList ('"'+$LegacyEtl+'"') -Wait -PassThru -NoNewWindow -RedirectStandardOutput "$p\legacy-v1.jsonl" -RedirectStandardError "$p\legacy-v1.log"
    if($legacy.ExitCode) { throw 'Legacy export failed' }
    $old=@(Get-Content "$p\legacy-v1.jsonl"|ForEach-Object {$_|ConvertFrom-Json})
    if(!$old.Count -or @($old|Where-Object {$_.version -ne 1 -or $_.process_start -ne 0}).Count) { throw 'Legacy identity was fabricated' }
    $legacyCount=$old.Count
}
Check-Baseline
@{Utc=[DateTime]::UtcNow.ToString('o');LiveEvents=$events.Count;LiveStates=$states.Count;OfflineEvents=$offlineRows.Count;LegacyEvents=$legacyCount;Scope='Synthetic CPU-only ETW, no GPU or native display acceptance';Baseline='0.1.0.59 / Code0'}|
    ConvertTo-Json|Set-Content "$p\transport-result.json" -Encoding UTF8
Get-Content "$p\transport-result.json"
