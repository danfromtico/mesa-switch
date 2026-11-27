# Standard shadow surfaces and virtual GDI Present

Work continues from .121 in the isolated `tegra-native-startup` worktree.
.130 reached visible native GDI scanout, confirmed by the user and bounded
pixel readback. DWM composition and continuous desktop refresh remain
broken. The timed rollback restored .59 Code 0, confirmed over SSH at
14:31:31 UTC on 2026-11-10. See [the concise result](NATIVE_STARTUP_RESULT.md).

## First retained failures

The .122 package preserves .121's behavior and adds durable, fixed journal
slots for the first paging, root, Present and scanout calls. Successful
interrupt toggles are deduplicated so they cannot consume the normal log.
PASSIVE_LEVEL records are written synchronously when special APCs are enabled;
the writer thread handles the other cases.

On hardware, .122 reached Code 0 and a 720x1280 active path on GM20B LUID
`90b35`. Shared-primary allocation, its page-table update and virtual fill
succeeded. The next standard allocation request was `SHADOWSURFACE`, which
returned `STATUS_NOT_SUPPORTED`. Cleanup then called `DISCARD_CONTENT`, also
unsupported. Windows subsequently restarted. This identifies the first
retained failed contracts, not a proven exception instruction.

.123 implements the fixed-size CPU-visible shadow descriptor and its pitch,
and accepts discard of an owned, uncompressed allocation without emitting
commands. The live adapter remained Code 0 from 11:48:40 through the last
sample at 11:53:48 UTC on 2026-11-10. Both active display-path LUIDs matched
the queried GM20B LUID `90c30`. Shadow creation succeeded and repeated Blt
Present calls replaced the immediate restart; those calls still returned
`STATUS_NOT_SUPPORTED` in .123. This is startup progress, not rendered frames.

The .123 rollback script initially retained a .122 package-match expression.
It was corrected, recovery was explicitly triggered, and .59 Code 0 plus
the baseline ACPI hash were verified after restart. Subsequent staging checks
validate the exact version expression, closed-gate status, signed-file hashes
and scheduled rollback action before admitting GPU initialization.

## Virtual Present ABI

.124 added a kernel-generated B0B5 pitch-linear copy and immutable command
capture, using the existing ordered worker and WFI/Reference completion.
The first Present failed input validation before a GPU submission. The
recorded source segment and destination address were inconsistent with GPU
VA inputs, pointing to an allocation-record layout mismatch.

The official WDK 10.0.26100 header defines
`DXGK_PRESENTALLOCATIONINFO` as a 32-byte ARM64 record, with the device-open
handle at offset 0 and `AllocationVirtualAddress` at offset 8. The legacy
`DXGK_ALLOCATIONLIST` is 24 bytes and puts its address at offset 16.

The exact installed `dxgkrnl.sys` has SHA256
`99d425e1c997eae32909fa4f37d6c8e2e846a5374c602295546c8be2547aa5e6`.
Its `DXGCONTEXT::SubmitPresentWithDmaBuffer` (RVA `140b98`) builds a
96-byte, three-element present array for a virtual context. The stores at
RVAs `140e24`/`140e5c` populate the source handle/VA at array offsets
`20`/`28`; `140e88`/`140eac` populate the destination at `40`/`48`.
The call at `140ef0` passes this array to the driver. Afterward, RVAs
`140ff4`–`141004` calculate private bytes used from the advanced private
data pointer. The source now consumes the typed present records and advances
both buffer pointers while preserving the input capacity fields.

This installed-code evidence conflicts with the public
[Present structure page](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/ns-d3dkmddi-_dxgkarg_present),
which still labels `pAllocationInfo` reserved. The implementation supports
the virtual GDI context it actually advertises; it does not guess between
layouts based on pointer values.

.125 confirms the corrected array interpretation: source VA `08010000`,
destination VA `00020000`, DMA VA `08000000`, all with segment zero as
expected for GPU VA. Code 0 and the active path on GM20B LUID `98e6e`
remain stable, but Present still returns `STATUS_INVALID_PARAMETER` before
submission. The .126 diagnostic package retains the exact rejecting source
line, descriptor format, handle-ownership check, internal VA reservation and
rectangles. It also permits the Present scheduler bit for a captured
kernel/GDI packet; application submission policy is unchanged.

.126 retained the exact rejection at the format check. Both device-open
handles belong to the GDI device and pass their range checks. The shadow
format is X8R8G8B8 (22); the primary is A8R8G8B8 (21). The source and
destination rectangles are valid and their VAs exclude the reserved
`04000000`–`08000000` interval. The new copy path preserves BGR and uses
B0B5 component remapping to write alpha 255 when converting XRGB to ARGB.
With remapping enabled, LINE_LENGTH_IN counts pixels rather than bytes.

