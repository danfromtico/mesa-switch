# Native startup result — .130

2026-11-10, Switch Lite HDH-001, Windows 11 ARM64 23H2 (22631.2861).

The KMD startup and visible native scanout milestones pass. The complete
desktop repair does **not** pass: DWM cannot create its Direct3D device,
and interactive refresh stalls. Do not promote .130 as a usable desktop
driver or as hardware-accelerated DWM.

## Physical evidence

| Check | Observation |
| --- | --- |
| Device | Tegra X1 (GM20B), `ACPI\SWOA0F01\0`, .130 Code 0 |
| Identity | KMT and active VidPN source/target all `000000000008e7d4` |
| Mode | 720×1280, identity rotation |
| Execution | GDI fence 14, Reference 5, completion/CPU fence readback/retirement mask 7 |
| Display | Successful immediate MPO3; active bank latched owned scanout `fb9c7000` |
| Pixels | 37 bounded readback records, fences 31–326, matching LUID/address; exact RGB samples and a subsequent layout change |
| Panel | User confirmed that the RGB bars were visible |
| Stability limit | Code 0 and SSH remained available through the observation; smooth GDI/desktop refresh failed |
| DWM | 22 recorded exits with `8898008d` (`MILERR_NO_HARDWARE_DEVICE`) in the captured application-log interval |
| Direct3D control | Explicit GM20B adapter: `D3D11CreateDevice` → `887a0004`; WARP → success, feature level `b000` |

The native direct-GDI test did not complete its intended 60 frames. It
logged frames 0–2 before its task deadline, including a roughly 17-second
flush interval. The earlier PowerShell test also hit its deadline. Visible
bars and changing pixels must not be described as a smooth animation pass.

## Functional changes

Starting from .121, the changes implement fixed-panel standard shadow
allocations and uncompressed discard; consume the actual virtual-Present
allocation layout and advance both DMA pointers; encode bounded pitched
B0B5 copies with XRGB-to-ARGB alpha conversion; identify OS GDI submissions
by registered context ownership; and complete immediate MPO3 activation
using validated physical segment addresses and the actual display bank.

Normal application command submission remains gated. The 512 MiB budget
is unchanged. Test-only journaling and pixel capture live in the isolated
diagnostic package, not the functional driver source. The pixel probe only
reads owned scanout memory; it never paints the destination or writes a
completion fence.

## Validation and recovery

The .130 SYS/CAT signatures and all 12 catalog members were verified on
Windows before binding. The closed gate returned `c00000a3`; the sole
desktop grant was consumed to zero before hardware initialization. The
12-minute scheduled rollback stayed armed throughout the trial.

The current functional source builds from scratch, including the ARM64
display package and Windows fixtures, and its host models pass. The
diagnostic package's paging/context fixtures passed on the Switch with
controlled substitutes; those fixtures alone are not GPU evidence.
The independently rebuilt full-source paging/context fixtures also passed
on the Switch after rollback. The finalized .130 ETL export contains
124233 records, matching its header count and internal record lengths.

After rollback, SSH at 14:31:31 UTC confirmed .59 Code 0, the original SYS
and ACPI hashes, no start permit, and no remaining test tasks. All four
AC/DC display/sleep timeouts remained zero. Windows restarted directly
into Windows. Firmware was not modified; cold-boot behavior was not
retested in this round.

## Evidence locations

All downloaded evidence was checked against device SHA256 and length.

- `/private/tmp/t214-gpu-round/render130-live/verified-returned.json`
- `/private/tmp/t214-gpu-round/render130-live/returned/`: journal, ETL export,
  interactive identity probes, application log, Direct3D comparison,
  pixel records and recovery records.
- `/private/tmp/t214-gpu-round/render130-live/pixel-analysis.json`
- `/private/tmp/t214-gpu-round/render130-live/`: diagnostic sources, build
  commands, signing/staging records, bounded test scripts and model logs.
- `/private/tmp/t214-gpu-round/native-startup-final-build/`: independent
  full-source rebuild and Windows fixture log.
- `/private/tmp/t214-gpu-round/render129-full-etl/`: all 148480 events from
  .129, including the startup portion omitted by its first 600-second export.

| Artifact | SHA256 |
| --- | --- |
| .130 signed SYS | `2a5381079bab0553f09507d6f2a8fb1b539bad398f0c3673d3b5e7d53290bb85` |
| .130 pixel capture | `82d53cf7d365dde85099953147c57a784bcbfc0aa7de4be68055296ed58beebe` |
| .130 first ETL export | `114646441572f4c71b4c99457da2394b2f712d70c7cf8d9887f6317d687203ae` |

## Reference basis and remaining contract

The [contract investigation](SHADOW_PRESENT.md) records the first retained
failures, exact Windows producer ABI, Microsoft samples, NVK B0B5 encoding,
NVIDIA HBlank/VBlank activation rules and lfrb's WDDM2 presentation boundary.
The existing no-Direct3D DLL rejects device creation. Mesa's D3D10 frontend
also lacks usable shared-resource opening in its current GDI target, so
simply substituting that DLL is not a DWM implementation.

The remaining work is a real Direct3D/DWM rendering and resource-sharing
backend, followed by ordinary-window and continuous-frame validation on
the GM20B LUID. A display-only/software-composition mode would be a distinct
architecture and must be labelled as such; it is not silently substituted
for this full-WDDM/native-rendering goal.
