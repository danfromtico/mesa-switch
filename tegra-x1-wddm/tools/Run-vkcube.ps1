param([ValidateRange(0,36000)][int]$FrameCount=0)
$ErrorActionPreference='Stop'
$arguments='--width 320 --height 240 --gpu_number 0 --present_mode 2'
if($FrameCount){$arguments+=' --c '+$FrameCount+' --suppress_popups'}
$record=[ordered]@{Begin=[DateTime]::UtcNow.ToString('o');Arguments=$arguments;PhysicalAcceptance=$false}
$watch=[Diagnostics.Stopwatch]::StartNew()
$p=Start-Process -FilePath (Join-Path $PSScriptRoot 'vkcube.exe') -WorkingDirectory $PSScriptRoot -ArgumentList $arguments -WindowStyle Normal -RedirectStandardOutput (Join-Path $PSScriptRoot 'last-run.stdout.log') -RedirectStandardError (Join-Path $PSScriptRoot 'last-run.stderr.log') -PassThru
$null=$p.Handle
$record.Pid=$p.Id
$record|ConvertTo-Json|Set-Content -Encoding UTF8 (Join-Path $PSScriptRoot 'last-run.started.json')
try{
 $p.WaitForExit();$watch.Stop();$record.Exit=$p.ExitCode
 $record.End=[DateTime]::UtcNow.ToString('o');$record.ElapsedSeconds=$watch.Elapsed.TotalSeconds
 $record|ConvertTo-Json|Set-Content -Encoding UTF8 (Join-Path $PSScriptRoot 'last-run.json')
 exit $p.ExitCode
}finally{$p.Dispose()}
