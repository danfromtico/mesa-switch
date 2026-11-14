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
