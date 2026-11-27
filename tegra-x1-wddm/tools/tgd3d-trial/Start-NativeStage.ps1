# SPDX-License-Identifier: MIT
param([ValidateSet('Static','Window','Interaction')][string]$Stage='Static', [string]$PreviousPhysicalObservation)
. (Join-Path $PSScriptRoot 'Common.ps1')
Require-Administrator
$null=Check-Package
$backup=Check-Recovery
$duration=@{Static=30;Window=60;Interaction=300}[$Stage]
if((Device-Property 'DriverVersion') -ne '0.1.0.158' -or (Device-Property 'ProblemCode') -ne 0){throw 'Native KMD is not ready'}
$task=Get-ScheduledTask 'Tgd3dTrialNative' -ErrorAction SilentlyContinue
if($task -and $task.State -eq 'Running'){throw 'A native stage is already running'}
$previous=@{Window='Static';Interaction='Window'}[$Stage]
$expected=$null
if($previous){
    $prior=Read-Json (Join-Path $PSScriptRoot ('evidence\stage-'+$previous+'.result.json'))
    if(!$prior.ApiObservationPassed -or !$prior.Luid -or [string]::IsNullOrWhiteSpace($PreviousPhysicalObservation)){throw 'Previous API stage and physical observation are required'}
    $expected=$prior.Luid
}
$requestPath=Join-Path $PSScriptRoot ('stage-'+$Stage+'.request.json')
if(Test-Path $requestPath){throw 'This stage has already been requested'}
$request=@{Stage=$Stage;DurationSeconds=$duration;ExpectedLuid=$expected;PreviousStage=$previous;PreviousPhysicalObservation=$PreviousPhysicalObservation;Utc=[DateTime]::UtcNow.ToString('o');Qpc=[Diagnostics.Stopwatch]::GetTimestamp();ControllerPid=$PID;ControllerStartFileTime=(Get-Process -Id $PID).StartTime.ToUniversalTime().ToFileTimeUtc();ManifestSha256=(File-Hash (Join-Path $PSScriptRoot 'trial-components.json'))}
# The failed Window trial left a 408-byte, all-zero Set-Content file. Flush
# the request before dispatch, and publish only the completed JSON. Retain
# an interrupted pending file rather than treating it as a completed request.
$pending=$requestPath+'.pending'
$bytes=[Text.Encoding]::UTF8.GetBytes(($request|ConvertTo-Json))
$stream=[IO.FileStream]::new($pending,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::Read,4096,[IO.FileOptions]::WriteThrough)
try{$stream.Write($bytes,0,$bytes.Length);$stream.Flush($true)}finally{$stream.Dispose()}
[IO.File]::Move($pending,$requestPath)
$ps="$env:windir\System32\WindowsPowerShell\v1.0\powershell.exe"
$action=New-ScheduledTaskAction -Execute $ps -Argument ('-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "'+$PSScriptRoot+'\Observe-Native.ps1" -Stage '+$Stage)
$principal=New-ScheduledTaskPrincipal -UserId $backup.User -LogonType Interactive -RunLevel Limited
$settings=New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit (New-TimeSpan -Seconds ($duration+40))
Register-ScheduledTask -TaskName 'Tgd3dTrialNative' -Action $action -Principal $principal -Settings $settings -Force|Out-Null
try {
    Start-ScheduledTask 'Tgd3dTrialNative'
    $watch=[Diagnostics.Stopwatch]::StartNew()
    $resultPath=Join-Path $PSScriptRoot ('evidence\stage-'+$Stage+'.result.json')
    while(!(Test-Path $resultPath) -and $watch.Elapsed.TotalSeconds -lt $duration+45){Start-Sleep -Milliseconds 300}
    $result=Read-Json $resultPath
    if(!$result.ApiObservationPassed){throw ('Native stage failed: '+$result.Error)}
    $result|ConvertTo-Json -Depth 6
} catch {
    $_|Out-String|Set-Content (Join-Path $PSScriptRoot ('stage-'+$Stage+'.controller-error.txt')) -Encoding UTF8
    throw
}
