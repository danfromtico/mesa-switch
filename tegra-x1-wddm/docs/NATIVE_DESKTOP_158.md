# Native desktop observation with .158

On November 12, 2026, `stabilize158-native-20261112` completes the first
retained 90-second native desktop screenshot sequence with the full taskbar.
All four original PNGs show the bottom Start/pinned icons and right-side
network, audio, battery and clock area in their expected positions. The user
also reports that the physical desktop appears normal and comparatively stable
during this observation. This is a short desktop observation pass; sustained
stability and interaction coverage remain incomplete.

## Exact components

| Component | Source or SHA256 |
| --- | --- |
| KMD source | `6c854e9f394c3d761fab85885bead4bbd4588581`, `stabilize/native-desktop-153` |
| Signed KMD `.158` | `46d1757674ff156028ce5b729ba53671981ffab09948fcaf125751c46bb10088` |
| D3D UMD | `77dea046dde0777d6c67e62769c983c5249595a9cc9e3dd7f8323805854f9cdf` |
| NVK ICD | `6e2fad99d742a69033cc59925093fb4a7c8489728a3f7e5e9ebb523c017e0323` |

The candidate starts from the selected `.153` rendering baseline. It includes
the sampler-view operand correction, SAMPLE_I lowering, execution-ordered
inherited-state validation, and the GR helper-invocation load correction with
write-readback verification. It excludes the `.155–.156` additional diagnostic
instrumentation, the remote SM exception-policy change and unfinished worker
timer changes. Its Mesa `src/` matches `ee7b0d50`, retaining the previously
verified UMD and its twelve assertion-enabled translator checks.

Host regressions, the actual NETB initialization model and the WDK ARM64 build
pass. The Windows ARM64 context fixture exits 0 at 03:33:15 UTC while `.59`
remains Code 0; executable SHA256 is
`3f12dc372d0150ae333a6c4e2ec3cfc713492b7de8998c5cf6cc4d400f0607c0`.
Embedded signatures and all 13 catalog members pass verification. These
controls precede the native observation; they are not hardware results.

## Desktop and presentation evidence

Native startup records Code 0, hardware status 0 and a consumed one-shot
permit at 03:42:23 UTC. The observation uses Tegra LUID `00000000000bc22f`,
720x1280 portrait and DPI 96. The native task exits 0 at 03:44:19 after captures
scheduled at 0, 15, 45 and 90 seconds. All captures record `.158`/Code 0 before
and after, with the same Explorer PID 4472 and a visible desktop composition
content bridge. The taskbar bounds remain `(0,1232)-(720,1280)`.

| Capture | PNG SHA256 |
| --- | --- |
| 0 seconds | `1bcad00c93a91fbc9e5c4cdc16fb3c2d720bac8ec6fa548a7220369b606ee38e` |
| 15 seconds | `156784932e878d8c62db53bdebf3d4fb118777da2d534f800bd47000dac04715` |
| 45 seconds | `156784932e878d8c62db53bdebf3d4fb118777da2d534f800bd47000dac04715` |
| 90 seconds | `b5eaea08246ac592e84172729c25c0b6825c0106eea824ba47b4853f7f5996f0` |

The finalized ETL contains 266,522 exported records, including 23,802 UMD
events. The exporter reports `invalid=0`, `events_lost=0`, `buffers_lost=0`, a
nonzero EndTime and 1,161 written buffers. Its binary export SHA256 is
`a8ee812eeed8aaad1e240dcfa872535d84b3a3a3e37a97913ecbd1288236b679`.

Dwm-Core's hardware-device event selects **Tegra X1 (GM20B)** at LUID
`bc22f`, Feature Level **10_0**. The same DWM PID 768 records successful Zink
initialization for that LUID and **79 successful flip Presents** across
**100.5441046 seconds** of ETW time. All 79 native MPO requests use one full
720x1280 plane; every flip address appears in a subsequent native VSync before
the next flip. The trace retains 8,128 native multi-plane VSync records.

No UMD RuntimeError/ZinkFailure or KMD recovery record is retained. The two
`VidSchMarkDeviceAsError` records, with numeric reasons 14 and 6, both precede
native `DdiStartDevice`; their reason values remain uninterpreted. There is no
such record after native start in this trace. One known Dwm-Core event 203
payload/template mismatch excludes 1,297 events from the decoded analysis;
their contents are not inferred.

## Recovery and scope

Collection completes normally at 03:44:50, followed by the scheduled `.59`
recovery. The only boots in this trial are the setup boot at 03:39:55 and the
planned recovery boot at 03:46:07. At 03:48:56, `.59` Code 0, original ACPI,
temporary-registration/task cleanup and unchanged power settings are verified.
Saved portrait verification is a no-op at 03:49:33. The restored desktop PNG
at 03:51:05 also has a complete taskbar and tray, SHA256
`e748bc62ca950626e0c0c8695b2962361fa468e84c5a116e71dab02fc837ff3c`.

All **184 files / 100,622,505 bytes** in the returned trial evidence match
device-side hashes and lengths. Native images were fetched early and match
the subsequently collected originals. Evidence is retained in the external
build root under `stabilize158-native-20261112` and
`desktop-after-stabilize158-20261112`; `screenshot-summary.json` and
`presentation-summary.json` separate image inspection from ETW analysis.

The first visible improvement does not isolate a single cause: this candidate
combines several independently justified fixes. It does not establish the
Start-menu popup's behavior, interactive stress, long-duration stability,
hardware landscape, Win32 vkcube, full Vulkan conformance or game compatibility.
Main retains later diagnostic source for future use; it is not the exact `.158`
build tree pinned above. The later attempts below supersede the instruction to
repeat the frozen set before further source work.

