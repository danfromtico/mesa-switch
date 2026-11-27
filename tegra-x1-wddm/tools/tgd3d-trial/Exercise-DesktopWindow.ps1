# SPDX-License-Identifier: MIT
param(
    [Parameter(Mandatory)][string]$OutputPrefix,
    [ValidateRange(3,400)][int]$DurationSeconds=360,
    [string]$CandidateVersion='0.1.0.158',
    [switch]$FullInteraction
)
$ErrorActionPreference='Stop'
if((Get-Process -Id $PID).SessionId -eq 0){throw 'Use an interactive desktop session'}
if(Test-Path ($OutputPrefix+'.json')){throw 'Window exercise result already exists'}
Add-Type -AssemblyName System.Windows.Forms,System.Drawing
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class Tgd3dWindowVisibility {
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr window, int command);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr window);
}
'@
[Windows.Forms.Application]::EnableVisualStyles()
$form=New-Object Windows.Forms.Form
$form.Text='Desktop observation '+$CandidateVersion
$form.StartPosition='Manual'
$form.MaximizeBox=$false;$form.MinimizeBox=$false
$area=[Windows.Forms.Screen]::PrimaryScreen.WorkingArea
$form.SetBounds($area.X+24,$area.Y+48,[Math]::Min(360,$area.Width-48),[Math]::Min(260,$area.Height-96))
$grid=New-Object Windows.Forms.TableLayoutPanel
$grid.Dock='Fill';$grid.ColumnCount=3;$grid.RowCount=2
for($i=0;$i -lt 3;$i++){$null=$grid.ColumnStyles.Add([Windows.Forms.ColumnStyle]::new([Windows.Forms.SizeType]::Percent,33.333))}
for($i=0;$i -lt 2;$i++){$null=$grid.RowStyles.Add([Windows.Forms.RowStyle]::new([Windows.Forms.SizeType]::Percent,50))}
$colors=@([Drawing.Color]::Red,[Drawing.Color]::Lime,[Drawing.Color]::Blue,[Drawing.Color]::Cyan,[Drawing.Color]::Magenta,[Drawing.Color]::Yellow)
for($i=0;$i -lt 6;$i++){
    $label=New-Object Windows.Forms.Label
    $label.Text=[string]($i+1);$label.Dock='Fill';$label.TextAlign='MiddleCenter'
    $label.BackColor=$colors[$i];$label.ForeColor=if($i -eq 2){[Drawing.Color]::White}else{[Drawing.Color]::Black}
    $null=$grid.Controls.Add($label,($i%3),[int][Math]::Floor($i/3))
}
$form.Controls.Add($grid)
$overlay=New-Object Windows.Forms.Form
$overlay.Text='Occlusion control';$overlay.BackColor=[Drawing.Color]::DarkOrange
$overlay.StartPosition='Manual';$overlay.SetBounds($area.X+70,$area.Y+100,220,150)
$state=[ordered]@{Begin=[DateTime]::UtcNow.ToString('o');Pid=$PID;Session=(Get-Process -Id $PID).SessionId;Moves=0;Resizes=0;Minimizes=0;Restores=0;Occlusions=0;Exit=1;Scope='Owned GDI/WinForms window exercises DWM composition; no native pixel-readback claim'}
$watch=[Diagnostics.Stopwatch]::new();$script:lastStep=0;$script:closeReason='UserClosed'
function Save-WindowRecord([string]$suffix,$record){
    $bytes=[Text.Encoding]::UTF8.GetBytes(($record|ConvertTo-Json -Depth 4))
    $f=[IO.FileStream]::new(($OutputPrefix+$suffix),[IO.FileMode]::Create,[IO.FileAccess]::Write,[IO.FileShare]::Read,4096,[IO.FileOptions]::WriteThrough)
    try{$f.Write($bytes,0,$bytes.Length);$f.Flush($true)}finally{$f.Dispose()}
}
$timer=New-Object Windows.Forms.Timer
$timer.Interval=500
$form.add_Shown({
    # STARTUPINFO from the hidden PowerShell launch can override the first
    # ShowWindow call. WinForms.Visible then describes managed state only.
    # A subsequent explicit show must make this owned test window visible.
    $null=[Tgd3dWindowVisibility]::ShowWindow($form.Handle,5)
    $form.Activate()
    if(![Tgd3dWindowVisibility]::IsWindowVisible($form.Handle)) {
        $state.Error='Owned test HWND remains hidden';$script:closeReason='Error';$form.Close();return
    }
    $watch.Start();$timer.Start()
    Save-WindowRecord '.ready.json' @{Utc=[DateTime]::UtcNow.ToString('o');Pid=$PID;Session=$state.Session;Handle=$form.Handle.ToInt64();Visible=$form.Visible;NativeVisible=[Tgd3dWindowVisibility]::IsWindowVisible($form.Handle)}
})
$timer.add_Tick({
    try{
        if(Test-Path ($OutputPrefix+'.stop')){$script:closeReason='StopRequested';$form.Close();return}
        if($watch.Elapsed.TotalSeconds -ge $DurationSeconds){$script:closeReason='DurationReached';$form.Close();return}
        $step=[int][Math]::Floor($watch.Elapsed.TotalSeconds/5)
        if($step -ne $script:lastStep){
            $width=[Math]::Min((280+($step%3)*60),$area.Width-48)
            $height=[Math]::Min((190+($step%2)*70),$area.Height-96)
            $x=$area.X+24+(($step*37)%[Math]::Max(1,($area.Width-$width-48)))
            $y=$area.Y+48+(($step*29)%[Math]::Max(1,($area.Height-$height-96)))
            if($FullInteraction) {
                switch($step % 8) {
                    3 {$form.WindowState='Minimized';$state.Minimizes++}
                    4 {$form.WindowState='Normal';$state.Restores++}
                    6 {$overlay.Show($form);$state.Occlusions++}
                    7 {$overlay.Hide()}
                }
            }
            $before=$form.Bounds
            $form.SetBounds($x,$y,$width,$height)
            $after=$form.Bounds
            if($before.X -ne $after.X -or $before.Y -ne $after.Y){$state.Moves++}
            if($before.Width -ne $after.Width -or $before.Height -ne $after.Height){$state.Resizes++}
            $script:lastStep=$step
            Save-WindowRecord '.position.json' @{Utc=[DateTime]::UtcNow.ToString('o');ElapsedSeconds=$watch.Elapsed.TotalSeconds;Moves=$state.Moves;Resizes=$state.Resizes;X=$after.X;Y=$after.Y;Width=$after.Width;Height=$after.Height;Visible=$form.Visible;NativeVisible=[Tgd3dWindowVisibility]::IsWindowVisible($form.Handle)}
        }
    }catch{$state.Error=($_|Out-String);$script:closeReason='Error';$form.Close()}
})
try{
    [Windows.Forms.Application]::Run($form)
    if($script:closeReason -in @('StopRequested','DurationReached')){$state.Exit=0}
}catch{$state.Error=($_|Out-String)}finally{
    $timer.Stop();$timer.Dispose();$overlay.Dispose();$form.Dispose();$watch.Stop()
    $state.End=[DateTime]::UtcNow.ToString('o');$state.ElapsedSeconds=$watch.Elapsed.TotalSeconds;$state.CloseReason=$script:closeReason
    Save-WindowRecord '.json' $state
}
if($state.Exit -ne 0){exit 1}
