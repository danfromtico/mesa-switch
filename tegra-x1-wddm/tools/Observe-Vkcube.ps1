# SPDX-License-Identifier: MIT
param([Parameter(Mandatory)][string]$Root,
      [Parameter(Mandatory)][string]$ExpectedBoot,
      [Parameter(Mandatory)][int]$ExpectedDwm)
$ErrorActionPreference='Stop'
$ProgressPreference='SilentlyContinue'
if((Get-Process -Id $PID).SessionId -eq 0){throw 'Interactive desktop required'}
if(Test-Path (Join-Path $Root 'result.json')){throw 'This observation already has a result'}
Add-Type -AssemblyName System.Windows.Forms,System.Drawing
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class CubeWindow {
 [StructLayout(LayoutKind.Sequential)] public struct Rect {public int Left,Top,Right,Bottom;}
 [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
 [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h,out Rect r);
 [DllImport("user32.dll")] public static extern bool MoveWindow(IntPtr h,int x,int y,int w,int hgt,bool paint);
 [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
 [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h,int n);
 [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
}
'@
$null=[CubeWindow]::SetProcessDPIAware()
$watch=[Diagnostics.Stopwatch]::StartNew()
$record=[ordered]@{Begin=[DateTime]::UtcNow.ToString('o');Pid=$PID;ExpectedBoot=$ExpectedBoot;ExpectedDwm=$ExpectedDwm;Exit=1;PhysicalAcceptance=$false;Events=@()}
$cube=$null;$desktop=$null;$cover=$null;$lastHeartbeat=-10
function Save-Record([string]$name,$value){
 $bytes=[Text.Encoding]::UTF8.GetBytes(($value|ConvertTo-Json -Depth 8))
 $f=[IO.FileStream]::new((Join-Path $Root $name),[IO.FileMode]::Create,[IO.FileAccess]::Write,[IO.FileShare]::Read,4096,[IO.FileOptions]::WriteThrough)
 try{$f.Write($bytes,0,$bytes.Length);$f.Flush($true)}finally{$f.Dispose()}
}
function Mark([string]$phase){
 $script:record.Phase=$phase
 $script:record.Events+=@{Phase=$phase;Utc=[DateTime]::UtcNow.ToString('o');ElapsedSeconds=$watch.Elapsed.TotalSeconds;Qpc=[Diagnostics.Stopwatch]::GetTimestamp()}
 Save-Record 'progress.json' $script:record
}
function Healthy {
 if(Test-Path (Join-Path $Root 'stop')){throw 'Stop requested'}
 $cube.Refresh()
 if($cube.HasExited){throw ('Cube exited early: '+$cube.ExitCode)}
 if(!$cube.Responding){throw 'Cube window stopped responding'}
 if(!(Get-Process -Id $ExpectedDwm -ErrorAction SilentlyContinue)){throw 'DWM process changed'}
 if($watch.Elapsed.TotalSeconds-$script:lastHeartbeat -ge 10){
  $script:record.LastTitle=$cube.MainWindowTitle
  $script:record.LastObservedUtc=[DateTime]::UtcNow.ToString('o')
  $script:record.ElapsedSeconds=$watch.Elapsed.TotalSeconds
  Save-Record 'progress.json' $script:record
  $script:lastHeartbeat=$watch.Elapsed.TotalSeconds
 }
 [Windows.Forms.Application]::DoEvents()
}
function Observe([int]$seconds){
 $until=$watch.Elapsed.TotalSeconds+$seconds
 while($watch.Elapsed.TotalSeconds -lt $until){Healthy;Start-Sleep -Milliseconds 500}
}
function Capture([string]$name){
 $r=[CubeWindow+Rect]::new()
 if(![CubeWindow]::GetWindowRect($cube.MainWindowHandle,[ref]$r)){throw 'Cannot read cube bounds'}
 $image=[Drawing.Bitmap]::new($r.Right-$r.Left,$r.Bottom-$r.Top)
 $g=[Drawing.Graphics]::FromImage($image)
 try{$g.CopyFromScreen($r.Left,$r.Top,0,0,$image.Size);$image.Save((Join-Path $Root ($name+'.png')))}finally{$g.Dispose();$image.Dispose()}
}
try{
 Mark 'Preflight'
 $boot=(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToUniversalTime().ToString('o')
 if($boot -ne $ExpectedBoot){throw 'Boot changed before observation'}
 $screen=[Windows.Forms.Screen]::PrimaryScreen.Bounds
 $record.Desktop=@{Width=$screen.Width;Height=$screen.Height}
 if($screen.Width -ne 1280 -or $screen.Height -ne 720){throw 'Expected fixed landscape desktop'}
 if(@(Get-Process vkcube -ErrorAction SilentlyContinue).Count){throw 'Another cube is already running'}
 $code=(Get-PnpDeviceProperty -InstanceId 'ACPI\SWOA0F01\0' -KeyName DEVPKEY_Device_ProblemCode).Data
 if($code -ne 0){throw ('GPU problem code '+$code)}
 $env:VKCUBE_TIMING_FILE=Join-Path $Root 'timing.csv'
 $identity=Start-Process -FilePath (Join-Path $Root 'nvk-windows-check.exe') -ArgumentList ('--loader "'+(Join-Path $Root 'vulkan-1.dll')+'"') -WorkingDirectory $Root -RedirectStandardOutput (Join-Path $Root 'identity.stdout') -RedirectStandardError (Join-Path $Root 'identity.stderr') -PassThru
 $null=$identity.Handle
 try{
  if(!$identity.WaitForExit(15000)){Stop-Process -Id $identity.Id -Force;throw 'Identity check timed out'}
  $record.Identity=Get-Content -Raw (Join-Path $Root 'identity.stdout')|ConvertFrom-Json
  if($identity.ExitCode -ne 0 -or !$record.Identity.matching_luid -or $record.Identity.physical_devices -ne 1){throw 'Native NVK identity mismatch'}
 }finally{$identity.Dispose()}
 Mark 'SmallWindow'
 $cube=Start-Process -FilePath (Join-Path $Root 'vkcube.exe') -ArgumentList '--width 320 --height 240 --gpu_number 0 --present_mode 2 --suppress_popups' -WorkingDirectory $Root -WindowStyle Normal -RedirectStandardOutput (Join-Path $Root 'cube.stdout') -RedirectStandardError (Join-Path $Root 'cube.stderr') -PassThru
 $null=$cube.Handle;$record.CubePid=$cube.Id
 $readyUntil=$watch.Elapsed.TotalSeconds+25
 do{Healthy;if($cube.MainWindowHandle -and $cube.MainWindowTitle -match 'FPS'){break};Start-Sleep -Milliseconds 250}while($watch.Elapsed.TotalSeconds -lt $readyUntil)
 if(!$cube.MainWindowHandle -or $cube.MainWindowTitle -notmatch 'FPS'){throw 'No measured Present cadence within25seconds'}
 $null=[CubeWindow]::ShowWindow($cube.MainWindowHandle,5)
 $null=[CubeWindow]::SetForegroundWindow($cube.MainWindowHandle)
 Observe 10;Capture 'small'
 Mark 'DesktopInteraction40s'
 $desktop=Start-Process -FilePath ($env:windir+'\System32\WindowsPowerShell\v1.0\powershell.exe') -ArgumentList ('-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "'+(Join-Path $Root 'Exercise-DesktopWindow.ps1')+'" -OutputPrefix "'+(Join-Path $Root 'desktop')+'" -DurationSeconds 40 -FullInteraction') -WorkingDirectory $Root -PassThru
 $null=$desktop.Handle
 $until=$watch.Elapsed.TotalSeconds+60
 do{Healthy;Start-Sleep -Milliseconds 500;$desktop.Refresh()}while(!$desktop.HasExited -and $watch.Elapsed.TotalSeconds -lt $until)
 if(!$desktop.HasExited){throw 'Desktop interaction timed out'}
 if($desktop.ExitCode -ne 0){throw 'Desktop interaction failed'}
 $record.DesktopInteraction=Get-Content -Raw (Join-Path $Root 'desktop.json')|ConvertFrom-Json
 Mark 'MoveResize'
 if(![CubeWindow]::MoveWindow($cube.MainWindowHandle,340,160,420,340,$true)){throw 'Move/resize failed'}
 $null=[CubeWindow]::SetForegroundWindow($cube.MainWindowHandle)
 Observe 8;Capture 'resized'
 Mark 'Occlusion'
 $cover=[Windows.Forms.Form]::new();$cover.Text='Cube occlusion';$cover.StartPosition='Manual';$cover.SetBounds(330,150,460,380);$cover.BackColor=[Drawing.Color]::DarkOrange;$cover.TopMost=$true
 $cover.Show();$null=[CubeWindow]::ShowWindow($cover.Handle,5)
 if(![CubeWindow]::IsWindowVisible($cover.Handle)){throw 'Occluder is hidden'}
 Observe 5;Capture 'occluded'
 $cover.Close();$cover.Dispose();$cover=$null
 Mark 'Restore'
 if(![CubeWindow]::MoveWindow($cube.MainWindowHandle,80,90,336,279,$true)){throw 'Restore size failed'}
 $null=[CubeWindow]::SetForegroundWindow($cube.MainWindowHandle)
 Observe 8;Capture 'restored'
 Mark 'Steady300s'
 Observe 300
 Mark 'SteadyComplete';Capture 'five-minutes'
 $record.LastTitle=$cube.MainWindowTitle
 if(!$cube.CloseMainWindow()){throw 'Could not close cube normally'}
 if(!$cube.WaitForExit(10000)){throw 'Cube did not exit after close'}
 $record.CubeExit=$cube.ExitCode
 if($cube.ExitCode -ne 0){throw ('Cube exit '+$cube.ExitCode)}
 $record.EndBoot=(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToUniversalTime().ToString('o')
 $record.DwmAfter=@(Get-Process dwm|Select-Object Id,StartTime)
 $record.CodeAfter=(Get-PnpDeviceProperty -InstanceId 'ACPI\SWOA0F01\0' -KeyName DEVPKEY_Device_ProblemCode).Data
 if($record.EndBoot -ne $ExpectedBoot -or $record.CodeAfter -ne 0 -or !($record.DwmAfter.Id -contains $ExpectedDwm)){throw 'Boot/DWM/device continuity failed'}
 $record.Exit=0;Mark 'Completed'
}catch{
 $record.Error=($_|Out-String);Mark 'Failed'
}finally{
 if($cover){$cover.Dispose()}
 if($desktop){if(!$desktop.HasExited){Stop-Process -Id $desktop.Id -Force};$desktop.Dispose()}
 if($cube){
  if(!$cube.HasExited){$null=$cube.CloseMainWindow();if(!$cube.WaitForExit(5000)){Stop-Process -Id $cube.Id -Force;$record.ForcedCubeStop=$true}}
  $cube.Dispose()
 }
 $record.End=[DateTime]::UtcNow.ToString('o');$record.ElapsedSeconds=$watch.Elapsed.TotalSeconds
 Save-Record 'result.json' $record
}
exit $record.Exit
