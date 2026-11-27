# SPDX-License-Identifier: MIT
# Run in the interactive session as a bounded child of the trial controller.
# Pair the PNG with the trial's active display-path/LUID records. A desktop
# capture is composition evidence, not proof of the physical panel's output.
param(
 [Parameter(Mandatory=$true)][string]$OutputPrefix,
 [ValidateSet('None','Start','QuickSettings','Dismiss')][string]$Shortcut='None'
)
$ErrorActionPreference='Stop';$ProgressPreference='SilentlyContinue'
$started=[DateTime]::UtcNow.ToString('o')
if((Get-Process -Id $PID).SessionId -eq 0){throw 'Capture requires an interactive session'}
foreach($suffix in '.png','.json','.pending.png','.geometry.json'){
 if(Test-Path -LiteralPath ($OutputPrefix+$suffix)){throw 'Capture output already exists'}
}
Add-Type @'
using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public static class TgDesktopCapture {
 [StructLayout(LayoutKind.Sequential)] public struct Rect { public int Left,Top,Right,Bottom; }
 // Native 64-bit INPUT: DWORD type, aligned union, total size 40 bytes.
 // Only the KEYBDINPUT fields used by these shell shortcuts are nonzero.
 [StructLayout(LayoutKind.Explicit,Size=40)] private struct Input {
  [FieldOffset(0)] public uint Type;
  [FieldOffset(8)] public ushort Key;
  [FieldOffset(12)] public uint Flags;
 }
 public sealed class ChildWindow {
  public string Class; public bool Visible,BoundsValid; public Rect Bounds;
  public uint Pid,Dpi;
 }
 private delegate bool EnumWindowProc(IntPtr window,IntPtr parameter);
 [DllImport("user32.dll")] public static extern IntPtr SetThreadDpiAwarenessContext(IntPtr context);
 [DllImport("user32.dll")] public static extern IntPtr OpenInputDesktop(uint flags,bool inherit,uint access);
 [DllImport("user32.dll")] public static extern bool CloseDesktop(IntPtr desktop);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern bool GetUserObjectInformation(IntPtr handle,int index,StringBuilder value,uint bytes,out uint needed);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string cls,string title);
 [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr window);
 [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr window,out Rect rect);
 [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr window,out uint process);
 [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr window);
 [DllImport("user32.dll")] private static extern IntPtr GetForegroundWindow();
 [DllImport("user32.dll")] private static extern short GetAsyncKeyState(int key);
 [DllImport("user32.dll",SetLastError=true)] private static extern uint SendInput(uint count,Input[] inputs,int size);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)] private static extern int GetClassName(IntPtr window,StringBuilder name,int count);
 [DllImport("user32.dll")] private static extern bool EnumChildWindows(IntPtr parent,EnumWindowProc callback,IntPtr parameter);
 public static ChildWindow[] Children(IntPtr parent) {
  var children=new List<ChildWindow>();
  // A null parent enumerates unrelated top-level windows, not a missing bar.
  if(parent==IntPtr.Zero) return children.ToArray();
  EnumChildWindows(parent,delegate(IntPtr window,IntPtr parameter) {
   var child=new ChildWindow();var name=new StringBuilder(256);
   GetClassName(window,name,name.Capacity);child.Class=name.ToString();
   child.Visible=IsWindowVisible(window);
   child.BoundsValid=GetWindowRect(window,out child.Bounds);
   GetWindowThreadProcessId(window,out child.Pid);
   child.Dpi=GetDpiForWindow(window);children.Add(child);return true;
  },IntPtr.Zero);
  return children.ToArray();
 }
 public static ChildWindow Foreground() {
  IntPtr window=GetForegroundWindow();var result=new ChildWindow();
  var name=new StringBuilder(256);GetClassName(window,name,name.Capacity);
  result.Class=name.ToString();result.Visible=IsWindowVisible(window);
  result.BoundsValid=GetWindowRect(window,out result.Bounds);
  GetWindowThreadProcessId(window,out result.Pid);result.Dpi=GetDpiForWindow(window);
  return result;
 }
 public static uint ShellShortcut(string shortcut) {
  if(shortcut=="None") return 0;
  if(IntPtr.Size!=8) throw new InvalidOperationException("Shortcut input requires a 64-bit process");
  if(InputDesktop()!="Default") throw new InvalidOperationException("Shortcut requires Default input desktop");
  foreach(int key in new int[]{0x10,0x11,0x12,0x5b,0x5c})
   if((GetAsyncKeyState(key)&0x8000)!=0)
    throw new InvalidOperationException("A physical modifier key is held; shortcut was not sent");
  ushort[] keys;
  if(shortcut=="Start") keys=new ushort[]{0x5b};
  else if(shortcut=="QuickSettings") keys=new ushort[]{0x5b,0x41};
  else if(shortcut=="Dismiss") keys=new ushort[]{0x1b};
  else throw new ArgumentException("Unknown shell shortcut");
  var input=new Input[keys.Length*2];
  for(int i=0;i<keys.Length;i++) {
   input[i]=new Input{Type=1,Key=keys[i]};
   input[input.Length-1-i]=new Input{Type=1,Key=keys[i],Flags=2};
  }
  uint sent=SendInput((uint)input.Length,input,Marshal.SizeOf(typeof(Input)));
  if(sent!=input.Length) {
   int error=Marshal.GetLastWin32Error();
   // A partial insertion must not leave an injected key held down.
   var release=new Input[keys.Length];
   for(int i=0;i<keys.Length;i++) release[i]=new Input{Type=1,Key=keys[keys.Length-1-i],Flags=2};
   SendInput((uint)release.Length,release,Marshal.SizeOf(typeof(Input)));
   throw new InvalidOperationException("SendInput accepted "+sent+"/"+input.Length+" events; error="+error);
  }
  return sent;
 }
 public static string InputDesktop() {
  IntPtr desktop=OpenInputDesktop(0,false,1);
  if(desktop==IntPtr.Zero) throw new InvalidOperationException("Input desktop unavailable");
  try { var name=new StringBuilder(256);uint needed;
   if(!GetUserObjectInformation(desktop,2,name,512,out needed)) throw new InvalidOperationException("Desktop name unavailable");
   return name.ToString();
  } finally { CloseDesktop(desktop); }
 }
}
'@
$desktop=[TgDesktopCapture]::InputDesktop()
if($desktop -ne 'Default'){throw ('Capture requires Default desktop; current='+$desktop)}
$dpi=[TgDesktopCapture]::SetThreadDpiAwarenessContext([IntPtr](-4))
if($dpi -eq [IntPtr]::Zero){throw 'Could not select physical pixel coordinates'}
$record=@{Begin=$started;Session=(Get-Process -Id $PID).SessionId;Desktop=$desktop;PhysicalPanelVerified=$false;Exit=1;Shortcut=$Shortcut}
try{
 Add-Type -AssemblyName System.Drawing
 Add-Type -AssemblyName System.Windows.Forms
 $id='ACPI\SWOA0F01\0'
 $record.DriverBefore=(Get-PnpDeviceProperty -InstanceId $id -KeyName DEVPKEY_Device_DriverVersion).Data
 $record.ProblemBefore=(Get-PnpDeviceProperty -InstanceId $id -KeyName DEVPKEY_Device_ProblemCode).Data
 if($Shortcut -ne 'None') {
  $record.ShortcutInputs=[TgDesktopCapture]::ShellShortcut($Shortcut)
  $record.ShortcutUtc=[DateTime]::UtcNow.ToString('o')
  Start-Sleep -Milliseconds 1500
 }
 $record.Foreground=[TgDesktopCapture]::Foreground()
 $bounds=[Windows.Forms.Screen]::PrimaryScreen.Bounds
 $record.Bounds=@{X=$bounds.X;Y=$bounds.Y;Width=$bounds.Width;Height=$bounds.Height}
 $work=[Windows.Forms.Screen]::PrimaryScreen.WorkingArea
 $record.WorkArea=@{X=$work.X;Y=$work.Y;Width=$work.Width;Height=$work.Height}
 $record.Screens=@([Windows.Forms.Screen]::AllScreens|ForEach-Object {
  @{Name=$_.DeviceName;Primary=$_.Primary;Bounds=@{X=$_.Bounds.X;Y=$_.Bounds.Y;Width=$_.Bounds.Width;Height=$_.Bounds.Height}}
 })
 $bar=[TgDesktopCapture]::FindWindow('Shell_TrayWnd',$null)
 $rect=New-Object TgDesktopCapture+Rect;[uint32]$owner=0
 $valid=[TgDesktopCapture]::GetWindowRect($bar,[ref]$rect)
 $null=[TgDesktopCapture]::GetWindowThreadProcessId($bar,[ref]$owner)
 $record.Taskbar=@{Found=$bar -ne [IntPtr]::Zero;Visible=[TgDesktopCapture]::IsWindowVisible($bar);Pid=$owner;BoundsValid=$valid;Bounds=$rect;Dpi=[TgDesktopCapture]::GetDpiForWindow($bar);Children=[TgDesktopCapture]::Children($bar)}
 # Retain geometry even if the controller must terminate CopyFromScreen.
 $record.GeometryUtc=[DateTime]::UtcNow.ToString('o')
 $record|ConvertTo-Json -Depth 5|Set-Content -LiteralPath ($OutputPrefix+'.geometry.json') -Encoding UTF8
 $checkpoint=[IO.File]::Open(($OutputPrefix+'.geometry.json'),[IO.FileMode]::Open,[IO.FileAccess]::ReadWrite,[IO.FileShare]::Read)
 try{$checkpoint.Flush($true)}finally{$checkpoint.Dispose()}
 $image=New-Object Drawing.Bitmap($bounds.Width,$bounds.Height)
 try{
  $g=[Drawing.Graphics]::FromImage($image)
  try{$g.CopyFromScreen($bounds.X,$bounds.Y,0,0,$image.Size)}finally{$g.Dispose()}
  $image.Save(($OutputPrefix+'.pending.png'),[Drawing.Imaging.ImageFormat]::Png)
 }finally{$image.Dispose()}
 Move-Item -LiteralPath ($OutputPrefix+'.pending.png') -Destination ($OutputPrefix+'.png')
 $record.DriverAfter=(Get-PnpDeviceProperty -InstanceId $id -KeyName DEVPKEY_Device_DriverVersion).Data
 $record.ProblemAfter=(Get-PnpDeviceProperty -InstanceId $id -KeyName DEVPKEY_Device_ProblemCode).Data
 $record.DesktopAfter=[TgDesktopCapture]::InputDesktop()
 $record.ImageSha256=(Get-FileHash -LiteralPath ($OutputPrefix+'.png')).Hash
 $record.Exit=0
}catch{$record.Error=($_|Out-String)}finally{
 $record.End=[DateTime]::UtcNow.ToString('o')
 $null=[TgDesktopCapture]::SetThreadDpiAwarenessContext($dpi)
 $record|ConvertTo-Json -Depth 5|Set-Content -LiteralPath ($OutputPrefix+'.json') -Encoding UTF8
}
if($record.Exit){exit 1}
