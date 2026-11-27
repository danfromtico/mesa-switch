# Desktop image evidence

Use [`Capture-DesktopEvidence.ps1`](../tools/Capture-DesktopEvidence.ps1) in
the logged-in interactive session to inspect taskbar position/content, Start
and tray visibility, and missing desktop regions. Inspect the returned PNG;
a visible `Shell_TrayWnd` rectangle or successful Present is not a pixel check.
Opening the Start menu remains a separate interaction check.

The script captures the actual primary-screen bounds in physical pixels,
without assuming portrait dimensions. Its adjacent JSON records UTC start/end,
session and input desktop, driver version/problem before and after, taskbar
owner/rectangle and PNG SHA256. The geometry extension also records all screen
bounds, the primary work area, taskbar window DPI and its child-window classes,
rectangles, visibility and DPI. This distinguishes a shorter/off-screen shell
layout from missing pixels within a correctly positioned window. Child windows
do not necessarily correspond to individual XAML task icons; inspect the PNG
as well. Window DPI is interpreted according to the window's own awareness,
as documented by [GetDpiForWindow](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getdpiforwindow).
These observations do not change DPI, layout or display settings.
A separate `.geometry.json` checkpoint is flushed before `CopyFromScreen`, so
geometry can survive a capture timeout. That checkpoint alone is not a completed
screenshot; completion still requires the final JSON and matching PNG.
Keep the trial's active display-path/LUID and
exact component hashes alongside these files: the installed driver version
alone does not identify the adapter that composed the image.

## Trial integration

Retain the important probe/trace records before attempting a screenshot. Run
the capture as a hidden child of the existing interactive trial controller,
with a unique prefix for each observation. For example, where `$captureScript`
and `$outputPrefix` are absolute paths chosen by that controller:

```powershell
$arguments = '-NoProfile -ExecutionPolicy Bypass -File "{0}" -OutputPrefix "{1}"' -f $captureScript, $outputPrefix
$capture = Start-Process "$env:windir\System32\WindowsPowerShell\v1.0\powershell.exe" `
    -ArgumentList $arguments -WindowStyle Hidden -PassThru