.127 then returned Present success for a 68-byte packet and successfully
installed the GDI root. The first recovery record is now virtual submission
fence 14 with flags `2` (Present) and `STATUS_NOT_SUPPORTED`. Preemption and
TDR follow this rejection; there is no GPU-copy completion record. Windows
restarted and the zero permit restored the fallback display. The queue's
Present admission had depended on snapshot existence and a zero UMD-private
byte count. .128 instead resolves the registered GDI context, separately
validates/captures the KMD stream, and accepts a context-owned Present token.
The diagnostic records now include the first Present submission's DMA and
private byte counts.

.128 confirms that a real GDI Present arrives with 232 total private bytes,
232 reported UMD-private bytes, and 68 command bytes. The registered context
is GDI. Its fence 14 completed with GPU Reference marker 5; the worker's
proof mask is 7 (GPU completion, CPU readback, retired channel). The next
failed contract is MPO3: one enabled plane with `FlipImmediate`, identity
720x1280 rectangles, RGB color space 0, segment 3 and physical segment address
`fc98f000`. The active display and KMT probe agree on GM20B LUID `90d86`.
No native scanout latch was recorded because the driver rejected immediate
flips before programming the address.

.129 accepts that immediate MPO path at PASSIVE_LEVEL, polls the active
window bank before returning success, and requests a PASSIVE retry before
queuing if called at elevated IRQL. Address-only changes use HBlank;
visibility changes use VBlank. Busy requests return retry before new work
is queued. Display addresses are validated as segment base plus offset,
while PTE page-number resolution keeps its existing relative-offset contract.
Legacy VSync notifications likewise report the submitted physical address.
The GPU completion, MPO acceptance and native scanout latch have separate
fixed records. This trial retains rollback with a 12-minute boot delay to
allow sustained presentation observation if the first flip succeeds.

On hardware, .129 remained Code 0 with GM20B LUID `9303d`. The active
VidPN probe and KMT identity matched. GDI fence 14 completed with Reference
5 and proof mask 7, MPO3 returned success, and the active display bank
latched the owned scanout address `fc9c7000`. The completion proof includes
CPU readback of the fence, **not verification of copied pixels**.

The user nevertheless reported a frozen image with an intermittent spinner;
the photograph shows a mostly black screen and a white window outline.
An interactive WinForms test executed 40 timer ticks and 41 Paint callbacks,
with matching native LUID probes before and after, but its screen capture
was entirely black. These observations keep continuous drawing unverified.
The first test invocation had omitted the probe's required DLL argument and
did not run; the corrected second invocation produced the above evidence.

The initial ETL export retained only its last 600 seconds. A separate
3600-second export retains all 148480 events (SHA256
`7f5d9399076c39aae9410e753c55abec23e6d4d7e1ad2a9971806c21d8663d34`,
matched to the device). Context-to-adapter attribution and pixel contents
must be checked before interpreting ongoing DMA completions as correct
desktop rendering. The startup, first-copy and scanout milestones alone
do not satisfy the repair's acceptance criteria.

Application-log review identifies a separate failure: Dwminit repeatedly
reports DWM exits with `0x8898008d` while the primary display is Tegra X1.
The installed SDK names this `MILERR_NO_HARDWARE_DEVICE`; Microsoft's
[protocol error table](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpcr2/3f7d2f30-5aa8-46c5-8efe-25d531d001ff)
agrees that no rendering hardware is available. This is not evidence that
the CE fence or panel itself stopped. `windows/no_d3d.c` still rejects the
Direct3D adapter entry points, so full-WDDM startup must not be conflated
with availability of a DWM rendering device.

.130 retains .129's functional code and adds only test-package pixel
readback. The serialized worker samples the owned scanout allocation after
real GDI completion, at most once per second, with a fixed 256-record cap;
it writes neither pixels nor GPU fences. An interactive direct-GDI probe
compares rotating RGB bars with those samples, separately from DWM and
ordinary redirected windows. The one-shot gate and 12-minute rollback are
retained.

.130 reached Code 0 on GM20B LUID `8e7d4`; active source/target LUIDs
matched. The active window bank latched `fb9c7000`. A stable capture contains
37 pixel records at that address/LUID, with GDI completion fences 31 through
326. Exact `ffff0000`, `ff00ff00` and `ff0000ff` samples first appear at
fence 47; a later layout change appears at fence 267. The user confirmed
that the RGB bars were visible on the actual panel. This establishes a
real shadow → CE copy → owned scanout → internal-panel path.

