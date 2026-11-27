# SPDX-License-Identifier: MIT
# CPU-only controls for current-process tgd3d evidence; emits no ETW or GPU work.
$ErrorActionPreference='Stop'
$dir=Join-Path ([IO.Path]::GetTempPath()) ('tgd3d-reader-'+[Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory $dir|Out-Null
$path=Join-Path $dir 'events.jsonl'
$dwmStart=[DateTime]::UtcNow.AddMinutes(-1);$creation=$dwmStart.ToFileTimeUtc()
$base=4000000;$frequency=1000000;$nowQpc=7000000
$readerArgs=@{Path=$path;DwmProcessId=77;DwmStart=$dwmStart;TrialQpc=$base;Frequency=$frequency;TraceName='test';ExpectedLuid='00000000000bc22f';NowQpc=$nowQpc}
function Rows {
    @(
        @{event='TraceStart';time=134390199000000000;qpc_stamp=$base-$frequency;qpc_frequency=$frequency;clock='qpc';pid=22;session='test';query_status=0;invalid=0;events_lost=0;log_buffers_lost=0;realtime_buffers_lost=0},
        @{id=3;version=2;process_start=$creation;qpc_stamp=$base+1;pid=77;image='dwm.exe';status='00000000';arg0=770607;arg1=[uint64]4299161600},
        @{id=4;version=2;process_start=$creation;qpc_stamp=$base+2;pid=77;image='dwm.exe';status='00000000'},
        @{id=30;version=2;process_start=$creation;kind=2;qpc_stamp=$base+3;pid=77;image='dwm.exe';status='00000000'},
        @{event='TraceState';time=134390199030000000;qpc_stamp=$base+2*$frequency;qpc_frequency=$frequency;clock='qpc';pid=22;session='test';query_status=0;invalid=0;events_lost=0;log_buffers_lost=0;realtime_buffers_lost=0}
    )
}
function Check([string]$Name,$Rows,[bool]$Expected,[string]$Tail='') {
    $text=(@($Rows|ForEach-Object {$_|ConvertTo-Json -Compress}) -join "`n")+"`n"+$Tail
    [IO.File]::WriteAllText($path,$text)
    $ok=$false
    try { $null=& (Join-Path $PSScriptRoot 'Read-NativeUmdEvidence.ps1') @readerArgs;$ok=$true } catch { $message=$_.Exception.Message }
    if($ok -ne $Expected) { throw ('Reader control failed: '+$Name+' '+$message) }
    'PASS '+$Name
}
try {
    Check 'current tgd3d creation and Present' (Rows) $true
    Check 'partial trailing record ignored' (Rows) $true '{"id":30'
    $r=Rows;$r[1].arg0=1;Check 'wrong NVK LUID rejected' $r $false
    $r=Rows;$r[1].arg1=4194304;Check 'non-NVK rejected' $r $false
    $r=Rows;$r[2].status='80004005';Check 'failed creation rejected' $r $false
    $r=Rows;$r[3].status='887a0005';Check 'failed Present rejected' $r $false
    $r=Rows;$r[3].kind=1;Check 'Submit is not Present' $r $false
    $r=Rows;$r[3].pid=78;Check 'other process rejected' $r $false
    $r=Rows;$r[4].events_lost=1;Check 'lost events rejected' $r $false
    $r=Rows;$r[2].id=11;$r[3].id=13;Check 'Zink IDs rejected' $r $false
    $r=Rows;$r=$r[0..3]+@(@{id=5;version=2;process_start=$creation;qpc_stamp=$base+4;pid=77;image='dwm.exe';status='00000000'})+@($r[4]);Check 'destroy after Present rejected' $r $false
    $r=Rows;$r[4].qpc_stamp=$nowQpc-20*$frequency;Check 'stale state rejected' $r $false
    $r=Rows;$r[4].time=$r[0].time-150000000;Check 'wall clock jumps backwards' $r $true
    $r=Rows;$r[4].time=$r[0].time+150000000;Check 'wall clock jumps forwards' $r $true
    $r=Rows;$r[3].process_start=$creation+1;Check 'reused PID with different creation time rejected' $r $false
    $r=Rows;$r[2].version=1;Check 'legacy events cannot certify current native process' $r $false
    $r=Rows;$r[4].clock='system';Check 'wrong trace clock rejected' $r $false
    $r=Rows;$r[4].qpc_frequency=$frequency+1;Check 'different QPC frequency rejected' $r $false
    $readerArgs.NowQpc=$nowQpc+20*$frequency
    Check 'quiet completed buffer fails ordinary freshness check' (Rows) $false
    $readerArgs.AllowIdle=$true
    Check 'either submission mode accepts idle current-process identity' (Rows) $true
    $r=Rows;$r[3].status='887a0005';Check 'idle comparison still rejects failed Present' $r $false
    $r=Rows;$r[4].events_lost=1;Check 'idle comparison still rejects event loss' $r $false
} finally { Remove-Item $dir -Recurse -Force }