if (-not $capture.WaitForExit(30000)) {
    $capture.Kill()
    throw 'Desktop capture exceeded 30 seconds'
}
if ($capture.ExitCode -ne 0) { throw "Desktop capture failed: $($capture.ExitCode)" }
```

The surrounding trial must retain its automatic restoration path even if
capture fails or times out. Session 0 or a non-`Default` input desktop is
rejected; do not interpret that failure as a black rendered frame. Existing
output files are preserved rather than overwritten. Fetch the PNG and JSON,
check their device hashes and inspect the actual image during the native
interval before calling the desktop correct. Do not substitute a screenshot
taken after `.59` restoration for a native result.

A screenshot observes the Windows desktop capture path. It does not establish
physical scanout, panel rotation, absence of a physical black screen, or
sustained stability. Keep `PhysicalPanelVerified=false` and correlate with
scanout records and physical observation when validating those properties.

## Verified baseline control

`desktop-capture-control-20261111` captures the recovered `.59` Code 0 desktop
from 23:30:06 to 23:30:12 UTC. The driver remains `.59` Code 0 and the input
desktop remains `Default`. The image is 720x1280; the taskbar rectangle is
`(0,1232)-(720,1280)`. Direct inspection confirms the bottom taskbar, Start
button, Wi-Fi, volume and clock. A foreground command window covers part of
the upper desktop in this control; this is not a clean full-desktop comparison.
Future controllers should launch the child hidden as above.

PNG SHA256:
`7599cbaaf68ea1123126f1ed0e5e488ebeea53677685c008f48eef33029b5d21`.
All eight returned files match the device lengths/hashes, and the control
task is removed. This verifies capture on `.59`, not corrected native output.

The user also supplied a 720x1280 historical screenshot with wallpaper/icons
but no taskbar, Start button or tray, identifying it as the earlier `.146`
capture. Its SHA256 is
`06091e339cb3d5f1be55d2df91a2b3e64dce46ad87f73ba15098bfeca10133c4`.
The missing taskbar is directly visible. The supplied file has no embedded
driver identity and is not byte-identical to a retained run PNG, so its `.146`
attribution remains the user's identification. Wallpaper/icons alone must not
be described as a complete desktop pass or used as a known-good taskbar baseline.

## Geometry control

`desktop-geometry154-20261112` validates the geometry extension on `.59` Code 0
at 00:50:13–00:50:21 UTC. All five returned files match device hashes. The image
is 720x1280, the work area is 720x1232 and the taskbar is `(0,1232)-(720,1280)`
at DPI 96. Its `TrayNotifyWnd` rectangle is `(462,1232)-(720,1280)`; the `Start`
child rectangle is `(184,1232)-(229,1280)`. Some legacy children report invisible
while the PNG contains their XAML-rendered content, illustrating why child-window
visibility alone is insufficient. Direct inspection confirms the full taskbar,
Start icons and right-side tray/clock. The geometry checkpoint predates image
completion. PNG SHA256:
`9b62c53aba9d87b7a82d47738993242e63ed2f271542df9fb65f9bf63480696d`.
This remains a recovered-baseline capture, not native presentation proof.

## Recovered shell with missing taskbar

After the sampler-view native trial, `.59` Code 0 and driver/registration
restoration are verified, but the user reports the taskbar missing. The
01:12:13 UTC PNG in `desktop-after-sampler-view154-20261112` independently
confirms wallpaper/icons with no taskbar. Its shell rectangle and work area
are still correct, at DPI 96. The Start child has zero extent and the
`Windows.UI.Composition.DesktopWindowContentBridge` child is absent. Scoped
module enumeration finds `D3D10Warp.dll` in both Explorer and DWM, with no
Tegra/Vulkan UMD in that list; no matching Application error/hang event is
returned for the current boot. This records the state, not its startup cause.

After retaining that evidence, only the captured Explorer owner is restarted
in the same interactive user session (PID 4360 to 2800). The 01:15:31 PNG
shows the complete taskbar, Start icons and tray/clock, also confirmed by the
user. The content bridge returns and Start/tray rectangles match the earlier
working control. Driver `.59`, 720x1280 resolution and DPI 96 remain unchanged.
This establishes recovery of the software shell, not native acceleration and
not the cause of earlier native misplacement. A restored driver and valid
outer taskbar rectangle alone are insufficient to declare the desktop restored.

All eleven returned files match device hashes. Before/after PNG SHA256 values:
`d98de373862ee54a5408a3edb39cd3e4b58a56c61ea9377090c2841dbd8e2394` and
`d16c0a27d86d456a98ca48afa11073a6a4af620616970cbf782936a2180f0cc3`.
The temporary Explorer-recovery task is removed.

## Shell interaction capture

The capture helper accepts an optional `-Shortcut Start`, `QuickSettings` or
`Dismiss`. The default `None` retains capture-only behavior. It sends a complete
Win-key, Win+A or Escape sequence on the interactive Default desktop, records
the inserted event count and foreground window, then captures the result.
It does not change a setting in the opened panel. The native 64-bit `INPUT`
layout is checked against SDK 10.0.26100.0. Held modifier keys cause rejection;
partial insertion releases the injected keys before reporting failure.
[SendInput contract](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-sendinput)

`shell-interactions158-control-20261112` completes on `.59` at 04:17:11 UTC on
November 12. All five captures preserve `.59` Code 0. Direct image inspection
confirms the opened Start menu, opened quick-settings panel and the complete
taskbar after dismissal. Start inserts two input events, quick settings four,
and each dismissal two. The foreground popup rectangles and owners are also
retained; an accepted keyboard sequence alone is not proof of visible content.
The temporary control task is removed and all returned files match device
hashes. This validates the interaction fixture on the software baseline;
the extended native `.158` interaction trial remains separate.
