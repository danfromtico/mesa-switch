# Build and porting guide

## Driver and tests

Run the commands from the Mesa checkout root. Use Python 3, LLVM with
`clang-cl`, `llvm-lib` and `lld-link`, plus the versioned Windows SDK and WDK
Include trees. The WDK must also have its
ARM64 Lib and KMDF 1.15 directories. Set `WDK_INCLUDE` and `SDK_INCLUDE` to
the versioned Include directories. The Windows 11 22H2/23H2 build uses
WDDM 2.4 private-memory callbacks.

```sh
python3 tegra-x1-wddm/tools/build.py \
  --wdk-include "$WDK_INCLUDE" --sdk-include "$SDK_INCLUDE" --out build/kmd
```

The base build runs host fixtures and emits `TegraGpuCore.lib`. Add
`--windows-tests` to build the native ARM64 WDK fixtures. For a hardware
package, add `--render` for the MCDM path or `--display` for the WDDM panel
path. For the KMDF diagnostic and optional PMIC bridge, add `--probe`.
`--probe --keep-fan` reuses an installed compatible bridge.

The hardware packages require `--gr-firmware "$GR_FIRMWARE"` with the
external Tegra21x `NETB_img.bin`, `fecs.bin` and `gpccs.bin`, and
`--acr-firmware "$ACR_FIRMWARE"` with the NVIDIA 32.7.6 GM20B
`nv_acr_ucode_prod.bin`, `pmu_bl.bin`, `gpmu_ucode_image.bin`,
`gpmu_ucode_desc.bin`, `pmu_sig.bin` and `fecs_sig.bin`. Supply these
locally. Generate and sign the catalog and package using the ARM64 WDK and
a signing certificate trusted by the test Windows installation. Match the
firmware, miniport, and ICD ABI when deploying. [Driver details](DRIVER.md)

For a desktop-gated native submission experiment, add
`--display --native-test-version 146`. This builds the entry from tracked
source, enables native submission after the startup gate, and sets the INF
to `0.1.0.146` with the UTC build date. Regenerate and sign its catalog; do
not reuse a catalog for different bytes. Ordinary builds retain paging-only
bring-up. This option is an experimental deployment contract, not automatic
installation or a production desktop claim.

The controller must first establish a working interactive desktop and a
scheduled recovery, then create the volatile service subkey
`tegragpurender\AllowDesktopStart` and arm the first little-endian ULONG of
`%SystemRoot%\System32\drivers\TgGpuBootTest146.bin` to 1. The file must be
exactly 4096 bytes. Before opening hardware, the driver consumes that value
and flushes zero to disk. A missing grant, spent permit or failed I/O returns
a closed startup gate; `BootPermitStatus` preserves the actual NTSTATUS.
The controller removes the volatile grant after the attempt and restores the
previous driver/table/environment after collecting the result. The new file
is only a one-shot permit: it does not implement the old temporary wrapper's
4 KiB business journal. `test-native-start.exe` verifies this gate using
controlled registry/file substitutes; it does not start the GPU.

## Mesa NVK ICD and triangle

Use Meson, Ninja, Mesa's build Python dependencies, LLVM-MinGW for ARM64,
Rust with `aarch64-pc-windows-gnullvm`, and bindgen. Set the Meson cross
file's host machine to Windows/aarch64, point the C, C++, and Rust linkers
at the LLVM-MinGW toolchain, and set `needs_exe_wrapper = true`.
Bindgen needs the MinGW target, its sysroot and the target clang resource
directory. Build `mesa_clc` and `vtn_bindgen2` for the build host from the
pinned Mesa tree and put them on PATH.

```sh
python3 src/nouveau/vulkan/nvkmd/windows/build_icd.py \
  --out build/icd --cross-file "$WINDOWS_CROSS_FILE" \
  --cc "$ARM64_MINGW_CC" --clang-cl "$CLANG_CL" \
  --sdk-include "$SDK_INCLUDE" --wdk-include "$WDK_INCLUDE"
```

The script builds the checked-out Mesa source and produces a matched
`libvulkan_nouveau.dll` and `nvk_windows_icd.json`. The ICD requires the
matching live adapter LUID and the KMD's `NATIVE_SUBMIT` capability.
NVK enumerates a native-ready GM20B by default.
Use `test-kmt.exe` for the controlled KMT cases and `nvk-windows-check.exe`
for live ICD enumeration.
`test-kmt-shared.exe` exercises the SDK-backed shared-resource lifecycle
with controlled KMT substitutes, including repeated opens, budget limits
and cleanup after residency failure.
`nvk-windows-shared-check.exe <16-digit-hex-KMT-LUID>` checks live shared
storage on the selected GM20B adapter. It maps the allocation on two KMT
devices, closes the producer and exported NT handle, then checks all 4,096
words through the importer's CPU mapping. This is a memory/lifetime test;
it does not submit GPU drawing work or exercise DWM.

`tegra-x1-wddm/tools/build_offscreen_triangle.py` builds the offscreen smoke test with
Khronos Vulkan headers and the ARM64 loader and import library. Run
`vk-triangle.exe <absolute-NVK-DLL-path> <16-digit-hex-KMT-LUID>`. The sample
loads that ICD through `VK_LUNARG_direct_driver_loading`. Optional
`--preview` opens a Win32 window after the verified readback. The command
validation in the current KMD covers the tested submission stream.
[Hardware status](STATUS.md)

### Win32 vkcube

Build the unmodified Khronos Vulkan-Tools C sample with the ARM64 compiler
and the Vulkan-Headers revision listed in that checkout's `known_good.json`:

```sh
python3 tegra-x1-wddm/tools/build_vkcube.py \
  --source "$VULKAN_TOOLS" --headers "$VULKAN_HEADERS/include" \
  --cc "$ARM64_MINGW_CC" --out build/vkcube
```

The tool records the upstream commit, shader/source hashes, compile command
and executable hash. Upstream retains its own license; its renderer and
embedded SPIR-V are not copied into or modified by this project.
Place the ARM64 `vulkan-1.dll`, NVK ICD and `nvk_windows_icd.json` beside the
executable. In an interactive Windows session, after checking that the
native-ready KMT LUID, NVK LUID and active display LUID match, select that
single ICD and run a bounded FIFO presentation test. In the 2026-11-11
device trial, the process environment alone did not make the loader discover
the ICD; the trial needed a temporary per-adapter `VulkanDriverName`
registration, saved and removed by its rollback script:

```bat
set VK_DRIVER_FILES=%CD%\nvk_windows_icd.json
vkcube.exe --suppress_popups --gpu_number 0 --present_mode 2 --width 256 --height 256 --c 120
```

The current Win32 WSI path renders with NVK, waits for the GPU copy fence,
then presents through GDI. A passing cube is evidence for native Vulkan
rendering and this windowed presentation path; it does not establish
hardware DWM composition, direct DXGI presentation or Vulkan conformance.

The first device trial with Vulkan-Tools commit
`1d3fbab1ff5abce9f3eb9f3e7fcf66f455ae4475` and Vulkan-Headers `v1.4.368`
runs from 01:07:14 to 01:07:35 UTC on 2026-11-11. The reused .144 miniport,
NVK and active display agree on GM20B LUID `91fd4`; `vkcube --c 120` selects
Win32/FIFO and exits 0. Both captured client images are black, and the user's
panel photo confirms missing content. **Visible cube presentation fails.**
The native path also changes rotation from 90 degrees to identity; its
VidPN and MPO contracts currently implement only the native 720x1280,
2880-byte-row, identity scanout. This identifies a missing rotation feature,
but does not isolate the cause of the missing image. Separate GPU image
readback, GDI/window content and scanout evidence is needed next.

Tested executable SHA-256:
`69ca28a36d20da41b6643573e0fe28527109e7d03454000696aa31002402ca38`.
Tested ICD SHA-256:
`2a08e044c70d77d111f3d88fa6b59e39c3e20eee7d5255da4109e48ce81bed8d`.
Artifacts and build provenance are retained under
`/Users/rujian/.local/share/mesa-switch-woa-build/runs/win32-cube-20261111/`.
The 01:15:27 UTC recovery check confirms .59 Code 0, original driver/ACPI
hashes, removed temporary registration/start permit/tasks and byte-identical
before/after power settings. The subsequent interactive display query
reports 90-degree rotation; the later user correction below supersedes
the claim that this restored their preferred orientation. All 40 downloaded records
(24,472,682 bytes) match device-side lengths and SHA-256 values. The native
cube result JSON has SHA-256
`7895ce876630b876c8d14b3d82250e5204b3df6a377dea369e90439a46583003`.

