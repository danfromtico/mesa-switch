# SPDX-License-Identifier: MIT
param([Parameter(Mandatory)][string]$Root,
      [Parameter(Mandatory)][string]$ExpectedBoot,
      [Parameter(Mandatory)][int]$ExpectedDwm)
$ErrorActionPreference='Stop';$ProgressPreference='SilentlyContinue'
Add-Type -AssemblyName System.Windows.Forms,System.Drawing
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class CubeOcclusion {
 [StructLayout(LayoutKind.Sequential)] public struct Rect {public int Left,Top,Right,Bottom;}
 [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
 [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h,out Rect r);
 [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h,int n);
 [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
 [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
}
'@
$null=[CubeOcclusion]::SetProcessDPIAware()
$state=[ordered]@{Begin=[DateTime]::UtcNow.ToString('o');Exit=1;OwnsCube=$false;PhysicalAcceptance=$false;Events=@()}
$cube=$null;$cover=$null
function Record([string]$phase){
 $state.Phase=$phase;$state.Events+=@{Phase=$phase;Utc=[DateTime]::UtcNow.ToString('o')}
 $state|ConvertTo-Json -Depth 5|Set-Content -Encoding UTF8 (Join-Path $Root 'occlusion-only-state.json')
}
function Observe([int]$seconds){
 $clock=[Diagnostics.Stopwatch]::StartNew()
 while($clock.Elapsed.TotalSeconds -lt $seconds){
  $cube.Refresh()
  if($cube.HasExited -or !$cube.Responding){throw 'Cube stopped responding or exited'}
  if(!(Get-Process -Id $ExpectedDwm -ErrorAction SilentlyContinue)){throw 'DWM changed'}
  [Windows.Forms.Application]::DoEvents();Start-Sleep -Milliseconds 200
 }
}
function Capture([string]$name){
 $r=[CubeOcclusion+Rect]::new();if(![CubeOcclusion]::GetWindowRect($cube.MainWindowHandle,[ref]$r)){throw 'Missing cube HWND'}
 $b=[Drawing.Bitmap]::new($r.Right-$r.Left,$r.Bottom-$r.Top);$g=[Drawing.Graphics]::FromImage($b)
 try{$g.CopyFromScreen($r.Left,$r.Top,0,0,$b.Size);$b.Save((Join-Path $Root ('occlusion-only-'+$name+'.png')))}finally{$g.Dispose();$b.Dispose()}
}
try{
 Record 'Preflight'
 if((Get-Process -Id $PID).SessionId -eq 0){throw 'Interactive session required'}
 if((Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToUniversalTime().ToString('o') -ne $ExpectedBoot){throw 'Boot changed'}
 $existing=@(Get-Process vkcube -ErrorAction SilentlyContinue)
 if($existing.Count -gt 1){throw 'Multiple cube processes'}
 if($existing.Count -eq 1){
  $cube=$existing[0]
  if($cube.SessionId -ne (Get-Process -Id $PID).SessionId){throw 'Cube is on another desktop session'}
 }else{
  $env:VKCUBE_TIMING_FILE=Join-Path $Root 'occlusion-only-timing.csv'
  $cube=Start-Process -FilePath (Join-Path $Root 'vkcube.exe') -ArgumentList '--width 320 --height 240 --gpu_number 0 --present_mode 2 --suppress_popups' -WorkingDirectory $Root -WindowStyle Normal -RedirectStandardOutput (Join-Path $Root 'occlusion-only.stdout') -RedirectStandardError (Join-Path $Root 'occlusion-only.stderr') -PassThru
  $null=$cube.Handle;$state.OwnsCube=$true
 }
 $state.CubePid=$cube.Id
 Observe 5
 if($cube.MainWindowHandle.ToInt64() -eq 0){throw 'No visible cube window'}
 $null=[CubeOcclusion]::ShowWindow($cube.MainWindowHandle,5)
 $null=[CubeOcclusion]::SetForegroundWindow($cube.MainWindowHandle)
 Observe 3;Capture 'before'
 $r=[CubeOcclusion+Rect]::new();$null=[CubeOcclusion]::GetWindowRect($cube.MainWindowHandle,[ref]$r)
 $cover=[Windows.Forms.Form]::new();$cover.Text='Occlusion test - 15 seconds';$cover.StartPosition='Manual'
 $cover.SetBounds($r.Left,$r.Top,$r.Right-$r.Left,$r.Bottom-$r.Top);$cover.BackColor=[Drawing.Color]::DarkOrange;$cover.TopMost=$true
 $cover.Show();$null=[CubeOcclusion]::ShowWindow($cover.Handle,5)
 if(![CubeOcclusion]::IsWindowVisible($cover.Handle)){throw 'Occluder is hidden'}
 Record 'OrangeVisible15s';Observe 3;Capture 'covered';Observe 12
 $cover.Close();$cover.Dispose();$cover=$null
 $null=[CubeOcclusion]::SetForegroundWindow($cube.MainWindowHandle)
 Record 'Restored';Observe 8;Capture 'restored'
 $state.LastTitle=$cube.MainWindowTitle
 $state.Exit=0;Record 'Completed'
}catch{$state.Error=($_|Out-String);Record 'Failed'}finally{
 if($cover){$cover.Dispose()}
 if($cube){
  if($state.OwnsCube -and !$cube.HasExited){
   $null=$cube.CloseMainWindow()
   if(!$cube.WaitForExit(10000)){Stop-Process -Id $cube.Id -Force;$state.Exit=1;$state.ForcedStop=$true}
   else{$state.CubeExit=$cube.ExitCode;if($cube.ExitCode -ne 0){$state.Exit=1}}
  }
  $cube.Dispose()
 }
 $state.End=[DateTime]::UtcNow.ToString('o')
 $state|ConvertTo-Json -Depth 5|Set-Content -Encoding UTF8 (Join-Path $Root 'occlusion-only-result.json')
}
exit $state.Exit
