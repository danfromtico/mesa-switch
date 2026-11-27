# SPDX-License-Identifier: MIT
param(
    [Parameter(Mandatory=$true)][string]$Path,
    [Parameter(Mandatory=$true)][int]$DwmProcessId,
    [Parameter(Mandatory=$true)][DateTime]$DwmStart,
    [Parameter(Mandatory=$true)][long]$TrialQpc,
    [Parameter(Mandatory=$true)][long]$Frequency,
    [Parameter(Mandatory=$true)][string]$TraceName,
    [Parameter(Mandatory=$true)][string]$ExpectedLuid,
    [switch]$AllowIdle,
    [long]$NowQpc=[Diagnostics.Stopwatch]::GetTimestamp()
)
$ErrorActionPreference='Stop'

# The consumer appends JSONL while this check runs. Only complete lines count;
# a partial last line is neither a successful event nor a corrupt old record.
$file=[IO.FileStream]::new($Path,[IO.FileMode]::Open,[IO.FileAccess]::Read,([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
$reader=[IO.StreamReader]::new($file)
try{$text=$reader.ReadToEnd()}finally{$reader.Dispose()}
$end=$text.LastIndexOf("`n")
if($end -lt 0){throw 'No complete UMD trace records'}
# The fixed exporter puts id first. Call-boundary diagnostics remain in the
# full JSONL, but are not device/Present identity and need no PS object graph
# on every observer poll. Other encodings still take the normal JSON path.
$rows=@($text.Substring(0,$end) -split '[\r\n]+' | Where-Object {$_ -and $_ -notmatch '^\{"id":(14|15|16|17),'} | ForEach-Object {$_|ConvertFrom-Json})
$start=@($rows|Where-Object {$_.event -eq 'TraceStart'})
$state=@($rows|Where-Object {$_.event -eq 'TraceState'}|Select-Object -Last 1)
if($start.Count -ne 1 -or $state.Count -ne 1 -or @($rows|Where-Object {$_.event -eq 'TraceEnd'}).Count){
    throw 'UMD live consumer is not collecting complete buffers'
}
$start=$start[0];$state=$state[0]
# ETW's converted FILETIME may retain the boot-time wall-clock offset after
# Windows corrects its clock. All coverage/freshness checks use raw QPC.
if($Frequency -le 0 -or $TrialQpc -le 0 -or $start.clock -ne 'qpc' -or $state.clock -ne 'qpc' -or
    $start.qpc_frequency -ne $Frequency -or $state.qpc_frequency -ne $Frequency -or
    $start.session -cne $TraceName -or $state.session -cne $TraceName -or $state.pid -ne $start.pid -or
    $start.query_status -ne 0 -or $state.query_status -ne 0 -or $start.invalid -ne 0 -or $state.invalid -ne 0 -or
    $start.qpc_stamp -gt $TrialQpc -or $state.qpc_stamp -lt $TrialQpc -or $state.qpc_stamp -gt $NowQpc -or
    (!$AllowIdle -and $NowQpc - $state.qpc_stamp -gt 10*$Frequency)){
    throw 'UMD consumer identity, coverage or current trace state is invalid'
}
foreach($field in 'events_lost','log_buffers_lost','realtime_buffers_lost'){
    if($null -eq $start.$field -or $null -eq $state.$field -or $state.$field -ne $start.$field){
        throw ('UMD trace loss after consumer attachment: '+$field)
    }
}
$creation=$DwmStart.ToUniversalTime().ToFileTimeUtc()
# TraceState follows a complete buffer. Do not accept a newer partial buffer
# whose validation/loss counters have not yet been published.
$stateIndex=$rows.Count-1
while($rows[$stateIndex].event -ne 'TraceState'){$stateIndex--}
$rows=$rows[0..$stateIndex]
$events=@($rows|Where-Object {$_.version -eq 2 -and $_.pid -eq $DwmProcessId -and $_.image -eq 'dwm.exe' -and
    $_.process_start -eq $creation -and $_.qpc_stamp -ge $TrialQpc -and $_.qpc_stamp -le $state.qpc_stamp}|Sort-Object qpc_stamp)
$created=@($events|Where-Object {$_.id -eq 4 -and $_.status -eq '00000000'}|Select-Object -Last 1)
$present=@($events|Where-Object {$_.id -eq 30 -and $_.kind -eq 2}|Select-Object -Last 1)
if(!$created.Count -or !$present.Count -or $present[0].status -ne '00000000' -or $present[0].qpc_stamp -lt $created[0].qpc_stamp){
    throw 'No successful hardware device creation and subsequent Present from current DWM'
}
# A previous device's Present cannot validate a later creation attempt or
# destruction in the same DWM process. A later successful Present is needed.
$lifecycle=@($events|Where-Object {$_.id -in @(4,5)}|Select-Object -Last 1)
if($lifecycle.Count -and $lifecycle[0].qpc_stamp -gt $present[0].qpc_stamp){
    throw 'DWM device lifecycle changed after its last successful Present'
}
$vulkan=@($events|Where-Object {$_.id -eq 3 -and $_.status -eq '00000000' -and $_.qpc_stamp -le $created[0].qpc_stamp}|Select-Object -Last 1)
if(!$vulkan.Count -or ('{0:x16}' -f [uint64]$vulkan[0].arg0) -ne $ExpectedLuid -or
    ([uint64]$vulkan[0].arg1 -shr 32) -ne 1) { throw 'No matching NVK LUID from current DWM' }
# An idle state proves the same process created/presented on this adapter;
# it does not prove continued GPU progress. Window execution checks that next.
[pscustomobject]@{Vulkan=$vulkan[0];CreateDevice=$created[0];Present=$present[0];ConsumerStart=$start;ConsumerState=$state;StateAgeSeconds=($NowQpc-$state.qpc_stamp)/$Frequency;IdleAllowed=[bool]$AllowIdle}