It does not establish smooth desktop rendering. Both interactive tests
exceeded their scheduled time limits: the native test logged only frames
0–2, with about 17 seconds between the first two flushes. The first capture
contains 22 Dwminit exits with `8898008d`. A separate interactive DXGI test
enumerated the exact GM20B LUID and attempted `D3D11CreateDevice` with that
adapter and `D3D_DRIVER_TYPE_UNKNOWN`: result `887a0004`
(`DXGI_ERROR_UNSUPPORTED`). The same test created a WARP device successfully,
feature level `b000`. The no-Direct3D UMD remains a concrete composition
blocker, independent of Code 0 and native scanout success.

The .130 rollback restored the original .59 SYS and ACPI table hashes,
removed the start permit and test tasks, and rebooted back into Windows.
AC/DC VIDEOIDLE and STANDBYIDLE were each still zero. Firmware and the
512 MiB budget were not changed. The final functional source was rebuilt
from scratch; host models passed. The full-source Windows ARM64 fixtures
are recorded separately from the physically tested diagnostic package.

Mesa's existing `gallium/frontends/d3d10umd` is useful source material but
is not a ready DWM backend: its `OpenResource` reports an error and its
GDI target presents through `GetDC`/frontbuffer flushing. It must not be
substituted for the no-D3D DLL while claiming shared-resource composition.
Likewise, enabling presentation capability bits does not implement the
missing user-mode rendering contract.

## Implementation and references

The shadow and primary use the existing 32-byte private descriptor. Only
the primary is physically allocated from the scanout segment; the shadow
uses VidMm system memory. The 512 MiB allocation budget remains in force.

The Present path validates device-open allocations, fixed panel format,
unscaled clipped rectangles and GPU VA bounds. It emits one 2D B0B5 copy
per dirty rectangle and supports multipass DMA exhaustion. Only a registered
OS GDI context with the matching KMD-generated descriptor may capture this
kernel-built stream; a byte count is not used as its origin discriminator.
Normal application submission remains gated by the existing feature policy.
No CPU-written completion fence substitutes for GPU execution.

- Microsoft's [shadow-surface contract](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmdt/ns-d3dkmdt-_d3dkmdt_shadowsurfacedata)
  requires CPU drawing into the shadow followed by a Present Blt to/from the
  corresponding primary.
- The [Present callback contract](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/nc-d3dkmddi-dxgkddi_present)
  defines output DMA commands, clipped rectangles and multipass progress.
- Microsoft's graphics-driver-samples at `de4a2161991eda254013da6c18226f5ea06e4a9c`
  treat uncompressed discard as a no-op and keep execution in the scheduled
  worker. Its render-only sample uses physical patching; that layout is not
  the virtual-context Present array above.
- Mesa NVK's `src/nouveau/vulkan/nvk_cmd_copy.c` programs pitched source and
  destination addresses, row lengths/counts, multiline launch and flush.
  The implementation uses the existing NVIDIA B0B5 register definitions.
- lfrb/mesa `wddm2`, revision `801c9763c6043f0de8408e905a5324eea06d81d7`,
  uses the adapter LUID to create its D3D12 presentation device and requires
  WSI blits. That user-mode path depends on a working Windows presentation
  backend; AMD allocation/queue private data cannot be transplanted into
  this GM20B KMD. See [the reuse notes](PORTING.md#radv-windows-reference).
- NVIDIA's [Tegra display update path](https://github.com/OE4T/linux-tegra-4.9/blob/oe4t-patches-l4t-r32.7.4/nvidia/drivers/video/tegra/dc/window.c)
  selects HCounter for safe address-only immediate changes, falls back to
  VBlank for other state changes, and checks the window activation bits
  before treating an immediate update as complete. Only the hardware
  programming rules are reused here; no Linux implementation is imported.
- Microsoft's [MPO3 callback contract](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/nc-d3dkmddi-dxgkddi_setvidpnsourceaddresswithmultiplaneoverlay3)
  defines `STATUS_RETRY` with `PrePresentNeeded` for a recall at PASSIVE_LEVEL.

## Local evidence

Artifacts are retained under `/private/tmp/t214-gpu-round/render122-live/`
through `render129-live/`. Each completed collection has a
`verified-returned.json` manifest matching downloaded files against device
SHA256 and lengths. Build commands, signed packages, contract snapshots,
interactive LUID/path probes, ETL exports and model-test output are separate.
The Windows ARM64 fixture uses controlled OS/GPU substitutes and is not a
physical rendering pass.
