# SPDX-License-Identifier: MIT
# Stages are requested separately, in the same native boot. Physical feedback stays separate.
param([ValidateSet('Static','Window','Interaction')][string]$Stage='Static')
$ErrorActionPreference='Stop';$p=$PSScriptRoot;$e=Join-Path $p 'evidence'
if(!(Get-Process -Id $PID).SessionId){throw 'Run on the interactive desktop'}
if(Test-Path (Join-Path $e ('stage-'+$Stage+'.result.json'))){throw 'Stage result already exists'}
$request=Get-Content -Raw "$p\stage-$Stage.request.json"|ConvertFrom-Json
$stem=Join-Path $e ('stage-'+$Stage)
$result=[ordered]@{Stage=$Stage;Begin=[DateTime]::UtcNow.ToString('o');BeginQpc=[Diagnostics.Stopwatch]::GetTimestamp();Frequency=[Diagnostics.Stopwatch]::Frequency;ApiObservationPassed=$false;PhysicalPanelVerified=$false;Luid=$request.ExpectedLuid;Captures=@();Actions=@()}
$ps="$env:windir\System32\WindowsPowerShell\v1.0\powershell.exe"
$window=$null;$performance=$null;$watch=[Diagnostics.Stopwatch]::new()
function Action([string]$Name){
    $a=@{Name=$Name;Utc=[DateTime]::UtcNow.ToString('o');Qpc=[Diagnostics.Stopwatch]::GetTimestamp()}
    $result.Actions+=,$a
    $bytes=[Text.Encoding]::UTF8.GetBytes(($a|ConvertTo-Json -Compress)+"`n")
    $f=[IO.FileStream]::new(($stem+'.actions.jsonl'),[IO.FileMode]::Append,[IO.FileAccess]::Write,[IO.FileShare]::Read,4096,[IO.FileOptions]::WriteThrough)
    try{$f.Write($bytes,0,$bytes.Length);$f.Flush($true)}finally{$f.Dispose()}
}
function Capture([string]$Name,[string]$Shortcut='None'){
    Action ('capture-'+$Name+'-'+$Shortcut)
    $prefix=$stem+'-'+$Name
    $captureArgs='-NoProfile -ExecutionPolicy Bypass -File "'+$p+'\Capture-DesktopEvidence.ps1" -OutputPrefix "'+$prefix+'" -Shortcut '+$Shortcut
    $child=Start-Process $ps -ArgumentList $captureArgs -WindowStyle Hidden -PassThru -RedirectStandardOutput ($prefix+'.log') -RedirectStandardError ($prefix+'.err')
    $null=$child.Handle
    if(!$child.WaitForExit(15000)){$child.Kill();throw 'Desktop capture timed out'}
    if($child.ExitCode -ne 0){throw 'Desktop capture failed'}
    $result.Captures+=($prefix+'.json')
}
try {
    $points=switch($Stage){Static {@(0,15,30)} Window {@(0,15,30,45,60)} Interaction {@(0,30,60,65,90,120,125,150,180,210,240,270,300)}}
    foreach($second in $points){
        while($watch.IsRunning -and $watch.Elapsed.TotalSeconds -lt $second){Start-Sleep -Milliseconds 200}
        if($window -and $window.HasExited -and $window.ExitCode -ne 0){throw 'Owned window exercise failed'}
        if($performance -and $performance.HasExited -and $performance.ExitCode -ne 0){throw 'Performance program regression failed'}
        Action ('identity-'+$second)
        $identityPath=$stem+'-'+$second+'-identity.json'
        & "$p\Assert-NativeDesktop.ps1" -Directory $p -OutputPath $identityPath -ExpectedLuid $result.Luid
        $identity=Get-Content -Raw $identityPath|ConvertFrom-Json
        if(!$result.Luid){$result.Luid=$identity.Luid}
        if(!$watch.IsRunning){
            $watch.Start()
            if($Stage -ne 'Static'){
                Action 'open-owned-window'
                $seconds=if($Stage -eq 'Window'){45}else{285}
                $arguments='-NoProfile -ExecutionPolicy Bypass -File "'+$p+'\Exercise-DesktopWindow.ps1" -OutputPrefix "'+$stem+'-window" -DurationSeconds '+$seconds+' -CandidateVersion 0.1.0.158'
                if($Stage -eq 'Interaction'){$arguments+=' -FullInteraction'}
                $window=Start-Process $ps -ArgumentList $arguments -WindowStyle Hidden -PassThru -RedirectStandardOutput ($stem+'-window.log') -RedirectStandardError ($stem+'-window.err')
                $null=$window.Handle
            }
        }
        if($Stage -eq 'Static'){
            if($second -eq 30){Capture '30'}
        }elseif($Stage -eq 'Window'){
            if($second -in @(15,60)){Capture ([string]$second)}
        }else{
            switch($second){
                60 {Capture '60' 'Start'}
                65 {Capture '65' 'Dismiss'}
                120 {Capture '120' 'QuickSettings'}
                125 {Capture '125' 'Dismiss'}
                180 {
                    Action 'open-performance-program'
                    $arguments='-NoProfile -ExecutionPolicy Bypass -File "'+$p+'\Exercise-PerformanceWindow.ps1" -OutputPrefix "'+$stem+'-performance" -ExpectedVersion 0.1.0.158'
                    $performance=Start-Process $ps -ArgumentList $arguments -WindowStyle Hidden -PassThru -RedirectStandardOutput ($stem+'-performance.log') -RedirectStandardError ($stem+'-performance.err')
                    $null=$performance.Handle
                }
                default {if($second -in @(30,90,150,240,300)){Capture ([string]$second)}}
            }
        }
    }
    if($window){
        if(!$window.WaitForExit(5000) -or $window.ExitCode -ne 0){throw 'Owned window did not complete'}
        $w=Get-Content -Raw ($stem+'-window.json')|ConvertFrom-Json
        if($w.Exit -ne 0 -or $w.CloseReason -ne 'DurationReached' -or !$w.Moves -or !$w.Resizes){throw 'Window operations incomplete'}
        if($Stage -eq 'Interaction' -and (!$w.Minimizes -or !$w.Restores -or !$w.Occlusions)){throw 'Interaction operations incomplete'}
        $result.Window=$w
    }
    if($Stage -eq 'Interaction'){
        if(!$performance -or !$performance.WaitForExit(5000) -or $performance.ExitCode -ne 0){throw 'Performance regression did not complete'}
        $r=Get-Content -Raw ($stem+'-performance.result.json')|ConvertFrom-Json
        if(!$r.Passed){throw 'Performance regression failed'};$result.Performance=$r
    }
    $result.ApiObservationPassed=$true
}catch{$result.Error=$_.Exception.Message}
finally{
    foreach($child in @($window,$performance)){if($child -and !$child.HasExited){$child.Kill();$child.WaitForExit()}}
    $result.End=[DateTime]::UtcNow.ToString('o');$result.EndQpc=[Diagnostics.Stopwatch]::GetTimestamp();$result.ElapsedSeconds=$watch.Elapsed.TotalSeconds
    $bytes=[Text.Encoding]::UTF8.GetBytes(($result|ConvertTo-Json -Depth 7))
    $f=[IO.FileStream]::new(($stem+'.pending.json'),[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::Read,4096,[IO.FileOptions]::WriteThrough)
    try{$f.Write($bytes,0,$bytes.Length);$f.Flush($true)}finally{$f.Dispose()}
    Move-Item ($stem+'.pending.json') ($stem+'.result.json')
}
if(!$result.ApiObservationPassed){exit 1}
