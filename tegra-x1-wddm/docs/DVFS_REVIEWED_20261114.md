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