## Later attempts

`stabilize158-extended-native-20261112` plans 300 seconds of desktop/menu
observation followed by a separate 32-MiB VidMm query. It retains twelve capture
records through 195 seconds, then returns failure after the 210-second capture
times out. The budget probe is not reached. Its archived recovery record confirms
`.59` Code 0, no remaining trial tasks and unchanged power at 05:00:29 UTC.
These records do not establish a completed sustained-interaction pass.

`native-vkcube158-20261112` uses the exact KMD, UMD and ICD hashes listed above.
It plans unmodified Khronos `vkcube` at 256x256, FIFO and 360 frames, desktop
captures, then the separate 32-MiB VidMm probe. The executable is the previously
built Vulkan-Tools `1d3fbab1ff5abce9f3eb9f3e7fcf66f455ae4475` sample, SHA256
`69ca28a36d20da41b6643573e0fe28527109e7d03454000696aa31002402ca38`.
The trial preserves portrait and requests no hardware rotation.

The initial setup boot is 07:03:03 UTC on November 12. Native startup records
Code 0, hardware status 0 and a consumed permit at 07:05:29. There is no retained
`mode-stage.json`, `cube-window.json`, `cube-result.json`, `native-result.json`,
native PNG or memory-probe result. Consequently even cube execution is
unproven. The user reports two automatic restarts; no cause is assigned or
investigated. The repeat-start guard records recovery at 07:08:43, restoration
is staged at 07:09:23, and the recovered OS boot is 07:09:57.

At 07:14:04 UTC, `.59` Code 0, the original ACPI hash, removed test permit,
temporary ICD/UMD registrations, loader/debug environment and task cleanup,
and unchanged display/sleep settings are verified. Saved 720x1280 portrait is
unchanged at 07:14:19. The recovery PNG at 07:15:22 shows the complete taskbar
and tray, SHA256
`9d24d4255750c0202cb6fbd9a51367ab5569d7f120ad5992970c8aef25137c30`.
This is the recovered software desktop, not a native test image.

All **137 returned files / 113,708,587 bytes** match device hashes and lengths.
The records are retained under the main checkout's
`build/native-vkcube158-20261112/evidence/`; the host-side user observation and
summary are alongside it. No native test remains active. Continue source work
and non-device checks; hardware **1280x720 landscape** still needs its own
asymmetric-image, dimensions/pitch, latch and physical-output verification
before another cube acceptance run.

## Five-minute observation and physical corruption

`stability158-five-minute-20261112` uses the exact frozen components above.
Native startup is Code 0 at 07:25:17 UTC on November 12, LUID `a79e7`.
Observation begins at 07:25:35 and finishes at 07:30:47 with exit 0 and
311.0569514 observed seconds. All six scheduled captures (0, 60, 120, 180,
240 and 300 seconds) record `Shortcut=None`; this script sends no shortcut
or click inputs. The timed window retains no intervening reboot.

The user supplies four physical photographs and reports tearing, stalls and
heat. Two photographs clearly show large white polygons covering Quick
Settings. **Visual acceptance fails despite the completed timed observation.**
Neither the photographs nor the timer result isolate a stride, shader,
translation or synchronization defect. Do not assign the cause to Zink or
stride without evidence from the responsible path.

The planned recovery boot is at 07:32:34, after the observation completes.
At 07:35:54, `.59` Code 0, original ACPI, test-registration/task cleanup and
unchanged power settings are verified. Portrait remains unchanged at 07:36:12.
The recovered desktop PNG at 07:37:04 has SHA256
`09833d9a0af32e410a1f6da7f79d767ef0ac9d33c1f2954c365d6f97cf8ab620`.
It is recovery evidence, not an additional native result.

All **196 files / 112,332,491 bytes** match the device-side manifest. The
unchanged physical-photo originals and their hashes are retained separately
under `build/stability158-five-minute-20261112/physical-photos/`; collection
and observation records are alongside `evidence/verified.json`. Hardware
landscape and vkcube have not been advanced past this failed visual gate.

The 300-second system PNG also contains white triangular corruption in the
Quick Settings Bluetooth tile, SHA256
`51ae7ecb7fc66edf036403d0a319cf5cfe6dde34ca15e679016e52781d68ab3a`.
The defect is therefore observable in the captured desktop image as well as
on the physical screen. This does not isolate its rendering/shared-storage/
synchronization cause, and does not support treating it solely as a final
display-controller stride problem.

Normal-presentation analysis covers a retained **243.6166229-second slice**
of the ETW export, not the entire 311-second observation. DWM PID 440 has
796 DXGI Present entries, all `SyncInterval=1`, and 795 successful returns.
All 795 native flips use interval 1. The single adapter's 14,606 VSync records
have spacing p50/p95/p99 of 16.6664/16.6854/16.7083 ms. These records do not
show an absent VSync limit. Excluding one ambiguous overlapping entry, the
794 paired DXGI Present calls take 25.084/130.6247/265.581 ms at p50/p95/p99
in wall-clock time. This is not CPU
execution time, GPU time or full-frame latency: future performance comparison
must separate rendering, fence waits and presentation queuing. Counting these
mixed idle/interactive Presents is not a throughput benchmark. The decoded
export and `presentation-pacing-summary.json` remain under the trial directory;
the known Dwm-Core event-203 payload mismatch excludes 1,803 records
from decoded analysis.
