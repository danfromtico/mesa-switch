# T210B01 HighOPT and monitoring review

The Gemini GPU DVFS base is `97e885c5a2ed826236d0e86903ff903c48ef5083`
(cherry-picked as `d0d61648` in the isolated review tree). The follow-up corrects
partial PMIC-write recovery, extends HighOPT through all 16 upstream OPPs up to
1228.8 MHz, uses 76.8 MHz as the minimum, and supplies fresh counter telemetry.
Windows defaults remain 460.8 MHz / 800 mV. CPU policy and DFLL belong to
WindowsDrivers/WinX; this repository remains the owner of GPU source.

## Corrections to the original report

The original code implemented 76.8–844.8 MHz, not the report's claimed
153.6–921.6. Its thermal trips are 20/30/50/70/90 C; EMC1600's rail floor is
590 mV, not 640 mV. It uses DispatchGate (KMUTEX), a queue/burst heuristic and
fixed NAPLL settle plus a GPCCLK counter check, not the claimed FastMutex,
50 ms hardware-utilization governor or a ±5% PLL-lock criterion. There was no
implemented Speedo1600 fallback. Actual registry settings are GpuDvfsMode,
GpuDvfsMinKhz and GpuDvfsMaxKhz. The original report is not an acceptance record.

The captured Noble table is capped by its DT to 844800 kHz/800 mV. Full
GPUB01_NA_CVB_TABLE_HIOPT in kernel `2d0059fd3167a8df756de2aa0489d4aa70a9fc15`
adds 921.6/998.4/1075.2/1152/1228.8 MHz with a 900 mV characterization ceiling.
OC requests resolve to supported OPPs and never promise 2 GHz. All thermal
ranges are checked against the ceiling; unsupported characterization is rejected.

## Synchronization and live measurements

GPU transitions and periodic GPCCLK measurements require the worker's existing
quiescent ownership proof. Measurement temporarily overrides slowdown, so it
must not run over an in-flight channel. An unavailable telemetry counter does
not by itself become a new device-loss/clock transition. Actual M1 readback is
reported separately. New bridge opcode 0x108 and value 0x00020001 identify the
live-counter protocol; SwitchFan v15 does not refresh an old driver's cached
clock as if it were new hardware evidence.

A failed regulator write may already have changed the rail. Recovery reads back
M1 enable/voltage, updates cached rail state only when confirmed, and stops using
an unverifiable clock. The negative control using the old code fails the new
partial-write test; the corrected path passes.

## API markers

`src/util/u_graphics_api_marker.h` exposes Windows-local counts for successfully
created NVK devices (kind1) and Zink screens (kind2). The named mapping is
`Local\Mesa.GraphicsApi.v1.<PID>.<kind>`. It is optional read-only observation
for Pacecar; rendering and synchronization never depend on it. Destruction drops
the count and closes the mapping. The embedded vk/pipe base remains at offset0.
A live Zink screen is stronger evidence than merely loading Mesa opengl32.dll.
These markers describe active backend objects, not which API issued each draw.

Build checks cover KMD, NVK ICD, Zink D3D backend and native WGL libraries. The
bundle's binaries come from the isolated source base and are for review; merge
the patches onto the main thread's current display fixes before deployment.
Do not replace current tgd3d with the legacy Zink D3D build-check artifact.
Hardware DVFS, physical display, OC stability and present cadence still need
main-thread acceptance. Kernel Present-based FPS is not physical scanout proof.


## Main integration review (2026-11-14)

The main tgd3d-vulkan checkout imports only the three delivered GPU commits
(d0d61648, 8aa92bbd, b42df749) onto c65bcdce. Existing WDDM2.6 VidPN/shadow,
180-degree copy, physical scanout, shader cache and GPU bridge code is retained.
The NOTICE append conflict was resolved by retaining both deko3d and HighOPT
attributions. Old-display SYS/ICD files in review-bundle are not deployment inputs.

The private GPU PMIC bridge remains in this repository under integration/;
WindowsDrivers includes it and owns CPU DFLL and user-facing power policy.
The bridge reuses windows/pmic.h rather than maintaining another opcode/struct
copy. WindowsDrivers builds require MESA_TEGRA pointing to this directory.

Reviewed voltage-up/frequency-up and frequency-down/voltage-down ordering,
partial I2C-write reconciliation, DispatchGate/queue/engine lock order, the
CPU transaction lock and runtime telemetry expiry. The CPU table coefficients,
closed-loop limits and rounding were checked against Switchroot kernel
2d0059fd3167a8df756de2aa0489d4aa70a9fc15 clk-tegra124-dfll-fcpu.c, clk-dfll.c
and drivers/soc/tegra/cvb.c. The upstream CVB table does clamp at its closed-loop
ceiling; these are not open-loop PLLX voltages. Hardware transitions remain
unaccepted until the native trial.

Main-review corrections in WindowsDrivers: serialize the cooling snapshot's
DFLL monitor access; never leave Po callbacks registered when failed PLLX
restoration is followed by teardown; align the runtime version with Fan15;
expire stale GPU values in the power tray. ReleaseHardware cannot veto teardown
by returning an error. A failed PLLX restore is propagated, while cleanup does
not disable the unverified CPU source or lower M4. This is not proof of recovery
from every hardware failure.

Controls passed locally with ASan/UBSan for clock/DFLL/PMIC/charging and the
existing screen-off checks; ARM64 platform drivers and tray API controls also
passed. Initial native policy remains mode7, CPU1497.6/GPU460.8/RAM1600 MHz,
with existing display/logon startup and dual GPU bridge. No SC7 patch is included.
Full integration/build/native receipts live in OUT/dvfs-pacecar-integration.

## Native mode-selection failure and clock-parent correction

