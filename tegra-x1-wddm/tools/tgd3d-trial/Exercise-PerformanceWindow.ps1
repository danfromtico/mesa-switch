# SPDX-License-Identifier: MIT
# Bounded, owned performance-window regression; no driver update.
param([Parameter(Mandatory=$true)][string]$OutputPrefix, [ValidateSet('0.1.0.59','0.1.0.158')][string]$ExpectedVersion='0.1.0.158')
$ErrorActionPreference='Stop';$id='ACPI\SWOA0F01\0'
$exe='C:\Users\Public\Desktop\LitePerformanceCheck.exe'
function Version { (Get-PnpDeviceProperty -InstanceId $id -KeyName DEVPKEY_Device_DriverVersion).Data }
if((Get-Process -Id $PID).SessionId -eq 0 -or (Version) -ne $ExpectedVersion) { throw 'Expected the selected interactive driver' }
if((Get-FileHash $exe).Hash -ne 'CA127482BBB59AC6C1B2F576F6B9793231C1B865A00057EC1171892F210929D4') {throw 'Performance application differs'}
function Save($suffix,$record) {
 $bytes=[Text.Encoding]::UTF8.GetBytes(($record|ConvertTo-Json -Depth 5))
 $f=[IO.FileStream]::new(($OutputPrefix+$suffix),[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::Read,4096,[IO.FileOptions]::WriteThrough)
 try{$f.Write($bytes,0,$bytes.Length);$f.Flush($true)}finally{$f.Dispose()}
}
$r=[ordered]@{Begin=[DateTime]::UtcNow.ToString('o');BeginQpc=[Diagnostics.Stopwatch]::GetTimestamp();Frequency=[Diagnostics.Stopwatch]::Frequency;DriverBefore=(Version);Boot=(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToUniversalTime().ToString('o');Target=$exe;Arguments='';Passed=$false;PhysicalPanelVerified=$false}
Save '.before.json' $r
$app=$null
try {
 $app=Start-Process -FilePath $exe -PassThru
 $null=$app.Handle
 $r.AppPid=$app.Id;$r.AppStart=$app.StartTime.ToUniversalTime().ToString('o')
 Save '.launched.json' $r
 if(!$app.WaitForInputIdle(10000)) {throw 'Application did not become input-idle'}
 $watch=[Diagnostics.Stopwatch]::StartNew()
 while($watch.Elapsed.TotalSeconds -lt 30) {
  if($app.HasExited) {throw ('Application exited early: '+$app.ExitCode)}
  Start-Sleep -Milliseconds 200
 }
 $app.Refresh();$r.WindowHandle=$app.MainWindowHandle.ToInt64();$r.Title=$app.MainWindowTitle;$r.RunningSeconds=$watch.Elapsed.TotalSeconds
 if(!$r.WindowHandle) {throw 'Application window missing'}
 Save '.running.json' $r
 if(!$app.CloseMainWindow() -or !$app.WaitForExit(5000)) {throw 'Application did not close normally'}
 $r.ExitCode=$app.ExitCode;$r.DriverAfter=(Version)
 $r.BootAfter=(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToUniversalTime().ToString('o')
 if($r.ExitCode -ne 0 -or $r.DriverAfter -ne $ExpectedVersion) {throw 'Application or baseline state differs'}
 # The same controller and application remained alive across this interval.
 # CIM boot wall time is retained for correlation, not compared across clock corrections.
 $r.Passed=$true
} catch {$r.Error=$_.Exception.Message}
finally {
 if($app -and !$app.HasExited){$app.Kill();$app.WaitForExit()}
 $r.End=[DateTime]::UtcNow.ToString('o');$r.EndQpc=[Diagnostics.Stopwatch]::GetTimestamp()
 Save '.result.json' $r
}
if(!$r.Passed){exit 1}