The subsequent WSI change releases every window `GetDC` with `ReleaseDC`
on the acquiring thread, rather than retaining a DC across swapchain calls
and passing it to `DeleteDC`. It also flushes the thread's GDI batch before
making the DIB available for another present/destruction, checks DIB setup
failures and uses the bitmap header's actual size. Memory DCs continue to
use `DeleteDC`. These fix GDI lifetime/synchronization contracts; they have
not resolved visible desktop presentation.
[ReleaseDC contract](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-releasedc)
[GDI batching contract](https://learn.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-gdiflush)

`MESA_VK_WSI_DEBUG=win32` writes per-present pixel summaries and GDI results
to stderr, including in release GUI processes with redirected output.
On 2026-11-11, 01:39:56–01:40:01 UTC, the unchanged upstream cube executes
24 presents on matching GM20B/KMT/display LUID `90f83`, exits 0 and does not
time out. The 256x256 buffers have 1024-byte GPU/DIB row pitches, 24 distinct
pixel hashes and 64,251–65,536 nonzero RGB pixels per frame. All 24 sampled
window centers match the corresponding readback RGB values after COLORREF
channel conversion; every `StretchBlt` and `GdiFlush` succeeds. The center
samples are grayscale and do not validate the entire image or channel order.
The desktop capture remains black. Verify its DPI/coordinate mapping and
the composition/scanout path; these observations alone do not prove which
later stage fails, or establish a visible cube.

Tested ICD SHA-256:
`0bb7125a1b96ec8c16b0b7c5e84f9e9bd743819dc54a827ef1d7f73d82712097`.
Result JSON SHA-256:
`ba3b1b2b82f94894d4e30fe22dcbf5c382bca7bc4f661f3e482a3e2f0c1923fb`.
The 33 native records (24,506,536 bytes) match device-side lengths/hashes;
the transfer resumed after the planned rollback restart. The 01:46:19 UTC
check confirms .59 Code 0, original driver/ACPI hashes, removed temporary
registration/permit/tasks and unchanged power settings. The subsequent
interactive query reports 90-degree rotation; the preference correction
below also applies to this trial. Artifacts and
the tested source patch are retained under
`/Users/rujian/.local/share/mesa-switch-woa-build/runs/wsi-gdi-20261111/`.

**Correction after the user's manual adjustment:** the 90-degree query
above recorded the post-test state, not a verified restoration of the
user's portrait preference. The old rollback restored driver/ACPI state
but did not save/apply the full desktop mode. The new baseline captured
at 02:01:58 UTC is 720x1280, DEVMODE orientation 0 and CCD identity rotation,
with matching current/registry modes. Use this user-selected baseline.

`tools/display_state.cpp` captures a versioned single-display snapshot
including monitor identity, current/registry DEVMODE and CCD source/target
data. `show` reports both desktop and signal dimensions; `visible` checks
that the process uses the active console/input desktop. `wake` is a no-op
on the active desktop; on `Screen-saver` it attempts one ordinary touch
sequence and then checks whether the input desktop actually changed. An
injection failure is reported without changing desktop permissions or
screen-saver settings. It does not authenticate a locked session. `save <file>`
creates a snapshot without overwriting an existing one; `verify <file>`
compares the current mode. `test <file>` uses `SetDisplayConfig` validation
without applying a mode, and `restore <file>` rebinds the saved modes to the
current adapter IDs before validating/applying only a changed mode. It
requires the same monitor and an active input desktop for a mode change.
Run it from the logged-in interactive session, after restoring the baseline
driver. The snapshot format is local Windows data, not an ICD/KMD ABI.

```sh
"$ARM64_MINGW_CXX" -std=c++17 -O2 -static -municode -D_WIN32_WINNT=0x0A00 \
  tegra-x1-wddm/tools/display_state.cpp -luser32 -lgdi32 -o display-state.exe
```

For a rotation trial, `rotation-test <degrees>` validates the requested mode
without applying it. `rotation <degrees>` applies it temporarily; only
`rotation-save <degrees>` persists it. Accepted angles are 0/90/180/270 in
the [CCD clockwise convention](https://learn.microsoft.com/en-us/windows/win32/api/wingdi/ne-wingdi-displayconfig_rotation).
For this fixed-panel KMD, the helper keeps the native CCD source extent and
checks the separately oriented DEVMODE desktop extent. It first validates
strictly; error 1610 permits a second validation with `SDC_ALLOW_CHANGES`.
The same flags are used for apply, followed by checks of source size, position,
format, desktop size, rotation and the complete target timing. JSON records
both the strict result and the adjustment flags. These commands require an
active input desktop. API acceptance does not verify the physical scanout.
[Validation, temporary apply and persistence flags](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setdisplayconfig)

The added rotation commands compile for Windows ARM64 with warnings treated as
errors. On restored `.59`/BasicDisplay, identity validation succeeds and 90-degree
validation returns `ERROR_GEN_FAILURE` (31); neither applies a mode. A subsequent
snapshot verification confirms unchanged portrait at 16:44:24 UTC on 2026-11-11.
All returned control files match device hashes. This does not establish whether
the native driver accepts that configuration; physical application remains
pending. The `.148` portrait trial used the previous, hash-verified helper.
Keep the saved baseline snapshot for recovery and persist the final landscape
mode only after the requested physical orientation is verified.

`tools/rotation_pattern.cpp` draws four distinct corner colors, numbered
direction labels, an arrow and a one-pixel border through GDI/DWM. It does not
change display settings or rotate a bitmap. Run `rotation-pattern.exe 60
ready.json` in the interactive session; the window closes after 60 seconds,
or with Escape. Its ready/output records describe a logical desktop window,
not physical acceptance. Capture comparison and physical orientation/cursor
checks must be recorded separately.

```sh
"$ARM64_MINGW_CXX" -std=c++17 -O2 -static -municode \
  -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 \
  tegra-x1-wddm/tools/rotation_pattern.cpp -luser32 -lgdi32 -o rotation-pattern.exe
```

Before a native trial, save `display-state.exe save user-display.bin` from
the user's preferred baseline. Check `display-state.exe visible` before
launching and while capturing visible output. After driver/ACPI rollback
and interactive logon, run `display-state.exe restore user-display.bin`,
then `verify user-display.bin`. Keep and hash the snapshot with the trial's
records. A refused mode change or failed verification is not a restored
display preference, even when the driver's Code 0 and power settings match.

When a screen saver is active, first attempt an ordinary touch wake from
the interactive session, then recheck the actual input desktop. Do this
before asking the user to dismiss it manually; request user intervention
only if the attempt fails or Windows requires a PIN/password. An input API
success alone does not establish that the desktop is visible. Keep the
existing screen-saver, display timeout and sleep settings. The user need
not keep the screen lit during local development; only the bounded visible
display trial requires the active desktop. This is the user's requested
test procedure, not a claim that remote touch dismissal has already passed.

The initial legacy ChangeDisplaySettings test failed without changing the
mode. CCD validation returned access denied while the process desktop was
`Default` and the input desktop was `Screen-saver`, including under the
elevated task. Ordinary mouse wake input was also denied; no input was
delivered. After the user returned to `Default`, the same CCD configuration
passed validation at 02:15 UTC, with unchanged portrait settings. Actual
application across a later driver rollback still needs its own verification.
The final helper also passes `visible`, `verify` and non-applying `test`
at 02:19:02 UTC on the same portrait baseline. Its ARM64 build passes with
`-Wall -Wextra -Werror`; executable SHA-256 is
`bd742cbfe7c9f3981887c8e70538fa7ee099114ef23533d85648228c598b6e79`.
Do not infer that the earlier cube captures necessarily had this desktop
state: it was observed later. Require an active-desktop check before visible
GPU tests and record the input desktop with captures.
[CCD validation/apply contract](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setdisplayconfig)

The user preference snapshot and validation records are retained under
`/Users/rujian/.local/share/mesa-switch-woa-build/runs/display-preference-20261111/`.
All 58 downloaded records (506,317 bytes) match device-side lengths and
SHA-256 values. The 1,192-byte user snapshot has SHA-256
`af56a3943261198dac51fe1631d33c7481b641171f9ed14fd79298dd39e93d48`.

**2026-11-14 supersedes the DC-rotation experiment below:** the user selected
tgd3d GPU prerotation into an ordinary 720x1280 portrait primary. DC column
scan and decrement transforms have been removed. The logical desktop target
remains 1280x720. CPU controls and ARM64 builds pass; physical output has not
been tested. See `TGD3D_COLD_BOOT_HANDOFF_20261113.md` for the current path,
frozen build provenance and Noble comparison. The following paragraphs
describe the earlier hardware-rotation experiment, not the active plan.

The final orientation requirement was **hardware rotation and a 1280x720
landscape desktop**, with the controls in the normal left/right handheld
position. Leave Windows in that landscape mode after validation. Until the
hardware path passes, the saved portrait snapshot remains the experiment's
recovery target. Update source dimensions, row pitch, allocation description,
VidPN/MPO rotation and actual scanout transformation together. The identity
metadata correction is not a rotation implementation. Check an asymmetric
corner pattern, full-image contents and sustained visible output before
claiming the complete on-screen result.

The Tegra210-specific L4T table declares column scan and packed-format
rotation for Display A Window A. Its window programming swaps the prescaled
axes and DDA inputs for column scan, retaining the output raster dimensions;
this is more than changing a Windows orientation flag.
[NVIDIA T210 feature table](https://github.com/OE4T/linux-tegra-4.9/blob/oe4t-patches-l4t-r32.7.4/nvidia/drivers/video/tegra/dc/dc_config.c)
[NVIDIA window programming](https://github.com/OE4T/linux-tegra-4.9/blob/oe4t-patches-l4t-r32.7.4/nvidia/drivers/video/tegra/dc/window.c)
Hekate also has a Switch-specific block-linear column-scan configuration.
Its stride, block height and offsets belong to that framebuffer layout and
must not be transplanted onto our current pitch-linear allocation.
[Hekate display configuration](https://github.com/CTCaer/hekate/blob/master/bdk/display/di.inl)
These GPL sources are hardware references, not code copied into the MIT
driver. The outstanding implementation must select a supported layout,
retain backing until the active bank confirms the complete geometry/address
change, and restore the original layout on driver release.

### Present contract audit

The 2026-11-11 follow-up separates DWM initialization, UMD presentation and
the Display A latch. No driver, mode, power setting or rotation register was
changed during this audit; the device remains on .59 Code 0.

The frontend enables neither D3D10.1 nor D3D11 DDIs in `State.h`, and
`CreateDevice` accepts only the D3D10 interface versions. The prior runtime
regression explicitly requests FL10_0, so its output alone is not a maximum
capability test. The new `nvk_d3d_runtime_check.exe --luid <luid>
--feature-levels` checks individual 11_1/11_0/10_1/10_0 requests and their
ordered list without changing the driver's claims. The ARM64 build and
`--warp --feature-levels` control pass on Windows: all four explicit requests
succeed, and the combined list selects 11_1 on Microsoft 1414:008c, LUID
`5427`. The native matrix follows in the trace-work result below. These are device-creation results,
not DWM or feature-conformance results.

The Application-log interval 01:25–01:44 UTC contains 35 `Dwminit` exits
attributed to Tegra X1 with `0x8898008d` (`MILERR_NO_HARDWARE_DEVICE`), plus
one separately attributed to BasicDisplay. On .59, the system has no
`System32/vulkan-1.dll`; the tested loader copy is in the .144 application folder.
The NVK opt-in is absent from machine/user settings and the SSH environment.
A separate SYSTEM task in a directory without a loader confirms that
`LoadLibraryW(L"vulkan-1.dll")` fails with error 126, while loading the
explicit .144 app-local path succeeds. Its process also lacks the opt-in.
This control is not the DWM process, but identifies a deployment prerequisite:
Zink uses name-only loader discovery, and the nonconformant GM20B NVK path
requires the opt-in. Neither the app-local tests nor their environment proves
that DWM can create the same device. Trace DWM's actual initialization result
after supplying its loader/ICD/test environment.

Independent functional gaps remain: hardware `_Present` returns
`DXGI_ERROR_UNSUPPORTED`, `_Blt` currently returns success without a copy,
and the runtime shared-image bridge is unfinished. Raising the advertised
Feature Level does not implement these contracts.

The retained `cube-pixels-contract.bin` (SHA-256
`6fb44bb06efec7d220ec6e0494230f78c2155822dda61ae17f49206eaaa01c13`)
contains fixed slot 46 at offset `ee0`, tag 68: success, scanout segment base
and cached `Displayed.Address` both `fc9a6000`, IRQL 2. The retrieved .144
SYS matches `e9cc6c5e576afd6861047983567eb2b4628209a5f227cdadc3ef48e86d59144c`.
Binary/source comparison verifies the record producer at RVA `2be4` and
the Displayed update at `1e370`: it reads through `tg_t214_dc_latched_scanout`
at `1cea0`, checks Window A's pending activation, selects the active register
bank, reads address/visibility and restores the selector. Initial POST
state uses a different address. This record supports a completed initial
latch; it is a retained cached state, not a live register sample concurrent
with every cube frame. Full-image contents and continuous updates still
require correlated capture.

Switchroot's NVIDIA display source at
`3c40adf1081096bc61a7c1280434c163e7e79b48` supplies a useful hardware
comparison: `window.c` programs the surface address and stride, transfers
assembly state to the armed bank, requests activation, and checks completion
for immediate flips; `dc.c` distinguishes active and assembly register
views. This supports the existing latch checks but does not replace WDDM
resource sharing, D3D UMD callbacks or DWM device creation. The reference is
GPL-2.0; no implementation was copied into the MIT driver.
[Switchroot display source](https://gitlab.com/switchroot/kernel/l4t-kernel-nvidia/-/blob/3c40adf1081096bc61a7c1280434c163e7e79b48/drivers/video/tegra/dc/window.c)

Probe builds, downloaded Windows records, the signed binary comparison and
the pinned reference manifest are retained under
`/Users/rujian/.local/share/mesa-switch-woa-build/runs/present-contract-20261111/`
and `references/switchroot-display/`. The next native trial must distinguish
loader/device negotiation from presentation and preserve the user's saved
portrait baseline. Rotation capability remains planned; ad hoc rotation
register writes are not part of this audit.
All 11 returned Windows records (526,873 bytes) match device lengths and
SHA-256 values. The feature probe executable has SHA-256
`9081c87021f358bcb52f5454f1bd32e7dc7bc49f5434bdd7ee80db53499a375c`.

### DWM initialization trace work

The UMD now exposes an opt-in ETW provider for initialization and present
contracts. Its release-build records include the process image, HRESULT,
interface/capability arguments and selected LUID. `ZinkBegin` additionally
records whether the process inherited the exact NVK test opt-in. The
provider unregisters before DLL unload. The exporter validates its fixed
versioned payload and reports the ETL header's end time, buffers written
and event/buffer loss counters; missing events in an unfinished or lossy
trace cannot establish that a callback never ran.
[Provider and collection commands](../../src/gallium/targets/d3d10umd/README.md)

The full ARM64 build passed, and the existing adapter, DLL-entry and device
fixtures passed on .59 at 03:15:15 UTC. The controlled ETL decodes 27 UMD
events with zero malformed records, including a successful device result
and the expected denied adapter query. The initial attempt used unavailable
`logman.exe`; the completed capture uses Windows' installed
`New-EtwTraceSession`/`Add-EtwTraceProvider` cmdlets. No GPU rendering or DWM
acceptance is claimed for these controlled callbacks. The exporter was
then corrected to use `LogfileHeader.EventsLost`, rather than the reserved
`EVENT_TRACE_LOGFILE.EventsLost`. Re-export of the retained ETL confirms 27
valid events, zero event/buffer loss, nonzero end time and two written buffers.

The native package reuses the signed .144 KMD and tested WSI ICD, changing
the UMD and its process environment. Staging installs the known ARM64
loader temporarily in System32 and saves/restores the machine NVK opt-in.
The startup gate remained closed before the 03:20 UTC restart. SSH was
unavailable during its test window and no native desktop grant was issued.
The automatic rollback ran at 03:34:17 UTC; after restart, .59 Code 0,
original SYS/ACPI hashes, removed registrations/permit/tasks and unchanged
power settings were verified at 03:39:55. The temporary system loader was
removed and the machine opt-in restored. Portrait restore/verify passed
at 03:40:09 as a no-op. All 88 returned records (12,819,703 bytes) match
device-side lengths and SHA-256 values. The native feature matrix did not run.
Retain all four API-specific `UserModeDriverName` slots when selecting the
new UMD for D3D10/11. The staged run initially reduced the list to one item;
that round never received a native grant. Its successor preserves all four
slots and replaces only the D3D10/11 entries before restarting the device.

The user's saved 720x1280 portrait setting passed interactive preflight at
03:13:48 UTC. `wake` returned success as a no-op because the input desktop
was already `Default`; it did not test touch dismissal of a running screen
saver. Physical direction, display/sleep policy and firmware were unchanged
by that check. After the native trial, restore/verify the saved mode with
the same helper in the interactive session.

The successor staged at 03:45 UTC uses a local SYSTEM task after interactive
logon: verify the gated baseline, admit one native startup, verify active
LUIDs, launch the user-session feature matrix and drawing regression, stop
and export the trace, then request rollback. The 12-minute independent
rollback remains armed. This removes timely SSH reconnection as a test
dependency; it does not remove desktop/identity checks or alter power policy.
The local workflow admitted the unchanged .144 KMD and ran on matched LUID
`8b7be` from 03:48:29.7392153 to 03:48:34.3017135 UTC, exiting 0 without
timeout. The new UMD, system loader and NVK opt-in were in use.

| Native feature request | HRESULT | Selected level |
| --- | --- | --- |
| 11_1 | `887a0004` | none |
| 11_0 | `887a0004` | none |
| 10_1 | `887a0004` | none |
| 10_0 | `00000000` | 10_0 |
| ordered list 11_1 through 10_0 | `00000000` | 10_0 |

The subsequent runtime RGBA/BGRA clear and SM4 triangle regression checks
16,384 pixels with zero mismatches and releases normally. This confirms
native rendering at FL10_0; it does not establish higher-level features or
desktop presentation.

The finalized trace exports 748 UMD events, zero malformed payloads, zero
recorded event/buffer loss and 1,853 written buffers. Three `dwm.exe` PIDs
(828, 968, 1132) account for 616 events. In all 88 OpenAdapter10_2 attempts,
the adapter callback succeeds with flags 3 and a 512 MiB budget; Zink sees
LUID `8b7be` and exact opt-in `1`, then returns no screen and the adapter
open returns `887a0004`. These failures precede the frontend's capability
negotiation and CreateDevice. The legacy OpenAdapter10 fallback requests
interface `a0002` and is rejected separately. Explorer also fails Zink setup
eight times, whereas the explicit runtime probe creates three D3D devices
successfully on the same LUID. Do not attribute DWM's current first failure
to its requested Feature Level, shared-resource open or Present yet.

The next investigation must retain the precise Zink loader, VkInstance,
physical-device selection or VkDevice failure in the desktop process. The
ICD manifest used `./libvulkan_nouveau.dll`. The original assumption that
this resolved against the manifest directory was incorrect for the deployed
Windows loader; the [later path control](#windows-icd-manifest-path)
reproduces the failure and verifies the backslash correction.
Windows Mesa logging defaults to stderr/OutputDebugString and explicitly
does not implement `MESA_LOG_FILE`, so setting that variable alone will not
collect the missing initialization error.

The 720x1280 screenshot remains black even with a successful `Default`
input-desktop check immediately before capture (96 DPI, source/signal
720x1280, identity rotation). Its SHA-256 is
`1affba48e5a5838c73c3371e829823d24c8f541ffbb1bc75ee7b5936c4504aa6`.
This is a GDI desktop capture, not a physical panel photograph.

Collection initially tried to hash its still-open automatic transcript.
The native results and finalized ETL were already saved; the outer finally
block requested rollback. The collector now excludes the live transcript,
and the full stable record set was hashed after recovery. .59 Code 0,
original driver/ACPI hashes, removed temporary UMD/ICD registration, permit
and tasks, restored machine opt-in, deleted temporary system loader and
unchanged power settings were verified at 03:54:14 UTC. Portrait
restore/verify passed at 03:54:27 without a mode change.

All 97 returned records (15,580,209 bytes) match device lengths and SHA-256
values under `runs/dwm-init-auto-20261111/evidence/`. The ETL hash is
`ef9e8d35397e2fd7e3017e14fd67e55d21cf276e2046f9aed4f0e5b9c4313535`;
the UMD event export hash is
`ee072a7101c6142c4441442ad516d30d1abc9c505bae9bc3c3dd69ee5164f846`.

Artifacts and scripts for the completed gated round remain under
`/Users/rujian/.local/share/mesa-switch-woa-build/runs/dwm-init-20261111/`;
the full build is in the adjacent `dwm-init-20261111-umd/` directory.

The `zink-init-auto-20261111` successor adds optional Zink creation-error
details and a `ZinkFailure` ETW record. The ARM64 build and three controlled
fixtures pass (04:16:24 UTC; 27 valid, lossless control events). Preflight
first finds `Screen-saver`: ordinary touch-down injection returns Win32
error 5 and the desktop remains unchanged. After the user returns to the
desktop, portrait/visibility checks pass at 04:19:18. No sleep, screen-saver
or display policy is changed.

On the same signed .144 KMD, the native trial runs from 04:23:42.9146795
to 04:23:46.5396818 UTC on matching LUID `8eb74`, exit 0 without timeout.
The feature matrix remains FL10_0 and the runtime checks another 16,384
RGBA/BGRA clear/draw pixels with zero mismatches. Its black desktop capture
has the same hash as the preceding round. The finalized ETL exports 844
valid UMD events, including 704 from three DWM PIDs. All 88 recorded DWM
screen-creation failures identify `vkCreateInstance` returning
`VK_ERROR_OUT_OF_HOST_MEMORY` (-1) at stage 4, source line 3515 in that
archived patch. Forty-four are fresh calls and 44 reuse the cached error.
This does not establish RAM exhaustion or the internal allocation site.
The ETL reports 924 lost events, zero lost buffers and 1,904 written buffers;
do not use missing events as execution evidence. The next diagnostic adds
an instance-creation debug-utils callback to retain the first error message.

Local collection finishes at 04:24:17.9146312 UTC; automatic rollback and
restart complete. At 04:28:12, .59 Code 0, original SYS/ACPI hashes,
temporary-registration/task/permit removal and unchanged power settings
are verified. System-loader removal and machine opt-in restoration are
also verified; portrait restore/verify is a no-op at 04:28:27. All 123
returned files (16,255,926 bytes) match remote lengths and SHA-256 values.
The run's `evidence/` directory retains the first screen-saver failure too.
ETL SHA-256: `e7b77cf90fd8804caf4b756ede5b0575c079c18525927edc62740c4cbcb2cc9c`;
UMD export: `4fc0f7c0fe013fe08ea3785e65c7529dd211da4fe3dde68b017c4ebcf820d079`.

The `zink-message-auto-20261111` follow-up adds a standard instance-creation
debug-utils callback and a bounded `ZinkMessage` event. Its storage belongs
to the cached instance, not the caller's stack; the callback returns
`VK_FALSE`. The full ARM64 build and three .59 controls pass, with 27 valid
lossless control events at 04:35:07 UTC. The unchanged .144 KMD runs on LUID
`8ead0` from 04:40:10.4549680 to 04:40:14.8924611, exit 0 without timeout.
The feature matrix is unchanged and all 16,384 runtime pixels again match.

Increasing the AutoLogger buffer size from 8 to 64 KiB and setting 16/128
minimum/maximum buffers yields a finalized ETL with 844 valid UMD events,
zero recorded event/buffer loss and 549 written buffers. DWM again records
44 fresh instance errors and 44 cached errors, all -1 at source line 3518
in the archived patch; no `ZinkMessage` is exported. The error text remains
unidentified: this does not prove the callback was enabled or locate the
failure relative to it. The desktop capture is still the same black image.

The deployed loader was independently copied back and matches SHA-256
`903719814032af3670373c7a82273917d80624d4ff0e50c3cbc3d72ae42b783a`.
It reports only `Vulkan Loader - Dev Build`. Its retained symbols show that
`vkCreateInstance` first requests 5,328 bytes through `loader_calloc`, before
installing debug messengers, and can return -1 there without a callback.
This is a possible unobserved path, **not proof that this allocation failed**.
Earlier extension enumeration and subsequent loader/ICD calls still need
their own failure attribution. The binary and `loader-vkCreateInstance.asm`
are retained under the build root's `references/` directory.

Collection completes at 04:40:45.1424652 UTC. Recovery verification passes
at 04:43:42 (.59 Code 0, original SYS/ACPI, no temporary registration,
permit or test tasks, identical power settings); the loader/opt-in cleanup
also passes. Portrait restore/verify is a no-op at 04:43:54. All 117 returned
files (34,154,872 bytes) match remote hashes. ETL SHA-256:
`fe772098510997510e3ac1c181aeb9c5079c24794d48c35e519b1e6803109f9f`;
UMD export: `7ca9594655694a73d9cf783a16b2fdd362c33e43d4b0bca4a31032331bac6579`.

### Instance extension discovery

The `instance-calls-auto-20261111` trial retains version, extension count/list,
layer count/list and instance-creation results, including the called mask,
enabled counts, debug-utils availability and cached-snapshot flag. Failed
API outputs are not treated as valid counts. Successful screen creation
also supplies the snapshot for comparison with DWM. No initialization
failure/retry policy is changed by this diagnostic.

The full UMD build and three controls pass (05:35:04 UTC; 27 valid, lossless
control records). The run uses the final memory-error ICD
`20a3c7b42bfc6ed05ac5974acb981a518d8b496c12729c4f8e4ac6fd02d0aa4c`
and the unchanged signed .144 KMD. Visibility and saved portrait verification
now occur after reboot, before the native startup grant. Both pass on Code
43 at 05:39:07. This avoids depending on an illuminated desktop during
local build work. The later Code 0 observer checks visibility again but
does not repeat the saved-preference comparison; its `PreferenceValid=false`
means not checked there, not a detected mode mismatch.

Native tests run on matching LUID `92fa3` from 05:40:02.8931785 to
05:40:07.1119275 UTC, exit 0 without timeout. The feature matrix remains
FL10_0 and all 16,384 RGBA/BGRA clear/draw pixels match. The black capture
still hashes to `1affba48e5a5838c73c3371e829823d24c8f541ffbb1bc75ee7b5936c4504aa6`.

The finalized trace contains 1,490 valid UMD records, zero recorded
event/buffer loss and 548 written buffers. DWM contributes 1,232 records
from PIDs 464, 1120 and 1376. Its 88 snapshots consist of 44 fresh attempts
and 44 cached failures. Version enumeration succeeds with 1.4.354; the
first extension-count query returns -1, so the extension-list call is not
made. Layer count/list succeed with zero layers; CreateInstance returns -1.
The called mask is 59, enabled extension/layer counts are zero, and the
debug-utils availability/chained flags are both absent. Explorer has the
same sequence in eight snapshots (four fresh/four cached). The native
runtime probe has ten successful snapshots (six fresh/four cached), 16
discovered extensions, eight enabled extensions, no layers and a chained
debug-utils callback.

This moves the first observed failure from CreateInstance to the earlier
instance-extension query. The missing message does not implicate an
allocation before callback registration: Zink could not discover the
debug-utils extension at all. Investigate the loader's ICD discovery and
extension aggregation, with direct loader diagnostic output. Do not infer
physical RAM exhaustion, an NVK GPU-budget rejection, or a particular
loader branch from the generic -1 alone.

Collection completes at 05:40:37.0650533 UTC. Recovery verification passes
at 05:45:54 (.59 Code 0, original driver/ACPI hashes, removed registrations,
permit/tasks and unchanged power policy), with loader/opt-in cleanup also
confirmed. Portrait restore/verify is a no-op at 05:46:06. All 114 returned
records (34,275,580 bytes) match remote lengths and hashes. ETL SHA-256:
`c5a314c2bce2dbca294c368daec195bf4c0558d3848e541925f1e31c9d60aa75`;
UMD export: `a680f1dfe901db9a4749f4dc7054ca4df69aa554c00678a606e1e26ffb7c746f`.

### Windows ICD manifest path

The `loader-debug-auto-20261111` trial keeps the UMD, ICD and signed .144
KMD unchanged. A bounded, session-1 Microsoft DbgViewCLI ARM64 capture
collects the loader's `OutputDebugString` output with temporary
`VK_LOADER_DEBUG=error,warn,driver`. The receiver's independent two-message
control passes; the trial also retains both boundary markers and exits 0.
No kernel capture or boot-logging driver is used.

The matching UMD trace identifies DWM PIDs 828, 5896 and 4772. Each has
uninterleaved loader errors reporting that `./libvulkan_nouveau.dll` cannot
be opened with Windows error 87. The ICD manifest itself is found.
The UMD records the same failed extension count and instance creation as
before: 44 fresh DWM attempts and 44 cached snapshots. All 1,490 events
decode, with zero recorded event/buffer loss and 517 written buffers.
Debug output is weaker evidence: of 1,508 captured rows, 69 contain
interleaved message fragments and are excluded from per-PID conclusions.
The receiver supplies no ETW-style loss counter.

The deployed loader's disassembly confirms that path processing tests for
backslash. Its library-open helper first calls `LoadLibraryW`, then on
error 126 retries `LoadLibraryExW` with flags `0x1100` (DLL load directory
and default directories). Windows requires an absolute path with the DLL
load-directory flag. The unresolved relative path is consistent with that
fallback returning error 87; the subsequent API result is -1. This is a DLL
loading failure, not evidence that GPU memory allocation exceeded 512 MiB.
[Windows DLL path and flag contract](https://learn.microsoft.com/en-us/windows/win32/api/libloaderapi/nf-libloaderapi-loadlibraryexw)

The false OOM translation is independently identified in
`loader_handle_load_library_error`: it tests the formatted message with
`strstr(error_message, " with error 8")`, which also matches error 87.
The deployed binary contains that exact string at RVA `62eba`, selects
library status 22 on a match, and `loader_scanned_icd_add` converts status
22 to Vulkan -1. The inspected upstream source has the same prefix test.
This external-loader defect remains a follow-up for the loader build;
the current fix removes the invalid manifest path without replacing the
loader or changing its error semantics.
[Upstream load-error classification](https://github.com/KhronosGroup/Vulkan-Loader/blob/main/loader/loader.c)

The native runtime still passes 16,384 RGBA/BGRA clear/triangle pixels on
LUID `7e769`, from 06:36:06.3871376 to 06:36:11.5277628 UTC, exit 0 without
timeout. FL10_0 remains the supported level and the desktop capture is
still black. The receiver stops at 06:36:31; collection finishes at
06:36:50. .59 Code 0 and exact driver/ACPI hashes are verified at 06:40:21,
with removed registration, permit/tasks, temporary loader/debug environment
and receiver, plus unchanged power settings. Portrait verification is a
no-op at 06:41:04. All 120 returned records (34,655,792 bytes) match device
lengths and hashes. ETL SHA-256:
`9a8be8973f45a70ddc69ee06400f6e3e2610859142a4c8e184ced7f4b49820b6`;
loader CSV: `ee4bac54a2d671cfe3870d4a687e7a08b3c9dc862473e74c2e8a59a6f461f9ff`.

The `manifest-path-control-20261111` comparison runs the unchanged loader
and ICD on .59, with the probe executable outside the ICD directory.
Only the manifest separator and process working directory vary:

| Manifest library path | Working directory | Instance result |
| --- | --- | --- |
| `./libvulkan_nouveau.dll` | ICD directory | success |
| `./libvulkan_nouveau.dll` | System32 | Windows 87, Vulkan -1 |
| `.\libvulkan_nouveau.dll` | ICD directory | success |
| `.\libvulkan_nouveau.dll` | System32 | success |

The successful cases subsequently find no native GPU on .59 and the full
identity utility exits 10; the failing case exits 8 at instance creation.
The earlier control placed the executable beside the ICD, which masked the
working-directory dependency. All 24 returned control records (13,859
bytes) match remote hashes. Temporary legacy ICD registrations are removed
and .59 Code 0 is rechecked at 06:48:42 UTC.

`build_icd.py` now emits the Windows backslash path through JSON encoding.
No allocator, budget, feature level or executable binary changes are needed
for this packaging correction. Creating a loader instance in this control
does not establish DWM composition or presentation.

The separate `manifest-path-auto-20261111` hardware trial changes only the
manifest path, retaining the same binaries and capture instrumentation.
The corrected manifest hashes to
`829706b27380ae3cc24ce89c85683d3c9377816e219d89e2ece8dd0242610bb3`.
On matching native LUID `994f3`, DWM PIDs 452, 5124 and 2768 now successfully
complete version, extension-count/list, layer-count/list and instance
creation. The twelve snapshots are six fresh and six cached successes:
16 discovered extensions, eight enabled extensions, no layers, with
debug-utils available and chained. Zink screen creation and adapter open
also succeed. Each DWM process reaches a successful D3D UMD CreateDevice
return for DDI interface `a0009`. These positive records establish that the
manifest correction removes the observed initial failure.

The finalized ETL exports 420 valid UMD records, including 192 DWM events,
with zero recorded event/buffer loss and 731 written buffers. It records
no DWM OpenResource/Present/Blt events. The loader capture has 511 rows;
some messages interleave, including non-UTF-8 system text. Original bytes
are retained, and only ASCII diagnostic fields are analyzed. Absence of a
loader error is not used as the success proof above.

Native tests run from 06:53:37.1285579 to 06:53:42.3160582 UTC, exit 0
without timeout. FL10_0 still matches 16,384 RGBA/BGRA clear/triangle pixels;
higher feature levels remain unsupported. The desktop capture is unchanged
and black. The DWM event log now records three exits with `c0000409`, after
the successful device returns. The selected Application 1000/1001 query
for 06:53:15–06:54:25 yields no matching DWM records. The code alone does
not identify the failing DDI or prove stack/heap corruption. Next retain
the first post-device resource/runtime failure, then complete sharing and
Present; this is not yet a desktop-composition pass.

Collection completes at 06:54:20. .59 Code 0, original driver/ACPI hashes,
removed temporary registration, permit/tasks, loader/debug environment and
receiver, and unchanged power policy are verified at 06:58:43. Portrait
restore/verify is a no-op at 06:59:36. All 122 returned records (34,141,870
bytes), including the later event query, match remote lengths and hashes.
ETL SHA-256: `e8e30b9dfcd2099526c2fdd806245d1fdcd6139dac3a9efdf0bdb10f6af6bd28`;
UMD export: `135865f2a4afca0d2528b6e2aea5f5602a2639741cfebf348010c963b0ef56a0`;
loader CSV: `c0379df4a7433e024754f0eeadb87a85dd4f164a8a859accfde25c9aa17fb054`.

### Post-device DDI trace

The `post-device-auto-20261111` round adds resource creation, format-query
and device-teardown boundaries, plus a source-located `RuntimeError`
event before `pfnSetErrorCb`. The latter uses a distinct 328-byte payload
with HRESULT, source basename/function/line and process image. The original
return values, capability bits and allocation behavior are unchanged.
See the [payload definitions](../../src/gallium/targets/d3d10umd/README.md).

The ARM64 UMD build passes, retaining the pre-existing ShaderTGSI warnings.
UMD SHA-256: `b00ba41c916c69b905a1f8b40abdcf1339472b2e92710d5e4b35b3c5ca23bf89`.
At 07:18:40 UTC, all three controlled fixtures pass. The 46 valid ETW
records include three resource allocation/map errors (`8007000e`) and
two device-removal callbacks (`88760870`), with the expected source sites.
There is no recorded event/buffer loss, and three buffers are written.
An initial PowerShell syntax preflight rejects the dump setup script
before driver installation; the missing brace is corrected before retry.

The hardware run retains the signed .144 KMD, memory-error ICD and fixed
backslash manifest. On matching LUID `90f60`, the native runtime runs from
07:26:06.2469440 to 07:26:11.9344414 UTC, exits 0 without timeout and again
matches 16,384 RGBA/BGRA clear/triangle pixels at FL10_0. The desktop capture
is unchanged and black.

The finalized ETL exports 22,407 valid events, zero recorded event/buffer
loss and 769 written buffers. DWM PIDs 772, 1632 and 1292 contribute 8,484
events. Their 18 resource creations all have matching successful endpoints:
each process creates constant buffers of 48, 4,000, 16 and 48 bytes, then
vertex buffers of 240,012 and 102,400 bytes. All have misc flags zero and
one subresource. DWM also completes 294 format-query pairs and 7,650
multisample queries. No DWM RuntimeError, OpenResource, Present, Blt or
device-teardown event is recorded. These observations do not cover every
shader/state DDI and do not prove that no internal error occurred.

DWM again reports three `c0000409` exits. A temporary per-application WER
configuration requests up to three minidumps in a dedicated directory,
granting write access only to the observed DWM account in addition to
inherited permissions. No dump is generated. This is an attempted capture,
not exception-context evidence or proof of a particular termination path.
Next obtain the exception context and trace the uncovered shader/state
boundaries. Do not infer a fault from the last successful buffer or change
capabilities merely to move that endpoint.
[WER local dump scope and limitations](https://learn.microsoft.com/en-us/windows/win32/wer/collecting-user-mode-dumps)

Collection finishes at 07:26:51. Recovery verification at 07:32:17 confirms
.59 Code 0, original driver/ACPI hashes, removed temporary registration,
permit/tasks, loader/debug environment and receiver, unchanged power
settings, and restored WER settings and dump-directory ACL. Portrait
restore/verify is a no-op at 07:32:49. All 134 returned records (38,295,250
bytes) match remote lengths and SHA-256 values. ETL:
`4fce9ece9e3a54050decff0384681af60f929ebfd36b50201e74cdaf5e0fe1c0`;
UMD export: `0b23877d5637bcd68e7758f4a444f27ec9d9815a1c70a3c7eb63bb91e01e0002`.

### DWM TGSI SAMPLE abort

The `dwm-exception-auto-20261111` trial uses the unchanged post-device UMD,
the corrected ICD manifest and the signed .144 miniport. A bounded ProcDump
attachment captures an ARM64 exception dump from DWM PID 484, thread 1708.
The exception is `c0000409`, fast-fail parameter 7, at
`ucrtbase.dll+76d8c`. The first exact UMD return address is
`TegraGpuD3D.dll+21de48`; the preceding instruction at RVA `21de44` calls
`abort()`. The matching binary's `ttn_parse_tgsi` disassembly identifies the
unsupported-opcode branch, with w26 = `0x91` = `TGSI_OPCODE_SAMPLE`.
This locates a shader-translation failure; it is not evidence of exhausted
memory or a reason to change the feature-level advertisement.

The 5,906,251-byte dump SHA-256 is
`9150b32e8fb38ccdcd83570b9a43b47bdef88d59d7cddcc54aceba798f699197`.
Matching UMD SHA-256:
`b00ba41c916c69b905a1f8b40abdcf1339472b2e92710d5e4b35b3c5ca23bf89`.
The ARM64 stream/context parser retains raw addresses and labels any masked
address interpretation separately; the fault and UMD caller above match
without masking. Its independent fast-fail-7 control also produces a valid
exception stream. ProcDump exits 1 after that control capture, so a zero
tool exit code is not used as the dump-validity criterion.
[ProcDump capture options](https://learn.microsoft.com/en-us/sysinternals/downloads/procdump)

Ordinary native FL10_0 rendering still matches 16,384 pixels on LUID
`9120c`, 08:07:31–08:07:38 UTC, exit 0 without timeout. The debugger-stop
helper then rejects a changed controller-process identity after ProcDump
has exited; automatic collection stops, while independent rollback succeeds.
The finalized ETL and dump are collected after recovery. The dump-copy
summary initially also fails because its records are hashtables; changing
them to objects fixes the summary, and the original error remains recorded.
All 132 returned records (44,042,686 bytes) match device lengths and hashes.
.59 Code 0, original driver/ACPI hashes, cleanup and unchanged power policy
are verified at 08:15:53; portrait verification passes at 08:19:31.

The working-tree fix lowers ordinary `SAMPLE` with independent texture-view
and sampler-state indices, the declared texture target, array coordinates,
offsets and result swizzles. Zink retains the separate NIR texture and bare
sampler, assigns nonoverlapping graphics bindings, and accounts for sampled
images separately from combined descriptors in its pools. Other SAMPLE
variants are not claimed by this change. The legacy combined TEX path remains
covered, and no feature level, GPU budget or scanout configuration changes.

On .59, the first six-case semantic run finds an incorrect texture-usage bit
inherited from the sampler declaration mask. Recording texture usage from
instructions fixes it. All six cases then pass on ARM64. The D3D runtime
sampling probe uses texture slot t5 and sampler s2, with deliberately wrong
bindings at t2 and s5. Its WARP control checks point wrap/clamp, rebinding
and channel swizzles across RGBA/BGRA targets: 24,576 pixels match. The first
control wrapper fails to retain process exit codes; a corrected wrapper
records both exits as 0 without timeout at 09:15:17–09:15:18 UTC. Original
results remain under `runs/sample-control-20261111/`. These are CPU/software
controls, not a native sampling or desktop-composition pass.

The next `sample-native-auto-20261111` trial runs without an exception
debugger. The signed .144 miniport reaches Code 0 with successful hardware
and panel initialization; the UMD traces and native command use LUID `9a316`.
The retained first ETL exports 34,774 valid UMD records, including 26,116
from DWM. DWM receives nine successful CreateDevice returns and 172 successful
resource creations. These include 36 720x1280 BGRA textures across repeated
device attempts (27 ordinary and nine with `DISPLAYABLE_SURFACE` misc flag
`0x20000`),
plus one 50x50 BGRA texture. Nine `SetDisplayMode` records explicitly carry
`887a0004` (`DXGI_ERROR_UNSUPPORTED`), matching the hardware stub in
`DxgiFns.cpp`. Eight device teardown pairs complete. The retained Application
event reports one Tegra-attributed DWM exit with `8898008d`.

The trial then restarts; a new boot is observed at 09:22:10 UTC. The command
log reaches the feature-level probe, but the ordinary and sampling probes
have no completed result. The direct identity output contains zero bytes
instead of JSON. The first ETL has `EndTime=0` and `BuffersWritten=0`, so its
zero recorded loss counters and lack of RuntimeError/Present events do not
establish a complete trace. The final event is not a fault location, and this
record does not identify the restart's cause. Native sampling, the latest
ordinary-draw regression and desktop composition remain unverified.

The consumed permit prevents another native start. The automatic trial's
second invocation refuses its existing marker and starts rollback; its new
transcript replaces the earlier automatic transcript. The driver/ACPI,
temporary environment/registration and power-policy verification passes at
09:28:09 on .59 Code 0. Portrait restore/verify is a no-op at 09:28:39.
All 121 returned records (63,349,783 bytes) match device lengths and hashes.
First ETL SHA-256:
`bd8993c71059d314a0819132e19b603c895237a5b6489b28c08468637f383447`;
UMD export: `a26ef33d8beb7bfc696742b8100c40e6f4a5a701236776464f59449614e44671`.
Tested UMD: `bfceb207ad4accba3b7610fa3735594144f1ce512628a703a1c142b455920171`.
A subsequent source review expands the compact program-layout arrays for
separate sampler states plus storage images. That rebuild passes, with UMD
SHA-256 `8170b20f06f64cc2e472b6a2a90d17f818224ce9614f22fb9d2744eafc28b4bc`;
this capacity adjustment is not part of the native trial above. The semantic
fixture and sampling probe hashes are unchanged.

The next presentation step is a real primary allocation/resource bridge.
Translate the DXGI resource/subresource to the corresponding primary
allocation, marked as primary when allocated, and call `pfnSetDisplayModeCb`
from the SetDisplayMode DDI on its calling thread. Preserve its HRESULT and
the required format-negotiation behavior. A successful texture creation does
not supply this missing runtime association by itself.
[Primary allocation contract](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/dxgiddi/ns-dxgiddi-dxgi1_1_ddi_base_functions)
[Display mode callback](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dumddi/nc-d3dumddi-pfnd3dddi_setdisplaymodecb)
[D3D11 callback-thread requirements](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/changes-from-direct3d-10)

### Host allocation and budget audit

The 2026-11-11 audit follows the reported DWM instance OOM into the actual
Windows paths. NVK's `nvk_CreateInstance` requests 8-byte-aligned metadata
through `vk_default_allocator`, which uses CRT `malloc`; the deployed
loader's initial instance storage uses `calloc`. The loader has no direct
VirtualAlloc/large-page import, and the inspected Windows instance path
does not use the POSIX `os_mmap` wrapper. The tested UMD build also disables
the shader disk cache. This does not exclude a process-specific CRT/loader
failure, and does not prove that all physical RAM is healthy.

GPU storage is a later path. `tg_kmt_allocate` rounds byte sizes to 4 KiB
with an overflow guard. Shared CPU-visible storage uses
`PAGE_READWRITE | SEC_COMMIT | SEC_WRITECOMBINE`; neither `MEM_LARGE_PAGES`
nor `SEC_LARGE_PAGES` is requested. GPU virtual-address alignment is handled
by reserving padding and retaining the original reservation for release;
it is not a request for Windows CPU large pages. KMD private memory uses
the WDDM contiguous-memory callback with noncached mapping and page-sized
range checks. These are distinct APIs and alignment contracts.
[Microsoft section flags](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-createfilemappinga)

The budget constant is `UINT64_C(512) * 1024 * 1024` = 536,870,912 bytes.
The user-mode counter charges page-rounded bytes, while the current KMD
source enforces the adapter-wide ceiling after reserving its 16 MiB scanout
segment (496 MiB left for the counted allocations). Rejected batches restore
their charges. `nvk_CreateInstance` does not call these KMT charging routines.
The audit found no byte/page-unit or 32-bit overflow error here. It does
not establish complete cross-process `VK_EXT_memory_budget` accounting or
mean that all driver/OS housekeeping memory is part of this reported heap.

One concrete error-translation defect was found: failed shared
`CreateFileMappingW` calls mapped every error except invalid parameters to
`STATUS_NO_MEMORY`. The bridge now preserves the SDK's
`NTSTATUS_FROM_WIN32` value. NVK maps `ERROR_NOT_ENOUGH_MEMORY`,
`ERROR_OUTOFMEMORY`, pool/resource exhaustion and memory/pagefile/commit
quota errors to host OOM; unexpected
access, privilege, parameter or unsupported-operation failures retain their
code and return `VK_ERROR_UNKNOWN`. The successful allocation flags and
budget are unchanged. This path follows logical-device/resource creation,
so it is **not established as the cause of DWM's earlier instance OOM**.
[Vulkan unexpected-error semantics](https://docs.vulkan.org/refpages/latest/refpages/source/VkResult.html)

The complete ARM64 ICD build passes. The SDK fixture intercepts only the
section-creation import in its test binary: seven injected errors verify
exact error preservation, no KMT allocation/residency call, and restored
budget. Success still calls the real Windows section API and checks its
write-combined mapping. Additional controls exercise exactly 512 MiB,
4 KiB rounding, over-budget rejection and 4 KiB/64 KiB/2 MiB GPU-VA
alignment/release. KMT calls are substituted, so the 512 MiB case commits
no such physical allocation. All these controls pass on .59 Code 0 at
05:02:58 UTC under `runs/memory-audit-control-20261111/`.

The same run tests the unchanged physical-baseline loader/ICD from both
the test directory and System32. The initial elevated SSH process ignores
`VK_DRIVER_FILES` and returns -9 with no registered ICD; the supposedly
limited interactive task also still has an administrator token. No UAC
policy was changed. A temporary, separately named legacy ICD registration
then lets both working directories create the instance successfully.
Device enumeration returns -3 and the identity utility exits 10, because
.59 exposes no native Vulkan GPU; these are not native-rendering passes.
The temporary registration is removed in the script's finally block.
This control does not reproduce DWM's -1 or identify its process-specific
cause. [Loader privilege rules](https://github.com/KhronosGroup/Vulkan-Loader/blob/main/docs/LoaderDriverInterface.md#exception-for-elevated-privileges)

At 05:10:45 UTC, .59 Code 0, removed test registration/task and unchanged
display/sleep power policy are verified. No display driver was switched
for these controls. The subsequent [instance-discovery trial](#instance-extension-discovery)
passes the new ICD's native rendering regression and identifies the
earlier extension-count failure in DWM. OS-error injection remains a
controlled test; DWM presentation is still unresolved.

The final `memory-errors-20261111-icd` build includes the pool/quota error
classes and the expanded 13-case SDK fixture. At 05:19:04.1921933 UTC,
the fixture and the new ICD's direct instance/identity control both exit 0
on .59 (zero native Vulkan devices, no GPU rendering). The original
seven-case result remains preserved separately. Final ICD SHA-256:
`20a3c7b42bfc6ed05ac5974acb981a518d8b496c12729c4f8e4ac6fd02d0aa4c`;
fixture: `46c3835cc63e58c095b055cf732efc5b3d4b0e6e1aaf533567952e3184bb6c89`.
All 52 returned control records (59,464 bytes) match device-side lengths
and SHA-256 values in the run's `evidence/verified.json`.

## Direct3D UMD and DWM

The November 12 instruction supersedes the earlier Zink-first staging:
develop **Windows D3D runtime → native UMD/DDI backend → Vulkan/NVK → Tegra
KMD**, without Zink, software rendering or the existing Gallium `d3d10umd`
implementation in the replacement. Desktop DXVK development is authorized as
a possible code-reuse route; Claude is asked to assess the concrete design.
Retain the existing rendering and sharing evidence as regression baselines.
Direct Flip remains deferred until image correctness and performance are
established. Zink remains a later OpenGL compatibility route. The historical
implementation notes below describe the existing baseline, not the selected
replacement architecture.

The .130 trial established visible native scanout on GM20B. The same trial
recorded `D3D11CreateDevice` returning `DXGI_ERROR_UNSUPPORTED` for the
explicit GM20B adapter and successful WARP creation. DWM reported
`MILERR_NO_HARDWARE_DEVICE`. The installed `windows/no_d3d.c` exports
adapter entry points that reject Direct3D.
[Physical result and evidence](NATIVE_STARTUP_RESULT.md)

An experimental D3D10 UMD target now connects the existing Gallium frontend
to Zink/NVK. Its adapter entry point queries the runtime callback for the
Tegra ABI, requires the native-submission capability, and selects the exact
Vulkan device LUID. It exports no software-adapter KMT emulation. The installed
driver still uses `no_d3d.c`; the new target is not installed by its build tool.

```sh
python3 tegra-x1-wddm/tools/build_umd.py \
  --out build/umd --cross-file "$WINDOWS_CROSS_FILE" \
  --wdk-include "$WDK_INCLUDE" --sdk-include "$SDK_INCLUDE"
```

Use the LLVM-MinGW/Meson environment described above. The tool stages the
required SDK/WDK headers under the output directory and emits `TegraGpuD3D.dll`
and three controlled Windows tests. Run `test_d3d10_nvk_adapter.exe`,
`test_d3d10_nvk_device.exe` and
`test_d3d10_nvk_entry.exe <absolute-TegraGpuD3D.dll-path>` on Windows.
The tests substitute adapter callbacks or a Gallium backend and do not prove
GPU rendering or acceptance by DWM.

The explicit `nvk_d3d_check.exe <absolute-UMD.dll> <absolute-NVK-ICD.json>
<16-digit-KMT-LUID>` probe calls the real DDI frontend and Zink/NVK for a
64x64 RGBA clear/copy/readback. It is not a default Meson test. It requires a
native-submission KMD and matching active adapter identity. Elevated Windows
processes ignore `VK_DRIVER_FILES`; the physical .133 test used a temporary
`VulkanDriverName` value in that device's software key, with removal included
in rollback. Normal packaging must register its ICD through the driver package.
[Loader discovery and privilege rules](https://github.com/KhronosGroup/Vulkan-Loader/blob/main/docs/LoaderDriverInterface.md#driver-discovery-on-windows)

The .143 trial passes three native Windows D3D11 runtime runs at feature
level 10_0. RGBA/BGRA clear and SM4 triangle readback match 49,152 pixels;
all three processes complete implicit release and exit 0. The UMD fixes
the fallback barrier's serial return and keeps queue-finish barriers alive
until every worker has returned. This supersedes the earlier .133 command
rejection and the later exit faults. The next work is runtime allocation,
shared-image metadata, synchronization and sustained composition/presentation.
[Native runtime and lifetime evidence](D3D_UMD_LIFETIME.md)
Device and resource tests must select and report the GM20B LUID.
The UMD must handle runtime-owned allocation and presentation callbacks,
resource layout metadata, ownership and completion across devices.
The KMD now accepts the versioned resource-level storage envelope, with
controlled ARM64 coverage of create/open/cleanup. The UMD image payload and
NVK binding remain to be connected; see the
[current resource metadata work](D3D_RUNTIME_SHARING.md#resource-metadata-work).
[Adapter DDI](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/nc-d3d10umddi-pfnd3d10ddi_openadapter)
[Shared-resource DDI](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/nc-d3d10umddi-pfnd3d10ddi_openresource)

The Windows KMT backend can create a single-allocation Tegra storage resource
with NT security sharing, export an NT handle, and open it on another device.
Import validates the allocation metadata, charges the importing device's
budget and waits for residency. Each open has a separate resource handle,
and the caller owns the NT handle. NVK shared storage allocations use this
resource path. NVK now implements OPAQUE_WIN32 import/export, including named
NT handles, security/access attributes and handle inheritance. It reports
only that handle type for buffers and images; D3D texture and legacy KMT
handles remain unsupported. The importing memory retains its own resource
reference without consuming the caller's NT handle. Imported memory is
registered in NVK's memory list and is not zeroed or debug-filled.
CPU-visible shared storage uses a write-combined section and the standard
ExistingHeap allocation path. The KMD handles the standard raw-buffer
description and linear surface metadata. The .132 physical trial passes
CPU readback at three ownership stages, including after producer release
and NT-handle closure. That test uses two devices in one process on GM20B.
The separate `nvk-windows-vulkan-shared-check.exe <absolute-vulkan-1.dll>`
probe tests buffer and optimal RGBA/BGRA image GPU access through the Vulkan
loader, external queue-family transfers, producer-device destruction and
re-export. It is an explicit physical test, not a default Meson test.
Cross-process sharing, D3D runtime image metadata and DWM remain separate work.
[Physical shared-storage result](SHARED_STORAGE_RESULT.md)
[Win32 Vulkan memory implementation and validation](VULKAN_WIN32_MEMORY.md)
[KMT resource sharing](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmthk/nf-d3dkmthk-d3dkmtshareobjects)

Mesa's `src/gallium/frontends/d3d10umd` supplies D3D10 DDI and shader-token
translation code. `State.h` currently disables its D3D10.1 and D3D11 branches.
The default GDI target still creates softpipe or llvmpipe screens. The NVK
target links Zink, but `Resource.cpp::OpenResource` still reports an error
and runtime-owned allocation metadata is not connected. Its Present and
SetDisplayMode callbacks return unsupported until the native bridge exists.
[Existing frontend](../../src/gallium/frontends/d3d10umd/)
[GDI target](../../src/gallium/targets/d3d10umd/)

DXVK is selected for the application-facing DX9/10/11 game path. Integrate a
pinned upstream version over NVK,
preserve its license and attribution, and validate the Windows binary/loader
architecture, required Vulkan features, resource synchronization and Win32
presentation. Desktop DXVK is now authorized, but still needs native DDI/runtime
integration. DXVK application rendering establishes application API
compatibility; hardware DWM acceptance needs
the desktop route's own device, shared-surface and sustained-composition
evidence. Generic compatibility work is in scope; individual-game debugging
remains outside this goal.
[DXVK upstream](https://github.com/doitsujin/dxvk)
[Current runtime resource investigation](D3D_RUNTIME_SHARING.md)

## Windows shader cache

The Windows UMD and NVK ICD builds enable Mesa's persistent blob cache.
`src/util/disk_cache_win32.c` implements the existing cache interface with
Win32 file operations; Zink's translated shader/pipeline cache and NVK's
compiled-shader cache use it. Writes run on a low-priority worker and publish
complete blobs by atomic replacement. A shared index and file lock coordinate
writers across processes. Invalid or unavailable entries become cache misses.

The default directory is `%LOCALAPPDATA%\mesa_shader_cache_win32` for the
process identity. `MESA_SHADER_CACHE_DIR` selects the parent directory instead.
`MESA_SHADER_CACHE_DISABLE=true` disables it and
`MESA_SHADER_CACHE_SHOW_STATS=true` reports hit/miss counts when a cache is
destroyed. `MESA_SHADER_CACHE_MAX_SIZE` controls the disk-blob limit (K/M/G,
default 1 GiB); a fixed key index is additional metadata. This is separate
from the unchanged 512 MiB dynamic GPU budget. This backend stores blobs
without compression and does not require zlib or zstd.

The namespace includes driver/device identity and compiler flags; Zink also
includes its DLL identity before hashing the Vulkan pipeline-cache UUID.
Keep matched UMD/ICD artifacts when testing shared-image changes. The Windows
ARM64 cache control validates persistence across process restart, simultaneous
writers, namespace separation, integrity, eviction and the disable option on
the restored `.59` adapter. It does not execute GPU work. Native cold/warm
shader compilation and desktop frame-time comparisons still require a matched
hardware trial.

## deko3D Maxwell reference

Consult deko3D alongside Mesa/NVK for GM20B command encoding, texture and
sampler descriptors, graphics state, synchronization and cache operations.
Relate those hardware sequences to the Windows KMD's VidMm mappings,
residency and scheduler completion contract. Reused source retains deko3D's
zlib notice and identifies local changes.
[Source](https://github.com/devkitPro/deko3d)
[License](https://github.com/devkitPro/deko3d/blob/master/LICENSE)

## RADV Windows reference

Collabora's experimental `wddm2` branch pairs a Mesa Vulkan ICD with AMD's
Windows kernel driver. Use the pinned revision to compare Windows interfaces
while keeping GM20B resource metadata and submission on the Tegra ABI.
[Pinned source](https://gitlab.freedesktop.org/lfrb/mesa/-/tree/801c9763c6043f0de8408e905a5324eea06d81d7) · [Collabora's porting notes](https://www.collabora.com/news-and-blog/news-and-events/cracking-windows-open-porting-radv-to-win32.html)

- **Mesa runtime and Win32 WSI.** NVK enables the Win32 platform,
  `VK_KHR_win32_surface` and `VK_KHR_swapchain`. Its GDI presentation path
  waits for the GPU copy fence before reading the image. Win32 WSI initializes
  DXGI and DComp for drivers with a D3D12 queue callback. Use `vkcube` to
  verify the GDI path on the Lite. The DXGI path requires a working D3D12
  device and queue on the same adapter LUID. Validate active VidPN, Tegra
  primary flips and hardware DWM composition separately.
  [NVK gate](../../src/nouveau/vulkan/nvk_physical_device.h) · [Instance extensions](../../src/nouveau/vulkan/nvk_instance.c) · [Mesa Win32 WSI](../../src/vulkan/wsi/wsi_common_win32.cpp)
  [Monitored fence](https://gitlab.freedesktop.org/lfrb/mesa/-/commit/629e52171e03c6b7f53137236819b8f81d4eb149) · [Blit synchronization](https://gitlab.freedesktop.org/lfrb/mesa/-/commit/98efb6c0b26d6af5230ef4bed7cbd15eda6240c8) · [Win32 WSI](https://gitlab.freedesktop.org/lfrb/mesa/-/blob/801c9763c6043f0de8408e905a5324eea06d81d7/src/amd/vulkan/winsys/wddm2/radv_wddm2_wsi.c)
- **Adapter selection and build.** Compare DXGI and DXCore enumeration
  when matching a presentation adapter to NVK's existing KMT LUID. Keep
  the SDK-backed ARM64 bridge for Windows structure layouts. Revisit the
  branch's thunk dispatch generator if NVK needs optional KMT entry points.
  [Adapter enumeration](https://gitlab.freedesktop.org/lfrb/mesa/-/commit/7a88cc539b31cfa94f5502b1e4592efb5f9aa08e) · [Thunk generation](https://gitlab.freedesktop.org/lfrb/mesa/-/commit/a793e2045d9fdd43ef734f8f08f5b1315656795f)
- **KMT transport and external objects.** Compare GPU VA mapping, paging
  fences, queue waits and signals, and NT-handle resource import with the
  existing Windows NVK backend. NT-handle monitored-fence import/export now
  exists, with SDK-backed controls executed on Windows; native GPU validation
  and the runtime/NVK per-submission bridge remain pending. Use
  [D3DKMTShareObjects](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmthk/nf-d3dkmthk-d3dkmtshareobjects)
  and [D3DKMTOpenSyncObjectFromNtHandle2](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmthk/nf-d3dkmthk-d3dkmtopensyncobjectfromnthandle2)
  with WDK types. Size the runtime, resource
  and allocation private-data regions separately when opening shared
  images. Shared images need a Tegra layout and lifetime contract for the
  KMD and importers. AMD allocation blobs and command streams are specific
  to its kernel driver.
  [NT resource sizing](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmthk/ns-d3dkmthk-_d3dkmt_queryresourceinfofromnthandle)
  [Allocations and imports](https://gitlab.freedesktop.org/lfrb/mesa/-/blob/801c9763c6043f0de8408e905a5324eea06d81d7/src/amd/vulkan/winsys/wddm2/radv_wddm2_bo.c) · [Queue submission](https://gitlab.freedesktop.org/lfrb/mesa/-/blob/801c9763c6043f0de8408e905a5324eea06d81d7/src/amd/vulkan/winsys/wddm2/radv_wddm2_cs.c)
- **`VK_EXT_memory_budget`.** NVK already exposes this extension. Its
  Windows values are estimates by default: free system RAM scaled into the
  512 MiB heap, and this process's `vkAllocateMemory` total. The KMT bridge
  can query `D3DKMTQueryVideoMemoryInfo` for the calling process and keeps
  the local and non-local segment groups separate. Which group VidMm charges
  for the heap is not decided by source: the KMD's page-table and scanout
  segments declare `LocalBudgetGroup`, but the paging-aperture segment that
  holds every ordinary allocation declares neither budget-group flag, and
  the WDK does not document a default. `nvk-windows-memory-check.exe <LUID>`
  measures it on the device by sampling both groups around one resident
  allocation; it also prints `Budget`, `CurrentUsage`, `CurrentReservation`
  and `AvailableForReservation` per group and the raw segment statistics.
  After a measured result, `NVK_WINDOWS_BUDGET_GROUP=local` or `non-local`
  makes the heap report that group's `Budget` (bounded by the 512 MiB
  ceiling) and `CurrentUsage`. A zero `Budget` or a failed query keeps the
  estimate; `CurrentReservation` is never counted as usage. Without the
  variable nothing changes. `test-kmt-memory.exe` covers the query ABI,
  failure statuses, adapter-handle ownership and these rules with controlled
  substitutes. The physical measurement is pending.
  [Budget rules](../../src/nouveau/vulkan/nvkmd/windows/memory_budget.h) · [Segment flags](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/ns-d3dkmddi-_dxgk_segmentflags) · [RADV query](https://gitlab.freedesktop.org/lfrb/mesa/-/blob/801c9763c6043f0de8408e905a5324eea06d81d7/src/amd/vulkan/winsys/wddm2/radv_wddm2_winsys.c#L436) · [Windows budget fields](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_4/ns-dxgi1_4-dxgi_query_video_memory_info)
- **`VK_EXT_pageable_device_local_memory`.** The RADV branch provides
  examples of `MakeResident`, paging-fence waits and `Evict`. Implement and
  verify the extension on GM20B with `VK_EXT_memory_priority`, live allocation
  priority updates and VidMm paging under pressure before advertising it.
  [RADV residency](https://gitlab.freedesktop.org/lfrb/mesa/-/blob/801c9763c6043f0de8408e905a5324eea06d81d7/src/amd/vulkan/winsys/wddm2/radv_wddm2_bo.c#L849) · [Vulkan requirements](https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_pageable_device_local_memory.html)
- **`VK_KHR_maintenance5` and later extensions.** Keep NVK's shared
  maintenance5 implementation and validate it on Windows ARM64. Revisit
  RADV's Win32 external memory and semaphore paths and sparse VA binds when
  evaluating the corresponding extensions. Gate support on the actual
  GM20B capabilities and verify with targeted Vulkan CTS cases.
  [NVK features](../../src/nouveau/vulkan/nvk_physical_device.c) · [RADV external memory](https://gitlab.freedesktop.org/lfrb/mesa/-/commit/b286bc76ca5bddf68cd9962e98484d02b9c26b5d)

### Desktop, Direct3D and Zink

- **Native WDDM and DWM.** The RADV reference covers the user-mode Vulkan
  path over AMD's KMD. For the Tegra desktop, reuse the resource, fence and
  LUID patterns alongside the Windows DDI contracts. Mesa's `d3d10umd` is
  a software DDI example for `OpenAdapter10`, resource callbacks and
  DXGI present. GM20B needs hardware allocation, submission and present
  behind those entry points.
  [Mesa DDI example](../../src/gallium/targets/d3d10umd/README.md)
- **Direct3D 9, 10 and 11.** DXVK is pinned to `v3.1.1`
  (`b1a1c99ab52b687cf950d62c88bc2fa316b41663`). At that commit it requires
  Vulkan 1.3, `VK_EXT_depth_clip_enable`, `VK_KHR_maintenance5`,
  `VK_KHR_maintenance6`, `VK_KHR_load_store_op_none`, `VK_EXT_robustness2`
  and `VK_KHR_swapchain`; `VK_EXT_transform_feedback` and
  `VK_EXT_descriptor_buffer` are optional and enabled on NVK. Pre-Turing
  indirect draws, `DrawAuto`, transform-feedback counter loads, and
  `DispatchIndirect` use the validated `TG_GPU_SUBMIT_ABI_INDIRECT` (v2)
  GPFIFO submission path across NVK, KMT, and the GM20B KMD. Native ARM64
  DXVK and the ARM64 NVK ICD build with LLVM-MinGW; `x64`, `x86` (WOW64), and
  `ARM64EC` KMT and ICD export ABIs are implemented, while full `x64`/`x86`
  ICD DLL linking additionally requires the corresponding Rust `rust-std`
  targets for NAK (`arm64ec` also requires an ARM64EC CRT/import sysroot).
  Verify the features, indirect stages, VidMm budget group, and Win32
  swapchain on GM20B for each tested process architecture.
  [Requirements, indirect submission, multi-arch status, build and smoke test](DXVK_GAMES.md)
  `VK_KHR_swapchain_maintenance1` and
  `VK_EXT_attachment_feedback_loop_layout` are separate compatibility
  checks (`VK_KHR_present_wait` is disabled on Win32 GDI WSI). Compare the
  RADV WSI and synchronization paths for this work.
  [DXVK driver support](https://github.com/doitsujin/dxvk/wiki/Driver-support) · [NVK platform gate](../../src/nouveau/vulkan/nvk_physical_device.h)
- **Zink.** Run Mesa's `zink_check_requirements` against the live Windows
  NVK driver to identify the OpenGL levels it can support. Check window
  presentation separately using the same Win32 WSI work.
  [Zink profiles and checker](https://docs.mesa3d.org/drivers/zink.html)
- **Direct3D 12.** The reference branch's Dozen changes use D3D12 as a
  backend for Vulkan. For D3D12 on Vulkan, compare the target vkd3d-proton
  build against live descriptor indexing and update-after-bind limits,
  robustness2 and push descriptors. Check actual allocations within the
  512 MiB ceiling and verify resource sharing and present on Windows ARM64.
  [Dozen change](https://gitlab.freedesktop.org/lfrb/mesa/-/commit/db23c10d7b5509a7479f168b32396bdb293c6470) · [vkd3d-proton requirements](https://github.com/HansKristian-Work/vkd3d-proton#drivers)

## Switch Lite integration

Apply the [WinX GPU patch](../integration/switch-lite/winx-gpu.patch) to
WinX at the documented base. Link this checkout's `tegra-x1-wddm/`
directory into the WinX root as `TegraGpu/` so ACPI and the UEFI
diagnostic include the same source.
Start with the HDH-001 firmware build profile and add
`-D SWITCH_LITE_GPU=1`. Enable `-D SWITCH_LITE_GPU_IRQ=1` for the GPU
nonstall interrupt route. For the WDDM panel package also enable
`-D SWITCH_LITE_GPU_POST=1`, which assigns the POST framebuffer and
display controller to GPU0. Use a single ACPI owner for POST.
[Base and lineage](../UPSTREAM.md) · [WinX Lite build](https://github.com/ruje0504-2/winx/blob/hdh001-hekate-sd/docs/hdh001-home/README.md#firmware-build)

The Lite bridge reuses SwitchFan's kernel-owned SPB connection to the
MAX77812 M1 rail. `windows/pmic.h` defines the interface. Build the bundled
bridge with `--probe` if a compatible bridge is needed. Use the
[performance checker](../integration/switch-lite/performance-check/) to query
or run the diagnostic with a matching ABI. Run its `build.sh` from an output
directory and set `MINGW_SYSROOT`, `CLANG` and `BUILD_TESTS=1` to build the
tool and existing acceptance fixtures.

For another board, start with the GM20B code under `core/` and the Windows
adapter contracts in `windows/`. Supply the board's resources, power
provider, interrupt routing, authenticated firmware and display timing
through its platform layer. The T214 assumptions are under `platform/`
and `integration/switch-lite/`. The shared adapter, allocation and submit
headers carry the KMT ABI between KMD and NVK. The Lite's 512 MiB ceiling
is enforced over VidMm allocations from system memory.

For device trials, keep a tested display package and matching ACPI available
for rollback. Validate GPU execution with fences, exact readback and guards,
and check actual panel flips separately from Device Manager status.