Fan15 with the integrated display package reached Code0 and DWM loaded the
current tgd3d/NVK pair. The first mode1 request failed with Win32 error21,
matching the user's tray dialog. No other dynamic mode was attempted; mode7
returned successfully with CPU1497600/GPU460800/RAM1600000 kHz. This is a
policy-initialization failure, not a completed DFLL transition or proof of a
GPU fault. The five-minute interaction was cancelled at the user's request.

WindowsDrivers ee3dc1e91d1b5522625451c1645fda59c1353b08 fixes two assumptions:
T210 PLLP_OUT0 is the VCO output, so BASE.DIVP must not divide its rate; and
WinX/Hekate I2C5 uses CLK_M selector6, whereas the initial adapter only accepted
selector0/PLLP. It preserves the shared I2C5 mux/divider. WinX 33a600c extends
the private DFLL handoff to version2 with the oscillator-derived CLK_M rate,
avoiding an out-of-resource CAR read in the Windows driver. Public power ABI2,
ACPI/display resources and the installed graphics binaries do not change.

The old adapter fails a control fixture with PLLP BASE.DIVP=1. Patched
CLK_M19.2MHz and PLLP/5 controls pass under ASan/UBSan, including existing
clock/PMIC recovery tests. These are MMIO models, not native DVFS acceptance.
The ordering open-loop DFLL -> CPU parent -> closed-loop DFLL matches the
pinned Switchroot clk-tegra210.c resume path; no speculative reordering was
introduced. Fan16 and the paired firmware were built, signed and staged with
an exact Fan15/firmware backup; native mode-switch results remain pending.

`tools/dvfs-integration/Update-ClockParent.ps1` changes only that paired platform
correction, selects the Fan driver with native GPU startup gated after a planned
restart, and then reactivates the existing display package. Its Restore action
returns the exact Fan15/firmware pair. Full original-platform recovery remains
available through Restore-Dvfs.ps1. The boot comparison tolerates the observed
8ms WMI boot-time jitter; it still requires a genuinely different boot.


Fan16 native mode1 still failed Win32 error21. Fan17 (WindowsDrivers
75180376caee1aaa2657fe2ca00881604cd2f3e6) additionally honors the fixed-PLLP
OVERRIDE=0 rule from pinned clk-pll.c. Native values were PLLP_BASE=0x48115408,
CLK_M=19200kHz, I2C5_SOURCE=0xc0000000 and I2C=19200kHz. The request now reaches
the later DFLL gate/ownership checks but still returns error21. Fixed7 returned
successfully and Code0 remained; no dynamic mode is accepted yet. Fan18 adds
one-shot rejection/readback traces without changing the gate decisions.

The user requested both applications at login. SwitchPowerTray's HKCU Run
entry remains enabled; PacecarSwitch config.start_with_windows and its quoted
HKCU Run path are enabled with the monitoring bar visible. After the planned
2026-11-14T21:57:58.5296220Z boot, both processes were observed in session1
(Pacecar5876, PowerTray5896). This confirms autostart, not FPS or physical-panel
acceptance. Charge-limit85 was retained; CPU/GPU/RAM fixed recovery remains
1497.6/460.8/1600MHz. Original-platform and Fan17 pre-update backups have been
downloaded and all 13+5 file hashes checked.


## Native policy-transition acceptance (Fan19, 2026-11-14)

WindowsDrivers b4057defa984c9c76f3428308f94180467295127 corrects the idle
handoff: CTRL=0, DVCO_RESET=1 and FORCE=0 permit disabling the retained
I2C_ENABLE bit and draining pending writes before acquisition. Active loops
and forced output remain rejected. Fan18's captured idle fixture fails the old
adapter and passes the correction with ASan/UBSan, including busy-I2C refusal.

On boot 2026-11-14T22:17:19.3200260Z, the test ran 22:22:15–22:23:05 UTC.
All modes 1,0,2,3,4,5,6 completed their short policy transition and individual
return to mode7. Each mode verified CPU DFLL active, no CPU error, live GPU
dynamic status, fresh clock samples, temperature bounds and unchanged RAM1600.
Every fixed return read CPU1497600kHz/1120mV, GPU460800kHz/800000uV and
DFLL/GPU dynamic disabled. DWM836 and Code0 remained through the test.

Actual short samples included CPU308191–1963500kHz and GPU76800–1228800kHz.
This does not validate every OPP, sustained OC stability, physical scanout,
landscape, FPS/API presentation or five-minute interaction. The five-minute
interaction was explicitly cancelled by the user and was not repeated.

Frozen components: KMD3ada55a42a3a116d41951a452893b2c09e3f01cc440e67ffd4b8b06b6d5c7918,
UMD8d7da8c1a3d9e9556a8e88b2b8bef67956dcf588c980d763e5f6684aaf6489c2,
ICD30a8d6a97d96d917b93ecb1f8a34489ce5613ca2082752b04c5823d6cb408e8d,
Fan19 SYS0579f19e75c3cc317506f8419c95fd0d1557be9bfdec680b4854e04fce1961ee,
firmware5ceb30735636ad4ca8be1428cce30629b0346e033d9799d89d6e167129093c3f.
ACPI14d2ca28b6b3d6e8afcef5200ba31c90010900783b78ed4cb52ffb1fdd6bc162
and Battery12 remain. Display code and the default GPU bridge were retained.

SwitchPowerTray4924 and Pacecar2752 were both observed in user session1 after
the same boot. Both HKCU Run entries remain enabled; Pacecar starts with its
bar visible. Tgd3dDefaultEnable is enabled with its existing login+5s trigger.
Final policy is fixed7; charge-limit85 is retained. Receipts and exact recovery
are in OUT/dvfs-pacecar-integration/dfll-idle-handoff; the original I pairing is
still available through the integration Restore-Dvfs.ps1.
