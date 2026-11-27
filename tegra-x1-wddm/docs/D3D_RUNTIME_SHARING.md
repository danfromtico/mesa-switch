# D3D runtime resource association

2026-11-10, Switch Lite, Windows 11 ARM64 23H2 (22631.2861).

The current desktop validation route is D3D UMD → Zink → NVK; DXVK is the
game-facing route. Desktop DXVK is deferred pending explicit user approval.
This investigation covers the Windows resource
association required by the desktop UMD. It does not establish shared-image
rendering, synchronization or DWM composition.

## Probe and controlled baseline

`nvk_d3d_runtime_check.exe --luid <LUID> --shared-contract` creates two
native D3D11 devices at feature level 10_0 and tests 64x64 BGRA textures.
It obtains/opens a legacy shared handle, releases the original texture and
checks the consumer descriptor. It then repeats with an NT/keyed-mutex
resource. Legacy handles are not closed as NT handles; the NT handle is
closed after opening. No drawing or copy command uses these textures.

The exact executable passes both cases with `--warp --shared-contract` on
the .59 baseline at 22:14:55 UTC, exit 0. This validates the application
probe against the Windows software implementation, not the Tegra driver.

The .145 diagnostic UMD retains the .144 Zink/NVK device path but intercepts
the shared-resource callbacks in the probe process. It allocates one raw
16 KiB KMT buffer through `pfnAllocateCb`, passing the real `hRTResource`;
opens the runtime-provided allocation and releases it through
`pfnDeallocateCb`. It also records `D3DKMTShareObjects` results. These
diagnostic resources have no Gallium/NVK image and must not be rendered to.
The diagnostic source lives in the artifact directory, not the production
`Resource.cpp`. The .144 ICD and KMD core are unchanged.
[Runtime creation/destruction contract](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/handling-resource-creation-and-destruction)

## .145 native observation

Code 0, matching KMT/active VidPN LUID `9edf6`, 512 MiB ceiling.
The probe runs 22:24:11–22:24:16 UTC, exits 1 and does not time out.

| Operation | Observation |
| --- | --- |
| Legacy allocation callback | `S_OK`; allocation handle `400019c0`; `hKMResource` remains zero in the output structure |
| Runtime legacy export | `GetSharedHandle` succeeds; handle `400002c2` |
| Consumer open callback | Resource `40001a00`, allocation `40001a40`, one allocation with 16 private bytes, zero resource-private bytes |
| Legacy resource release | Both deallocation callbacks return `S_OK`; the opened descriptor still matches after producer release |
| NT export attempt on the legacy resource | `D3DKMTShareObjects` returns `c000000d`; no NT handle is obtained |
| NT texture creation | `CreateTexture2D` with `SHARED_NTHANDLE | SHARED_KEYEDMUTEX` returns `80070057` (`E_INVALIDARG`) |

The zero creation-time `hKMResource`, later opened resource handle and
legacy global handle are distinct observations. Do not cast runtime opaque
handles or allocation handles into another handle class. The existing NVK
NT import cannot be connected merely by substituting one of these values.
[NT sharing requirements](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmthk/nf-d3dkmthk-d3dkmtshareobjects)
[NT texture creation requirements](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_2/nf-dxgi1_2-idxgiresource1-createsharedhandle)

Two diagnostic limits matter. The open callback printed a `va` field using
an `OPENALLOCATIONINFO2` view, but that extended layout was not established
for the negotiated D3D10 DDI. That field is discarded; it proves no GPU VA
mapping. The allocation handle/private-data fields have the SDK's common
prefix. Future instrumentation must use the negotiated structure layout.
Also, the create hook only intercepts the DDI shared flag. The lack of an
NT allocation-hook record does not prove that the runtime never entered
any UMD callback, or identify the callback responsible for `E_INVALIDARG`.
[Open-resource structure](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/ns-d3d10umddi-d3d10ddiarg_openresource)

Next, resolve the negotiated DDI/resource contract and the typed storage
bridge before binding a Zink image to runtime-owned memory. Validate GPU
contents and synchronization separately. .144 remains the native Vulkan
sharing and D3D rendering baseline.

## Recovery and artifacts

The 22:30:30 UTC check confirms .59 Code 0, original driver/ACPI hashes,
removed test tasks/permit and temporary ICD/UMD registration. Display and
sleep settings remain Never. Firmware/Hekate were not changed.

The 27 downloaded test records total 9,973,055 bytes and match every
device-side length/SHA-256. Five recovery records also match device hashes;
the before/after power reports are byte-identical. The ETL export contains
122,665 records in 9,574,862 bytes with matching count and record boundaries.
Artifacts and diagnostic source:
`/private/tmp/t214-gpu-round/runtime-sharing145._hkhc88e/`.

| Artifact | SHA-256 |
| --- | --- |
| Diagnostic .145 SYS | `5aa6c8788f265531140d7daff4274f611fd3af281f1a1fe425385adb68dcfb85` |
| Diagnostic UMD | `6f528daad49df78acf7c96a83f04eed07ac51890461ae4f11a0c3c8b4842cc86` |
| Runtime application probe | `fd7523c45d6bedda71acb10c36571b69789e9486734324b16f3226351b77f185` |
| Diagnostic Resource.cpp | `fa849818caac21fd3c02ba8c65a30f1c398354e4f5da82d066636ea67c063b91` |

## Resource metadata work

On 2026-11-11 the KMD create/open paths gain a resource-level storage
envelope. Both previously rejected all nonempty resource-private data.
`tg_gpu_resource_request` is a 32-byte, pointer-free header with version,
header length, allocation count, renderer-payload length and the sum of
page-rounded allocation sizes. Allocation creation validates it before
charging memory or allocating objects; the KMD retains the storage header.
The trailing renderer payload is opaque to the KMD and is not yet a defined
or implemented D3D image-layout contract.

Windows forwards the original resource-specific data to OpenAllocation.
The KMD now accepts matching metadata, requires the opened allocations to
belong to the same fixed allocation set, and preserves each device's
read/write access. Later allocation appends are rejected for these fixed
sets; resources without private data retain the original behavior.
[CreateAllocation resource data](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/ns-d3dkmddi-_dxgkarg_createallocation)
[OpenAllocation resource data](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/ns-d3dkmddi-_dxgkarg_openallocation)

The D3D runtime allocation handle cannot simply be used with NVK's separate
paging queue: MakeResident requires allocations to belong to the device
that owns that queue. Sharing/binding must preserve device ownership and
residency as well as the process GPU address space. The .145 legacy/NT
handle and negotiated DDI questions remain open.
[Residency device ownership](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dukmdt/ns-d3dukmdt-d3dddi_makeresident)

The standard KMD build passes the host models and builds the ARM64 library
and WDK fixtures. At 00:34:50 UTC, `test-context.exe` passes on the .59 Code 0
baseline with controlled OS/GPU substitutes. Coverage includes malformed
headers, incorrect rounded totals, create/open allocation failures, balanced
references, cross-device read-only access after another device closes, and
append rejection without changing existing allocations or budget charges.
Existing buffer, shared-open and context/queue tests also pass. This is not
a live installation or GPU execution result; the desktop image bridge and
DWM remain pending.

Executable SHA-256:
`2cc70b21bf3c9458d0f5511cbb7f5a3ec75d91eb716db4a3f090d9380e8cb730`.
The downloaded final report matches device-side length and SHA-256
`19b660447c0a226c0231b380379bd0ee0891db400c95c38244068bf0ff4b09ae`.
Artifacts, exact build command and source diff are retained under
`/Users/rujian/.local/share/mesa-switch-woa-build/runs/resource-metadata-20261111/`.

The former `/private/tmp/t214-gpu-round` host directory is no longer present.
The C/C++ toolchain and SDK/WDK were restored into
`/Users/rujian/.local/share/mesa-switch-woa-build/`: LLVM-MinGW 20261104 and
SDK/WDK NuGet 10.0.26100.6584 (Include 10.0.26100.0). Download origins/hashes
are recorded in `downloads-manifest.json`. Rust 1.90.0, Python dependencies
and the host `mesa_clc`/`vtn_bindgen2` have since been restored under that
same persistent root. Complete ARM64 UMD and NVK ICD builds pass. Five
rebuilt controlled Windows fixtures also pass on .59: UMD adapter, DLL
entry, device, KMT transport and shared-resource lifetime. These fixtures
use controlled substitutes and do not establish native presentation.
The two .144 native result JSON files and the .145 native result JSON were
recovered from the device into `recovered-evidence/`; all three match the
original verified hashes. This does not restore the former host build trees
or the .145 diagnostic source.

## Runtime primary bridge work

The 2026-11-11 working tree connects the D3D primary's allocation owner to
Zink/NVK. This addresses the explicitly unsupported SetDisplayMode boundary
observed after the SAMPLE fix; it is not yet a native presentation result.

`RuntimeMemory.cpp` allocates through the runtime's callback with the real
`hRTResource`, `Primary` flag and VidPN source. It retains the returned
allocation handle separately from the optional kernel resource handle.
Residency, paging-fence waits and GPU/CPU mappings use that same device's
callbacks and paging queue. No runtime opaque handle is cast to a KMT device
or NT shared-resource handle. The implementation currently targets the KMD's
existing 720x1280 BGRA primary, 2880-byte pitch and 3,686,400-byte allocation.
The 16 MiB scanout segment and overall 512 MiB ceiling are unchanged.
[Resource association](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dumddi/ns-d3dumddi-_d3dddicb_allocate)
[Primary description](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/dxgiddi/ns-dxgiddi-dxgi_ddi_primary_desc)

Zink passes a typed, Mesa-internal owner description through the existing
WSI allocation chain. NVK checks its adapter LUID, size, access and callbacks,
retains the owner, and maps this allocation into its GPUVA reservation.
The owner completes its paging fence before returning success. Process-wide
GPUVA makes the allocation addressable by the NVK context without sending
the runtime's allocation to an unrelated device's paging queue. An explicit
import acknowledgement prevents an older/different ICD from silently
allocating unrelated memory. This is not a new public Vulkan external-handle
type; it cannot be exported as OPAQUE_WIN32 memory.
[GPUVA process scope](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/gpu-virtual-memory-in-wddm-2-0)
[Mapping and paging completion](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dukmdt/ns-d3dukmdt-d3dddi_mapgpuvirtualaddress)

For the explicit native pitch, NVK uses its existing tiled-shadow rendering
and GPU resolve implementation for linear images. The internal image query
does not change the public linear-image format capabilities or advertise a
DRM extension on Windows. There is no CPU pixel-copy presentation path in
this change. The UMD waits for rendering before calling `pfnSetDisplayModeCb`
with the actual primary allocation and returns its HRESULT unchanged.
Other modes, format conversion and rotation remain work beyond this fixed
primary layout; Present and shared-resource opens are also still unfinished.

Resource destruction waits for GPU use, releases the complete GPUVA
reservations, and then deallocates through the runtime resource handle.
The owner context survives delayed Zink/NVK object cleanup; subsequent frees
of already-released ranges are no-ops and further mapping requests fail.
This ordering matters because runtime deallocation automatically releases
associated GPUVAs and those addresses can be reused immediately.
[Free/deallocate ordering](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dumddi/nc-d3dumddi-pfnd3dddi_freegpuvirtualaddresscb)

Both ARM64 builds pass. On .59 Code 0 at 10:18:44 UTC, the new controlled
primary fixture passes ownership, allocation flags, exact byte count,
residency/map waits, failure cleanup, mode HRESULT propagation and delayed
cleanup without a second VA free. Existing UMD adapter, device and DLL-entry
controls also pass; their finalized ETL exports 46 valid events with no
recorded loss. These callbacks are substitutes, not real GPU allocations.
The candidate `runtime-primary-auto-20261111` reused the signed .144 miniport.
UMD SHA-256:
`78d45f3d4352c37da1ca6780a5482ffe3fe9fd9cd844506afed4c88c70a01cb9`;
ICD: `f828845c38b7bfb6e34ef83beb1c873009c2f89afcd014090a82ec4f926a2f30`.

On native LUID `9750b`, DWM creates fourteen real runtime primary allocations
of 3,686,400 bytes, all successfully. Each `MakeResident` then returns
`8000000a` (`E_PENDING`), with paging fence `1b59`. The first implementation
incorrectly treated the HRESULT failure bit as a terminal error and
deallocated each primary without waiting. It therefore did not reach the
display-mode callback. The finalized ETL has 57,369 valid events, with no
recorded loss. All 131 returned files (44,406,284 bytes) match device hashes.
.59 Code 0 and unchanged power settings were verified at 10:29:21 UTC;
portrait restoration was a no-op at 10:29:36 UTC.

`MakeResidentCb` and `MapGpuVirtualAddressCb` explicitly permit `E_PENDING`:
the operation has been queued, and the returned paging fence must complete
before GPU access. The implementation now handles this status only for
these paging callbacks, preserving their raw trace values and propagating
a failed wait. This does not turn arbitrary failed HRESULTs into success.
The extended ARM64 fixture passes queued residency/mapping and failed-wait
cases on .59 at 10:41:09 UTC; existing UMD controls export 46 valid events.
[Residency return values](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dumddi/nc-d3dumddi-pfnd3dddi_makeresidentcb)
[Mapping return values](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dumddi/nc-d3dumddi-pfnd3dddi_mapgpuvirtualaddresscb)

The same native trial matches 16,384 ordinary clear/triangle pixels, but its
explicit device release faults with `c0000005`. The matching UMD's PC RVA
`112f18` reads a freed `curr_program->last_variant_hash` during shader unbind;
LR `111530` is in `zink_bind_fs_state`. Program pruning now clears the
context's non-owning selected-program pointer and its hash contribution
before releasing references. Native sampling separately reports 2,940
mismatches in 4,096 pixels, a pattern consistent with clamp addressing
instead of the requested wrap. This does not yet distinguish an incorrect
sampler binding from a missing/default state. The next probe runs all three
sampler/palette cases and retains its SPIR-V.

`runtime-paging-auto-20261111` tests the paging and lifetime fixes with UMD
artifact `runtime-primary-20261111-umd+d17a557b3a76` and the same ICD above.
Its updated probe passes the WARP reference (24,576 pixels); native results
are pending. Its first automatic attempt observes Code 0 and a consumed
start permit while `HardwareStatus` is still `ffffffff`, then prematurely
starts rollback before the drawing probes. `runtime-paging-wait-auto-20261111`
adds a bounded wait for a terminal hardware status while retaining that
first snapshot; it uses the same UMD/ICD binaries. The harness records each
probe's exit status separately and recognizes negative Windows exception
codes as failures.

The recovered first attempt nevertheless establishes a real DWM primary
bridge pass on LUID `98df6`: 116 successful primary allocations, 116 queued
residency operations, 116 queued GPUVA maps and 232 successful paging waits.
The actual runtime `SetDisplayMode` callback returns S_OK 115 times, with
the real primary allocation handle. DWM has 479 successful resource creations
and no recorded runtime-error callback in this finalized 12,816-event trace
(no recorded event/buffer loss). Repeated primary creation/mode setting is
not evidence of sustained Present or a visible desktop. The drawing probes
were not launched because of the early harness check. All 120 returned files
(36,355,064 bytes) match device hashes. .59 Code 0 and environment/power
restoration pass at 10:48:35 UTC; portrait verification is a no-op at
10:48:49 UTC.

The bounded-wait run reaches native LUID `954c5` at 10:54:18–10:54:40 UTC.
Ordinary RGBA/BGRA drawing matches all 16,384 pixels and completes context,
device and adapter release with exit 0. The sampler probe exits 1 without
timeout: its wrap/clamp/rebound-texture cases have 2,940/0/2,940 mismatches.
This is inconsistent with simply selecting the deliberately swapped `s5`.
The retained finalized circular ETL contains 14,432 valid events, including
140 successful SetDisplayMode callbacks and no recorded DWM runtime error.
Circular retention does not establish a complete startup history.
All 139 returned files (36,812,705 bytes) match device hashes. .59 and cleanup
pass at 10:58:29 UTC; portrait verification is a no-op at 10:58:44 UTC.

Six returned SPIR-V files identify the sampler bug: the fragment shader
correctly loads the independent texture and sampler, but the sampler's
Binding decoration is 2, the original Gallium slot. `emit_sampler` used
`driver_location` instead of the Vulkan `binding` assigned by Zink, unlike
image emission. The next candidate uses the assigned binding and adds a
complete NIR-to-SPIR-V control covering remapped binding 450, binding zero,
and the unchanged-slot case. The old fragment SPIR-V `sample-dump05.spv`
SHA-256 is `fb513b6a16cee202daa5602d0a36f4e3836e5a31b16bdb124b1c7f255441a462`.
`sampler-binding-auto-20261111` tests UMD artifact
`runtime-primary-20261111-umd+826085f423ab` with the same ICD and signed KMD.
On .59 at 11:00:40 UTC, the new regression fixture reproduces the original
generator's binding 2 instead of 450 and exits 1; the patched generator passes
all three cases and exits 0. WARP reference sampling again matches 24,576
pixels. These are compiler/CPU controls.

The patched native trial on LUID `92166` runs at 11:05:01–11:05:09 UTC.
Ordinary clear/triangle drawing matches 16,384 RGBA/BGRA pixels; independent
`t5/s2` sampling matches all 24,576 pixels across wrap, clamp and texture
rebinding in both render-target formats. Both processes report S_OK device
status, complete explicit teardown and exit 0 without timeout. This validates
the sampler binding fix on GM20B. The returned fragment SPIR-V now uses
Binding 451 for the separate precompile and Binding 450 for the linked
program, matching Zink's remapping. The linked fragment's SHA-256 is
`96ffd0ccb1d67d2df0bd5adc1a328ba430348e5943a0ee92911ac574604549e4`.

The retained finalized circular ETL contains 13,760 valid UMD events and no
recorded event/buffer loss, including 141 S_OK SetDisplayMode callbacks. DWM
continues recreating primary surfaces; the retained window has no Present,
Blt or runtime-error records. This does not establish their absence outside
that window or identify the first failure after mode setting. Desktop capture
is still black despite the confirmed Default input desktop, 720x1280 mode,
identity rotation, (0,0) origin and 96 DPI. Visible desktop composition and
sustained presentation are not validated. The next boundary is the repeated
mode/resource cycle and actual Present/shared-image contracts.

All 144 returned files (36,655,102 bytes) match device hashes in
`runs/sampler-binding-auto-20261111/evidence/verified.json` under the external
build root. .59 Code 0, original driver/ACPI hashes, temporary registration,
loader/environment/task/permit cleanup and unchanged power policy pass at
11:09:12 UTC. Portrait verification is a no-op at 11:09:27 UTC.

ETW numeric event 29 is `RuntimeMemory`. The high 32 bits of `arg0` identify
the operation; the low 32 bits contain the allocation handle when available.
`arg1` is the byte count, paging-fence value or paging-queue handle, as relevant.

| Stage | Operation |
| --- | --- |
| 1 | Allocate runtime primary |
| 2 | Make resident |
| 3 | Wait for paging completion |
| 4 | Map GPUVA |
| 5 | Free GPUVA reservation |
| 6 | Deallocate runtime resource |
| 7 / 8 | CPU lock / unlock |
| 9 / 10 | Create / destroy paging queue |

## Runtime context registration

The full retained kernel stream from `sampler-binding-auto-20261111` was
reviewed after the UMD-only export. The existing native exporter produces
252,932 complete records, including 239,158 DxgKrnl events, with an exact
count/length match. Every DxgKrnl payload decodes to the installed provider's
matching ID/version template without unknown layouts or trailing bytes.
The raw export and provider metadata match device hashes. This remains a
circular trace, not a complete boot history. Analysis artifacts are under
`runs/dwm-mode-analysis-20261111/` in the external build root.

DWM PID 460/TID 1600 records 142 `AzureTriage` events saying that a signal
requires at least one broadcast context. They occur during the repeating
mode/resource cycle, 7–229 ms after the preceding retained successful UMD
SetDisplayMode. The source never registered a context with the D3D runtime:
its NVK queue owns a separate KMT device/context. This is a concrete contract
gap and a stronger lead than the 562 unsupported IOCTL results. Those IOCTL
records have a zero control code in their captured stack locations and do
not establish the cause of the DWM loop. Adapter pointers also differ across
the visibility/flush/history records; do not attribute every kernel Present
event to the Tegra rendering path.

The working tree now creates a virtual-addressing context through
`pfnCreateContextVirtualCb` on the real runtime device and destroys it through
the matching callback. Creating the UMD device fails with the original
HRESULT if registration fails. No NVK KMT handle is substituted for a runtime
handle. Until the two owners share a GPU timeline, the UMD's Flush waits for
its Zink/NVK fence before the runtime may signal its separate context; device
destruction drains the GPU before destroying that runtime context. This is
a synchronous correctness bridge with a performance cost, not a claim that
Present or shared-image integration is finished.
[Virtual context callback](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dumddi/nc-d3dumddi-pfnd3dddi_createcontextvirtualcb)
[D3D10 Flush contract](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/nc-d3d10umddi-pfnd3d10ddi_flush)
[DXGI Present context ownership](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/dxgiddi/ns-dxgiddi-dxgiddicb_present)

ARM64 compilation passes. The controlled device fixture at 11:31:00 UTC
checks callback ownership, failed-create cleanup, GPU fence waits and failed
wait propagation, as well as existing rendering/resource cleanup. The other
primary/adapter/entry controls pass; their finalized ETL has 57 valid events
without recorded loss. Native candidate `runtime-context-auto-20261111`
uses UMD artifact `runtime-primary-20261111-umd+41f76160496e`, the same ICD
and signed .144 KMD.

That first trial reaches Code 0/native LUID `9dd9e` at 11:35:11 UTC but does
not run either pixel probe. The interactive observer fails to publish its
fresh JSON because the automation's read temporarily denies writes. Its
separately completed display-path records show the correct native identity;
the gate correctly refuses the stale Code 43 snapshot. The next harness
uses a shared read and waits for the fresh snapshot before opening its path
log. This is a harness fix, not a changed GPU binary or relaxed identity gate.

The retained kernel stream has 252,348 complete records; all 245,967 DxgKrnl
payloads decode against the installed templates. DWM PID 764 records 316
`SignalSynchronizationObjectFromGpu` events with `ContextCount=1`, non-null
contexts and advancing fence values. Its UMD GPU-completion records return
S_OK. The old empty-broadcast diagnostic is absent from this retained window,
but the finalized UMD export reports two lost events, and the trace is
circular. These are positive synchronization observations, not proof of
complete startup coverage or a desktop fix. Primary/mode recreation remains.
All 120 returned files (56,354,345 bytes) match device hashes; .59 Code 0,
cleanup, portrait and unchanged power policy are verified at 11:39 UTC.

`runtime-context-shared-auto-20261111` reuses those exact binaries and their
verified controls. In addition to the reader fix, it records DXGI API return
values and DWM composition/scheduling at informational level using templates
read from the device.

On native LUID `9b7ff`, the probes run at 11:54:21.950–11:54:41.669 UTC.
All 16,384 ordinary clear/triangle pixels and 24,576 independent `t5/s2`
sampling pixels match. Both explicitly release their context/device, return
S_OK from GetDeviceRemovedReason and exit 0 without timeout. Their runtime
context creation and destruction callbacks succeed. DWM PID 488 records 401
successful GPU-completion waits and 134 successful SetDisplayMode callbacks;
261 kernel GPU-signal events have `ContextCount=1` with non-null contexts and
advancing fence values. No empty-broadcast diagnostic is retained. The ETL
is finalized with 13,878 valid UMD events and no reported event/buffer loss.

The full export has 253,480 records, including 228,484 DxgKrnl, 7,728 DXGI
and 3,370 Dwm-Core events. All three providers decode with exact payload
lengths against this OS's templates, including nested DWM structures. This
still covers the retained circular window, not the entire startup.

### Fullscreen transition boundary

DWM PID 488/TID 1900 attempts SetFullscreenState(TRUE) 133 times. Both the
DXGI operation's return event and its API return event report `887a0025`,
`DXGI_ERROR_MODE_CHANGE_IN_PROGRESS`, on every retained complete pair. This
is the error value, not the different success-severity status `087a0008`.
Each attempt's output remains windowed, with null primary/output pointers;
the corresponding swapchain is then destroyed and recreated. The UMD's
preceding SetDisplayMode returns S_OK; the later DXGI return follows it by
3.2–161.6 ms (median 14.0 ms). Its first journal error appears immediately
after that DDI return. The error identifies an incomplete transition but
does not establish which component causes it.
[Microsoft error-code definition](https://learn.microsoft.com/en-us/windows/win32/com/com-error-codes-10)
[SetFullscreenState contract](https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgiswapchain-setfullscreenstate)

The swapchain requests three 720x1280 BGRA buffers, 60/1 Hz, sample count 1,
and swap effect 1. There is no retained UMD Present/Blt/OpenResource call or
runtime-error callback. Those absences do not prove what happened before
the circular window. Two retained DWM HWDEVICE creation events identify
Microsoft Basic Render Driver LUID `547d`/FL12_1; they must not be reported
as Tegra feature-level results. The native D3D probes remain FL10_0.

Capture is still black on the native `Default` desktop, with matched native
adapter, origin (0,0), 96 DPI, portrait 720x1280 and identity rotation.
Visible DWM composition and sustained Present are not established. The next
investigation is transition completion and the primary/presentation contract,
not an unsupported feature-level increase or speculative scanout-register
change.

Read-only inspection of the device's `d3d11.dll` 10.0.22621.2715 clarifies
the apparently successful DDI. `NDXGI::CDevice::SetDisplayModeCB` stores both
the actual D3DKMT NTSTATUS and a mapped HRESULT in its per-call state, then
returns S_OK for selected mapped errors, including `887a0025`. The outer
SetDisplayMode operation consumes that saved result after the UMD returns.
Consequently, a successful callback alone cannot establish that the kernel
mode change succeeded. The mapping includes invalid parameters and unsupported
VidPN modality as well as genuine present-mode/ownership changes; the generic
DXGI code cannot distinguish them. Its kernel-call wrapper records failures
in the D3D11 journal, which was not enabled in this trial.

The DLL's SHA-256 is
`ccc352f7097d0320368000dbe9ff92772d8fc909185d95c8a0753cb8f115f880`.
The public symbol GUID is `4DD64C1C-37D9-00CB-45AF-89C52B2A1534`; the image
requests age 1 and the stripped public PDB reports age 2. Relevant native
callback instructions are at RVA `548a0`–`54b74`; the status-save/masking
sequence is at `54b1c`–`54b40`. This static inspection motivated the following
capture; it does not by itself select one of the possible kernel statuses.

### Raw mode-setting status

`mode-journal-auto-20261111` enables the installed Direct3D11 provider's
JournalEntries keyword (`0x800`, event 1820) with unchanged UMD/ICD/KMD
binaries. A 12-second DWM observation on native LUID `953d1` runs at
12:29:40.175–12:29:52.929 UTC. It does not rerun the pixel probes or establish
a new drawing/presentation pass.

The retained D3D11 journal contains **144 SetDisplayMode failures from DWM
PID 812/TID 1864**, all `c01e0005`
(`STATUS_GRAPHICS_PRESENT_MODE_CHANGED`). Each precedes that thread's UMD
SetDisplayMode S_OK record by 9.5–20.8 microseconds (median 10.7). DXGI reports
144 corresponding fullscreen failures with `887a0025`. This confirms the
runtime's status conversion in the observed path. These specific failures
are neither OOM nor invalid-parameter results. Their cause within mode/
primary compatibility is still to be established; do not infer a particular
rotation, format or timing field from the generic mode-change code alone.

The other 15 D3D11 journal entries belong to different process IDs and cover
UMD access/interface selection. They are not DWM's mode-setting failure.
The next check is the actual primary description and active VidPN state,
including the meaning of PreserveVidPn. The existing source's allocation
description and mode/rotation fields require comparison to that state before
changing behavior.
[SetDisplayMode input contract](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmthk/ns-d3dkmthk-_d3dkmt_setdisplaymode)
[PreserveVidPn flag](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmthk/ns-d3dkmthk-_d3dkmt_setdisplaymode_flags)

The raw export has 250,385 complete records, including 229,940 DxgKrnl,
7,984 DXGI, 3,582 Dwm-Core and 159 Direct3D11 events. All four providers
decode exactly against the device templates; no unknown or malformed
payloads remain. The finalized UMD export has 8,712 valid events with no
reported loss. All 132 returned files (57,562,355 bytes) match device hashes.
Raw-export SHA-256:
`9f51635babac995098db73d39330ebf8d6251b92cbf43281e71479d466df3dbe`.
.59 Code 0 and complete cleanup are verified at 12:33:15 UTC after the
recovery reboot. Portrait restore/verification at 12:33:29 UTC is a no-op,
and the power reports are unchanged. Artifacts are in the external build
root's `runs/mode-journal-auto-20261111/` directory.

For the preceding runtime-context drawing trial, all 145 returned files
(58,992,567 bytes) match device lengths and SHA-256.
The raw event export hashes to
`de5210fd632314db473658c25a6c50915cdf47a7608ff80826b02089d18b868a`.
The automatic rollback is verified at 11:58:44 UTC: .59 Code 0, original
driver/ACPI hashes, cleared temporary tasks/registration/environment and
unchanged power policy. Portrait restore/verification at 11:59:25 UTC is a
no-op. Evidence and decoding artifacts are in the external build root's
`runs/runtime-context-shared-auto-20261111/` directory.

Numeric event 30, `RuntimeContext`, uses `arg0` stages 1=create, 2=destroy,
3=GPU completion. `arg1` records a non-null returned context or completed
fence as a boolean, not an opaque handle value; the HRESULT is preserved.

### Allocation rotation compatibility

Read-only inspection of this device's `dxgkrnl.sys` 10.0.22621.2792 locates
an explicit rotation check inside `DXGDEVICE::SetDisplayMode`. Before calling
DescribeAllocation, RVA `1494ac` initializes `CheckDisplayMode=1` and
`Rotation=UINT32_MAX`. A returned rotation other than that sentinel is
compared to the active mode at RVA `269ab4`; a mismatch reaches the
`c01e0005` return at `269b04`. This is a concrete path to the observed error,
not proof that every occurrence takes this branch. The kernel image hashes
to `99d425e1c997eae32909fa4f37d6c8e2e846a5374c602295546c8be2547aa5e6`;
its public symbol GUID is `797F457A-6488-C67C-8E55-B0F3E771DC32`.

Our old whole-structure assignment returns Rotation=0, while all current
image layouts are unrotated and the active portrait mode reports identity
(1). The full 180-byte old function is byte-identical to a sequence at file
offset 73180 in the signed `.144` SYS, SHA-256
`e9cc6c5e576afd6861047983567eb2b4628209a5f227cdadc3ef48e86d59144c`. The
fix explicitly reports `D3DDDI_ROTATION_IDENTITY`; it neither rotates pixels
nor advertises a new orientation. The public DescribeAllocation page calls
Rotation reserved, so the observed OS behavior must be distinguished from
that documentation, rather than attributed to it.
[Allocation description](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/ns-d3dkmddi-_dxgkarg_describeallocation)
[Rotation values](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dukmdt/ne-d3dukmdt-_d3dddi_rotation)

The real-WDK ARM64 fixture seeds the same sentinel and checks the expected
identity result. The old implementation exits 675 at the new assertion
(12:54:10 UTC); the fixed implementation exits 0 (13:01:39 UTC). Both run
on `.59` with controlled kernel substitutes and no native GPU execution.
The source-owned native startup gate's eleven cases also pass at 13:18:21
UTC. All six returned control logs/JSON files match device SHA-256.
The controls and binary analysis are retained externally under
`runs/describe-rotation-control-20261111/` and
`runs/dwm-mode-analysis-20261111/`. A newly signed `.146` package, rebuilt
from source with the documented gate, is the native validation candidate;
the controls alone do not establish a desktop or hardware rotation pass.

### Native identity-rotation trial

The first `.146` controller attempt aborts before arming the GPU because its
new directory lacks two capture utilities. No native startup is claimed for
that attempt. It returns to `.59` Code 0 with the original display/power
state, and all 99 returned files match device hashes. The corrected preflight
checks those utilities before installing the unchanged signed package.

`describe-rotation-native-v2-20261111` uses the source-built `.146` KMD,
SHA-256 `2b88feb19ec679ebc80caa16b0249222ed633ed8b29654705df7d56934ca55d6`,
with the previously validated `41f76160496e` UMD and `f828845c38b7` ICD.
This also replaces the lost temporary entry wrapper with the tracked native
gate; it is not a binary-only single-field A/B experiment. Hardware startup
completes with Code 0 and a consumed permit. On LUID `92611`, the probes run
at 13:37:06.926–13:37:27.406 UTC: all 16,384 clear/draw pixels and 24,576
sampling pixels match, both devices explicitly release and both tests exit
0 without timeout or a removed-device error.

In the retained 13:37:05.364–13:37:33.911 UTC window, DWM PID 812 creates
15 devices identified as **Tegra X1 (GM20B), LUID `92611`, FL10_0**. All 14
complete fullscreen transitions return S_OK, set `NewWindowed=0`, and report
a non-null primary. The journal contains no SetDisplayMode failure in that
window. These positive DXGI results establish passage through the mode-setting
boundary; they go beyond the UMD callback's previously masked success.

The next concrete failure is the existing hardware `_Present` stub in
`DxgiFns.cpp`: all 14 DWM calls return `887a0004` (unsupported). The runtime
records 14 driver-internal-error removals (`887a0020`), and the outer DXGI
Present returns `887a0005`. Feature-level negotiation warnings precede
successful FL10_0 device creation and must not be substituted for this
presentation failure. Wire the real runtime presentation callback after
finishing GPU work and preserving allocation/context ownership. Runtime
shared-image integration is also still incomplete.

The 13:37:27 capture is no longer black: it contains the Windows wallpaper
and desktop icons. Its immediately preceding visibility record identifies
the native ACPI adapter, `Default` input desktop, origin (0,0), 96 DPI,
720x1280 source/signal and identity rotation. Capture SHA-256 is
`d98de373862ee54a5408a3edb39cd3e4b58a56c61ea9377090c2841dbd8e2394`.
This is a desktop capture, not a physical panel photograph, a verified
hardware-composited frame, or evidence of sustained successful Present.
Hardware rotation and final 1280x720 landscape remain to be implemented.

The raw export contains 235,099 complete records: 176,364 DxgKrnl, 7,625
DXGI, 1,250 Dwm-Core, 46 Direct3D11, 49,812 UMD and two trace headers.
All four OS providers decode with exact payload lengths and no unknown
events. Raw-export SHA-256 is
`c2147e5828e78ab172c1bd40b1945d74b953bfe7a33f5283036bfff0ad650a87`.
The original exporter summary files are filled with zero bytes. A read-only re-export
from the retained ETL produces the identical 49,812-event UMD JSONL hash
`7c2fdbdcead5af166f70ec6cd9e5b7637109825b83551547ab4e790f5cb71cb8`:
ProcessTrace=0, invalid=0, events/buffers lost=0, nonzero EndTime and 2,444
buffers written. This validates finalization of the retained circular trace,
not coverage of the entire startup.

A further Windows boot at 13:38:58 is observed before a durable completion
record from collection/restoration. The record does not establish its cause.
The consumed permit keeps the restarted `.146` closed; the subsequent
recovery restores `.59` Code 0, original driver/table hashes, cleared test
registration/tasks/environment and unchanged power policy at 13:43:47 UTC.
Portrait restore/verification at 13:44:09 is a no-op. All 149 returned files
(76,015,725 bytes, including the re-export) match device SHA-256. Evidence,
strict decoding and summaries are under the external build root's
`runs/describe-rotation-native-v2-20261111/` directory.

### Runtime Present and swapchain identities

The new UMD routes native primary flips through `pfnPresentCbDXGI`. It
passes the allocation returned by the runtime, the registered runtime
context and the original opaque DXGI context. Before submitting, it resolves
the Zink primary's tiled shadow and waits for NVK GPU completion. Callback
HRESULTs, including positive statuses, are returned unchanged. A missing
destination means its subresource index is unused. This connects the current
mono primary-flip path; UMD blt presentation and shared-image opens are still
separate unfinished paths.
[DXGI presentation callback](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/dxgiddi/nc-dxgiddi-pfnddxgiddi_presentcb)
[Callback allocation/context ownership](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/dxgiddi/ns-dxgiddi-dxgiddicb_present)

Hardware swapchain identity rotation now moves the actual Gallium storage
and its runtime allocation owner instead of copying pixels. Runtime resource
handles remain in their original logical slots. RTV/DSV descriptions and
sampler views follow the new storage, and current framebuffer/sampler
bindings are updated. Replacement sampler views are prepared before mutating
identities; a failed allocation or GPU wait leaves the previous identities
and bindings intact. This is buffer-identity rotation, not screen rotation.
[Resource identity and view contract](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/dxgiddi/ns-dxgiddi-dxgi_ddi_base_functions)

The exact candidate is `runtime-present-20261111-umd+79fa8169fb00`, built
from `d6635e3d`; its DLL SHA-256 is recorded in the external build manifest.
Five ARM64 controlled fixtures pass on `.59` at 14:28:25 UTC. They cover
actual frontend Present/Rotate/view functions with substitute runtime and
Gallium owners: opaque context forwarding, allocation ownership, GPU-before-
Present ordering, raw success/error/positive callback results, four rotations
of three resources, bound RTV/SRV updates, partial view-allocation failure,
failed completion and teardown through unchanged runtime resource handles.
The finalized control trace has 111 events, no invalid payloads or reported
loss, and explicit successful/failed Present and RotateIdentities events.
These are controlled API tests, not a native presentation pass.

Trace event 31, `RotateIdentities`, records the final HRESULT and resource
count. `Present` records the final HRESULT, DDI flags in `arg0` and flip
interval in `arg1`. The matching exporter names the new event. Native trial
collection now flushes exporter outputs to disk, and a controller invocation
after a reboot preserves the first attempt instead of overwriting its error.
The native run reuses the signed `.146` KMD and `f828845c38b7` ICD.

### Runtime Present native trial

`runtime-present-v2-auto-20261111` starts the unchanged `.146` KMD with the
`79fa8169fb00` UMD and `f828845c38b7` ICD. Hardware startup returns success
and the adapter reaches Code 0 on LUID `94aa5`. The ordinary and independent
sampling probes run at 14:35:20.335–14:35:27.335 UTC; all 40,960 pixels match,
both devices explicitly release, and both executables exit 0. Their success
record precedes the separate desktop capture and is not a complete trial pass.

DWM PID 440 / TID 1700 completes three retained fullscreen transitions with
S_OK and non-null primary allocations. Two retained HWDEVICE creation events
identify Tegra X1, LUID `94aa5`, FL10_0. Each of the three following flips
has `MMIOFlip=1`, `FlipWithNoWait=1`, interval 1 and a null DMA buffer.
The corresponding kernel Present event returns `c00000bb`
(`STATUS_NOT_SUPPORTED`). The D3D11 journal preserves that status, then
records `PresentCB=80004005`, matching the UMD's returned HRESULT. Device
removal (`887a0020` internally, `887a0005` from outer DXGI Present) follows.
These are actual calls through the new runtime bridge, replacing the old
`887a0004` UMD stub failure. Other redirected presents on a different thread
and context do not establish success of these Tegra primary flips.

The user reports physical black output and confirms that the desktop returns
after rollback. No native desktop PNG is retained. The input-desktop records
identify `Default`, the native ACPI display, 720x1280 identity mode and origin
(0,0). Neither those records nor the successful offscreen pixel comparisons
establish visible composition or hardware landscape rotation.

The trace is finalized: ProcessTrace=0, 20,004 valid UMD events, no reported
event/buffer loss, nonzero EndTime and 973 buffers written. The raw export
contains 103,487 records: 80,001 DxgKrnl, 3,122 DXGI, 318 Dwm-Core, 40
Direct3D11, 20,004 UMD and two headers. All four OS providers decode exactly
against the device templates, with no unknown or malformed events.
Raw-export SHA-256:
`d308619328fb380dfa31e4288f87029224b58360b73990139f98f3192f0a3585`.
The separate loader receiver reaches its 150-second limit with only one of
two control markers, so its validation fails; this does not invalidate the
independently finalized ETL. The controller collects the trace and rolls back.

At 14:41:10 UTC, `.59` Code 0, original driver/table hashes, removed temporary
registration/tasks/environment and unchanged power reports are verified.
Portrait restore/verification at 14:41:24 is a no-op. All 154 returned files
(48,364,096 bytes) match device SHA-256 and lengths. Evidence and strict
decoding are retained under the external build root's
`runs/runtime-present-v2-auto-20261111/` directory.

The KMD's no-DMA flip filter omitted `FlipWithNoWait`; the source now accepts
it without emitting GPU commands or waiting for a flip. The actual scanout
change remains owned by the scheduled panel DDIs. Rotation, blt, stereo and
DMA-bearing requests are not admitted by this change. The Windows flag
describes a tear-free flip that does not stall the graphics pipeline, not
permission to tear or to claim a completed physical latch.
[Present flags](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/ns-d3dkmddi-_dxgk_presentflags)

The real-WDK context fixture reproduces the rejection with `Flip|FlipWithNoWait`
and no DMA buffer: the old code exits at assertion 241 at 14:47:05 UTC.
With the flag admitted, the full context fixture exits 0 at 14:48:56 UTC,
including unchanged-output checks and rejection of unsupported rotation,
missing Flip, multipass and DMA-bearing inputs. Both run as CPU-only controls
on `.59` Code 0; returned logs match device hashes. Host models and the ARM64
kernel/fixture build pass. Artifacts are under
`runs/mmio-nowait-control-20261111/`. This is not yet a native rerun or a
physical screen-rotation implementation.

### Native MMIO Present result

`mmio-nowait-native-20261111` builds and signs `.147` from `2d351cdf`.
Its SYS SHA-256 is
`e491932a9fd3ea6741f890eadaed31c9c96b8d366a4b2a8f5b74756505a553d6`.
Only SYS, INF and catalog differ from `.146`; firmware and the initial no-D3D
stub are unchanged. Windows validates the signature and every catalog member.
The context fixture is byte-identical to the passed no-wait control. The trial
reuses the exact `79fa8169fb00` UMD and `f828845c38b7` ICD, including their
previous controlled verification. Hardware startup completes at 15:02:04 UTC
with Code 0 and a consumed permit.

DWM PID 468 / TID 1948 creates its hardware device on **Tegra X1 (GM20B),
LUID `a2008`, FL10_0**. Its complete fullscreen transition returns S_OK.
The retained native trace then contains **108 successful presents at each
layer**: DxgKrnl Present ReturnStatus=0, UMD Present=0 and outer DXGI
Present Result=0. All corresponding Flip events have MMIOFlip=1,
FlipWithNoWait=1, interval 1 and no DMA buffer. All 108 swapchain identity
rotations return S_OK for three resources. The first and last completed
presents span 24.725 seconds; this is an observation interval, not a measured
steady frame rate. It establishes native passage through the actual Present
boundary that `.146` rejected.

The user confirms a normal desktop during the test and provides a physical
photograph showing Windows wallpaper, Computer Management and Tegra X1
(GM20B) under Display adapters, without a visible warning on that adapter's
icon. The original photo is retained outside the repository as
`user-desktop-photo.jpg`, SHA-256
`082f6cf6739e1ecc4549d0f0a6ee056bb743a9acb826e3ed49028ca77eb928a6`.
It has no independently verified capture timestamp or package-version field;
its trial attribution comes from the user's report. Together with the native
Present records, this is an initial visible desktop milestone, not proof of
all desktop applications, sustained stability or final screen orientation.

Both drawing command lines explicitly select LUID `a2008`. Their logs report
16,384 ordinary and 24,576 independent sampling pixels with zero mismatches,
Device status S_OK, explicit device/adapter teardown and the final success
message. The separate result JSON, identity JSON and exit-code files are
zero-filled after the subsequent reboot, so no surviving exit-code record
is claimed. The native input-desktop record at 15:02:19 UTC identifies
`Default`, the ACPI display, 720x1280 identity mode and origin (0,0).
No native desktop PNG or completed screenshot-result file is retained.

A further boot at 15:03:07 interrupts the controller's first collection.
The retained evidence does not establish its cause. The consumed permit keeps
the restarted `.147` closed; the next controller invocation preserves the
first attempt and proceeds to recovery. The second ETL reports the gated
StartDevice failure `c00000a3`, no UMD events, and DWM devices on Microsoft
Basic Render LUIDs `4dd4`/`4e0d`. Its desktop activity is not additional
native rendering. The observer's generic desktop-path file was replaced
after reboot and likewise must not be used as the earlier native LUID proof.

Read-only export after recovery preserves both original ETLs. The first raw
export contains 115,919 records, including 20,931 valid UMD events. Its
EndTime and BuffersWritten are zero; the header's zero loss counters do not
establish complete coverage. The second contains 157,062 records and is
finalized with 444 buffers written. DxgKrnl, DXGI and Direct3D11 payloads
decode exactly. Dwm-Core event 203 (`SCHEDULE_GETPRESENTSTATS1`) has a
52-byte payload where the installed template describes 36 bytes; 388 such
records in the first trace and 1,250 in the second remain undecoded. They
are not silently accepted as fully decoded events. The adapter, fullscreen
and Present events used above decode without those mismatches.

Raw-export SHA-256 values, respectively:
`ab151a6272ed8387f578a876a30a847a79d0e3bc7be2ff06c93326d701a0220c`
and `3395d42f046e2f98b39d4753715a873b47f04369f2fbeff1ec158cfdb8e1f772`.
All 151 final returned files (89,001,905 bytes) match device lengths and
SHA-256. `.59` Code 0, original driver/table hashes, cleared test state and
unchanged power reports are verified at 15:08:11 UTC. Portrait restoration
and verification at 15:08:32 are a no-op. The package and evidence are under
the external build root's `runs/mmio-nowait-native-20261111/` directory.

Next, implement the requested hardware 1280x720 landscape path and runtime
shared-image handling, then validate sustained composition and visible
Win32 `vkcube`. The existing Blt stub and shared-resource OpenResource path
remain incomplete. A present return or this photograph alone does not close
those contracts or establish a complete Vulkan implementation.

### Hardware rotation backend

The Display A backend now accepts a complete scanout request: physical
address, visibility and a locally defined counter-clockwise rotation. Window A
column scan and direction controls select pixels directly from pitch-linear
BGRA storage. The target DSI timing, 720x1280 output window, prescaled size
and unit DDA remain unchanged; quarter turns exchange the source dimensions.
Each orientation consumes the same 3,686,400 bytes.

| Physical transform | Source | Pitch | Rotation option bits | H/V origin |
| --- | --- | --- | --- | --- |
| Identity | 720x1280 | 2880 | `0` | `0 / 0` |
| 90 degrees CCW | 1280x720 | 5120 | column + H decrement | `5119 / 0` |
| 180 degrees CCW | 720x1280 | 2880 | H + V decrement | `2879 / 1279` |
| 270 degrees CCW | 1280x720 | 5120 | column + V decrement | `0 / 719` |

The horizontal decrement origin is the final byte of the row. Register
definitions and origin rules are checked against NVIDIA L4T 32.7.4
[window programming](https://github.com/OE4T/linux-tegra-4.9/blob/oe4t-patches-l4t-r32.7.4/nvidia/drivers/video/tegra/dc/window.c)
and [register definitions](https://github.com/OE4T/linux-tegra-4.9/blob/oe4t-patches-l4t-r32.7.4/nvidia/drivers/video/tegra/dc/dc_reg.h).
The implementation is original MIT code using those hardware facts; no Linux
source is imported. Pitch-linear column scan still requires physical validation.

A direction, pitch or visibility change always waits for VBlank; an immediate
address-only update can use HBlank. Latch confirmation reads the active bank's
address, direction, pitch and origins together. A partially matching layout
is not a completed flip. Release requires the original visible POST address
with identity pitch/origins. Windows stop recovery now queues that full
identity layout even if the address already equals POST. The compatibility
scanout wrappers still request identity; the Windows integration below uses
the complete surface request for rotated primaries.

The host model checks every output pixel's byte address against independent
coordinate rotation for all four directions, catches incorrect active pitch
or origin, checks VBlank versus HBlank behavior, and exercises rotated-POST
restoration. The Windows build and existing ARM64 display/paging/context
fixtures cover compatibility with the portrait path. These controls do not
prove rotated physical output.
Both real-WDK ARM64 fixtures exit 0 on `.59` Code 0 at 15:28:07 UTC on
2026-11-11; their returned logs match device hashes. The full host models and
ARM64 build pass. Evidence is retained under the external build root's
`runs/hardware-rotation-control-20261111/` directory. No native driver is
installed for these controls.

Remaining integration must preserve the distinct Windows contracts. VidPN
rotation is counter-clockwise; MPO describes its own plane rotation, and
DXGI primary flags can describe already-oriented content. Do not apply one
numeric enum to every layer or rotate the same pixels twice. Also,
`D3DKMDT_VIDPN_HW_CAPABILITY.DriverRotation=0` denotes rotation directly in
the display pipeline, while 1 denotes a software/graphics-engine transform.
[VidPN rotation](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/supporting-rotation-in-a-display-miniport-driver) ·
[Hardware capability meaning](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmdt/ns-d3dkmdt-_d3dkmdt_vidpn_hw_capability)

The allocation/import and Windows scanout work below connect this backend to
the candidate's four normal VidPN rotations. Physical validation must still
verify asymmetric corners, the actual primary geometry and sustained output,
then retain the user's requested normal-handheld 1280x720 landscape setting.
This is implementation in progress, not a completed hardware-rotation milestone.

### Explicit primary storage layout

The shared UMD/KMD primary descriptor now has 48 bytes: the existing buffer
header, format and refresh rate, followed by Windows rotation, width, height,
pitch and a zero reserved word. It describes the allocation's own storage,
not the adapter's latest mode. Identity/180-degree storage is 720x1280 at
2880 bytes per row; quarter-turn storage is 1280x720 at 5120 bytes per row.
Both occupy 3,686,400 bytes. These Windows enum values are not raw Display A
rotation controls; the eventual presentation path must explicitly convert the
relevant Windows contract.

The KMD normalizes the old 32-byte portrait descriptor to explicit identity
geometry and rejects a nonzero old reserved word. Ordinary 16-byte NVK buffer
and 32-byte linear-surface requests keep their existing contracts. New primary
producers require the updated KMD: `.147` does not accept the 48-byte request.
Standard shared-primary/shadow generation retains the unrotated portrait
layout; the DXGI primary producer can supply explicit quarter-turn storage.
The dynamic 512 MiB budget and scanout-segment accounting are unchanged.

`DescribeAllocation` reports stored geometry even when old and new primaries
coexist. The UMD imports the primary's actual pitch and byte size into Zink/NVK.
`DXGI_DDI_PRIMARY_NONPREROTATED` retains identity storage, as specified by the
[primary descriptor contract](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/dxgiddi/ns-dxgiddi-dxgi_ddi_primary_desc).
An ordinary GDI blit checks each allocation's bounds and uses separate source
and destination pitches in its copy commands. This is a straight copy; the
GDI `Rotate` operation remains unsupported.

Trace event 32, `PrimaryLayout`, records input mode width/height in `arg0`
and primary flags/raw mode rotation in `arg1`. It is emitted before validation.
Its S_OK field means an informational record, not successful allocation,
presentation or rotation. The subsequent allocation/mode/Present records still
determine each operation's result.

Validation on 2026-11-11:

- All host models and the real-WDK ARM64 build pass. On `.59` Code 0, the
  paging and context fixtures both exit 0 at 15:45:29 UTC. They cover legacy
  normalization, four explicit rotations, malformed layout rejection and
  portrait/landscape copies with independent pitches. Returned logs match
  device hashes.
- UMD candidate `scanout-layout-20261111-umd+413d32bb6034` passes the primary,
  Present, adapter, device and DLL-entry controls at 15:46:27 UTC. Primary
  controls cover all four rotations, nonprerotated storage, invalid extents
  and flags, paging failures and lifetime cleanup. The finalized ETW contains
  114 valid events with no recorded loss, including three input-layout records.
  All 12 returned files (159,133 bytes) match device lengths and hashes.

Evidence is retained outside the repository under
`runs/scanout-layout-control-20261111/` and
`runs/scanout-layout-umd-control-20261111/`. These execute controlled ARM64
fixtures on Windows without installing the candidate or rendering on the
native GPU. Actual landscape modes, physical latch/rotation and sustained DWM
composition remain to be validated.

### Windows scanout rotation

Legacy `SetVidPnSourceAddress` now reads immutable allocation metadata through
the KMD handle supplied and retained by Dxgkrnl. It checks ownership, primary
format, actual pitch/dimensions and the physical scanout range before programming
the fetch layout. This lookup does not take an engine lock at display DIRQL.
A null allocation during a mode change still selects the original POST buffer.
[Allocation-specific programming contract](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/ns-d3dkmddi-_dxgkarg_setvidpnsourceaddress)

MPO validation and submission share the same geometry check. Its explicit
clockwise plane rotation converts to the backend's counter-clockwise enum;
neither the VidPN angle nor the allocation's orientation is added again.
The source rectangle covers the actual storage and the destination covers
the native 720x1280 panel. Unsupported crop, stretch, blend and extra planes
remain rejected.
[MPO rotation and rectangles](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/ns-d3dkmddi-_dxgk_multiplane_overlay_attributes3)

Pending/displayed state includes the physical rotation. Interrupts and immediate
completion confirm the active address, direction, pitch and origins together.
The same address with a different rotation is a new flip, and a mixed or
incomplete latch cannot inherit its PresentId. Blanking waits for the original
POST layout and clears stale pending state before VidMm can release the old
primary. Stop also restores identity even when the address already equals POST.

VidPN reports identity/90/180/270 with Offset0 only. The `.148` integration
incorrectly kept source-mode enumeration at 720x1280 as well as retaining the
unrotated standard shared primary. Those are distinct contracts: the desktop
source extent must be compatible with the path rotation and fixed panel target.
The subsequent source-mode correction is described below. A DXGI allocation
independently describes its actual stored extent; the controls cannot establish
which primary layout DWM will choose. `DriverRotation=0` selects dedicated
display rotation. Smooth rotation
is not advertised: CommitVidPn records the successfully committed angle and the
primary-address DDI applies the corresponding allocation layout atomically.
An active-path request to change the angle without that modeset is rejected.
The no-DMA flip Present accepts `Rotate`; GDI rotated blits remain unsupported.
[Rotation contract](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/supporting-rotation-in-a-display-miniport-driver) ·
[Active-path and smooth-rotation contract](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/nc-d3dkmddi-dxgkddi_updateactivevidpnpresentpath)

The real-WDK scanout fixture links the display DDIs and register backend,
substituting allocation handles, register banks, interrupt delivery and shared
clock data. It checks all four legacy/MPO transforms, same-address direction
changes, corrupt pitch and incomplete-latch rejection, immediate recall/waits,
pending-flip blanking and rotated-POST recovery. The VidPN fixture checks all
four supported/committed angles, rotation pivots, reference cleanup, failure
propagation and rejection of path-only angle changes. The context fixture checks
real allocation ownership and no-DMA rotation dispatch.

All host models and the ARM64 build pass. On `.59` Code 0, paging, VidPN and
scanout fixtures exit 0 at 16:04:23 UTC on 2026-11-11. The context fixture's first
run stops at a stale assertion expecting rotated flips to be rejected; after
correcting that expectation, it exits 0 at 16:06:30 UTC. Returned logs match
device hashes. Evidence and the failed original binary/source are retained in
`runs/panel-rotation-v2-control-20261111/`, with the final context result in
`context-recheck/`. No native driver was installed for these controls.

### Native explicit-layout regression

The `panel-rotation-native-20261111` trial pairs source-built KMD `.148`
(`8b599271`) with UMD `scanout-layout-20261111-umd+413d32bb6034` (tested source
`8427bc89`) and the unchanged `f828845c38b7` NVK ICD. The signed SYS is
`5d6c3e2893c32db72f1f0fc1c359e9984c706b29120d3e439d5a12dbfb16f3f2`;
the CAT is `601690828ee2ebcdff4b098da5155301a0a59ce3cc02cc7b2013a5b4b06e5a5d`.
All package signatures and catalog memberships are verified before installation.

On 2026-11-11, native LUID `9a1e7` reaches hardware startup/Code 0. The ordinary
probe matches 16,384 RGBA/BGRA clear/draw pixels, and the independent texture/
sampler cases match another 24,576. Both report S_OK device status, explicitly
release their devices, and retain exit-0 records. The controller reports no
timeout and completion at 16:21:40 UTC. These probe records are flushed before
desktop capture and survive the later interruption; their source files, not
only their success messages, are present in the verified receipt. Probe feature
level is 10_0; these probes do not exercise sharing or presentation themselves.

The independent DWM trace records PID 692/TID 1652 issuing 469 successful flip
Presents at each of the KMT, UMD callback and outer DXGI layers, plus 469 successful
rotations of its three swapchain storage identities. The retained Present span
is 42.2345337 seconds, not an FPS measurement. Four other KMT records from DWM
TID 1592 have flags `0x8000` and are excluded from that flip count.

All 469 flips use no-wait MMIO presentation at interval 1. MPO records identify
one full-size 720x1280 plane in segment 3. For every submitted flip, a subsequent
same-adapter VSync reports its target physical address before the next captured
flip. The three addresses are `0xfc09f000`, `0xfb997000` and `0xfbd1b000`, used
157/156/156 times. The driver produces those scanout reports from the active
register bank; the new path checks rotation, pitch and origins as well as the
address before completing pending state. This is real portrait scanout evidence,
not a physical validation of the quarter-turn layout.

The native `Default` desktop/ACPI display state is retained before and after
the drawing probes, still 720x1280 identity. No desktop image or capture-result
file is retained. The user did not observe the native interval and confirms
only the recovered desktop. Thus this trial does not add a visible desktop
photograph to the `.147` milestone or establish long-term stability. A later
startup interrupts normal collection; its cause is not established. In
particular, the last available pre-capture record does not locate a fault.

The native ETL is unfinalized (`EndTime=0`, `BuffersWritten=0`), containing
165,790 exported records, including 19,933 UMD events. Early DWM HWDEVICE and
PrimaryLayout records are not retained; their absence is not nonexecution.
The later ETL is finalized (436 buffers, 158,581 records): the Tegra startup gate
returns `c00000a3`, there are no UMD events, and DWM selects Basic Render LUIDs
`4e12`/`4e4b`. It is not continued native rendering. The device-provided Dwm-Core
203 template still accounts for only 36 of its 52 payload bytes, leaving
1131/1202 records undecoded in the two files; the other provider schemas decode.
ETL absolute timestamps differ from the live/CIM wall clock, so the timeline
uses wall-clock controller records and the within-trace Present span separately.

The raw export SHA256 values are:

| Trace | SHA256 |
| --- | --- |
| Native `.001` | `4590f514c3d87efce57033912129ba0b2e325ea4dafb0b6e03ce4133eba9f63a` |
| Later `.002` | `342fde691430391f6b2be41f76254eeba71456ebb3c62f93cc5cf94817bbf293` |

Recovery stages `.59` at 16:25:21 UTC and reboots. `.59` Code 0, baseline SYS/ACPI
hashes, temporary-registration/task/environment cleanup and unchanged power
policy are verified at 16:31:52; portrait restoration/verification is a no-op
at 16:32:52. All 152 returned files (91,883,051 bytes) match device lengths and
SHA256. The first receipt reports `loader-debug.csv` length 0 despite a matching
hash for its actual 41,218 bytes. That receipt is preserved; the final collector
derives byte count and SHA256 from the same read stream and verifies all files.
Local `native-summary.json` and `display-analysis-001/scanout-correlation.json`
retain the counts and per-flip matches. Hardware landscape and a visible Win32
`vkcube` remain the next physical milestones.

### Landscape source-mode correction

The subsequent `landscape-native-20261111` trial reuses the exact signed `.148`
KMD, UMD and ICD above. Native LUID `940b9` reaches Code 0, but CCD
`rotation-test 90` returns 1610 (`ERROR_BAD_CONFIGURATION`) at 17:07:24 UTC.
The controller records `ValidationRejected`, leaves the 720x1280 identity mode
unchanged, and never launches `vkcube`. Native snapshot restoration succeeds
without a mode change. Recovery verifies `.59` Code 0, baseline hashes, complete
cleanup and unchanged power policy at 17:15:27 UTC; the baseline portrait
snapshot is also a no-op at 17:16:27. The earlier user's normal-desktop report
does not establish a visible landscape result.
All 168 returned files (60,978,917 bytes) match device SHA256 and lengths.

Code review finds that `.148` advertises quarter-turn rotations while accepting
and enumerating only the 720x1280 source. The candidate now provides both source
shapes: 720x1280 with pitch 2880 for identity/180, and 1280x720 with pitch 5120
for 90/270. Pinned source geometry and path rotation constrain one another;
an unconstrained source/rotation exposes both shapes. Source and rotation pivots
remain untouched, and CommitVidPn rejects incompatible pairs. The physical DSI
target remains 720x1280. Standard shared-primary allocation and the independent
48-byte DXGI primary storage description are unchanged by this correction.
[Cofunctional mode constraints](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/enumerating-cofunctional-vidpn-source-and-target-modes) ·
[Source geometry and stride](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmdt/ns-d3dkmdt-_d3dkmdt_graphics_rendering_format)

Host models and the real-WDK ARM64 build pass. Four controls run on `.59` Code 0
at 17:15:32–33 UTC, with all exits 0 and returned logs matching device hashes.
The expanded VidPN fixture checks both source shapes, each angle/shape pair,
wrong-pitch rejection, source/rotation pivots, preservation of OS-assigned mode
IDs and cleanup after a failed second AddMode. Its executable SHA256 is
`5d6e1ef948e1a8c6529923d7425a5fda14f1cb66103980436a817cc5fe3bfab9`;
evidence is in `runs/vidpn-source-control-20261111/`.
The next native KMD version is `.149`. These controls do not yet prove that
Windows accepts the corrected native mode or that the panel rotates correctly.

The trial controller also retains the process handle before waiting for a
PowerShell `Start-Process` child. A control caught null exit codes without that
retention; the corrected controller reports exact exits 0 and 7 at 17:02:33 UTC,
before the native test is armed. The original failed control is retained.

### Native source-mode retest

`landscape149-native-20261111` uses KMD `.149` from `ebfd5142`, with unchanged
UMD `413d32bb6034` and ICD `f828845c38b7`. Native LUID `bd778` reaches Code 0
at 17:31:34 UTC, but the 90-degree CCD validation still returns 1610 at
17:32:03. No mode is applied and `vkcube` is not launched. The source-mode
correction is therefore insufficient to fix the physical landscape path.
The user reports no rotation and a missing bottom menu, apparently already
at native startup. The exact physical observation timestamp is unavailable.
This is not a complete visible-desktop pass.

The recovered export contains 129,717 records, including 21,071 UMD events;
the finalized ETL reports no event/buffer loss. Dwm-Core event 203 retains the
known 36/52-byte template mismatch in 622 events. Raw SHA256:
`51292758886c7dc78a88a856d4db1d8463a08b2c8e2efdb979528cd772a3e9ff`.
The live controller reports collection at 17:32:37, but that result file is
zero-filled after a subsequent startup. The live monitor and durable native
result are retained; do not infer an interruption cause from this observation.
All 153 returned files (64,013,449 bytes) match device lengths/hashes. Baseline
`.59` Code 0, cleanup and unchanged power policy are verified at 17:39:47;
portrait restore/verify is a no-op at 17:40:49–50 UTC.

### AppContainer runtime loading

The `.149` trace contains repeated D3D11 `LoadUMD / 80070005` errors and DXGI
journal entries naming `\\app\\TegraGpuD3D.dll`. DWM and Explorer have successful
UMD loads. The failed PIDs are not mapped to executable identities by this
retained trace, so they must not be labeled StartMenuExperienceHost without
additional evidence. The absence of UMD OpenResource events is also not proof
that sharing is unnecessary: some processes fail before entering the UMD.

The test runtime files under ProgramData grant ordinary Users read/execute,
but lack an AppContainer ACE. An AppContainer access check also considers the
package identity, so ordinary-user access alone is insufficient.
[Microsoft AppContainer access model](https://learn.microsoft.com/en-us/windows/win32/secauthz/implementing-an-appcontainer)

`tools/appcontainer_load_check.cpp` compares file read/execute and LoadLibraryEx
in an ordinary process and a disposable AppContainer, without calling any
D3D/Vulkan entry point. It grants only its temporary package SID access to the
probe executable, restores the executable ACL, and deletes its own profile.
It records the actual session/window station/desktop. Build with an ARM64
LLVM-MinGW C++ compiler using `-std=c++17 -static -municode -ladvapi32 -luserenv
-luser32`.

On `.59` Code 0 at 17:50:20 UTC, the ordinary and AppContainer processes both
run in session 1, `WinSta0/Default`. System D3D11, GDI32 and User32 load in both.
Original UMD/NVK files load normally but return file/load error 5 inside the
AppContainer. Byte-identical private copies with `S-1-15-2-1` read/execute load
successfully there, as does the copied Vulkan Loader. The probe and scheduled
task exit 0; executable ACL restoration and profile deletion succeed. No native
driver is enabled during these controls. The original runtime ACLs are intact.
All 16 returned control files match device lengths/hashes; evidence is in
`runs/appcontainer-load-control-20261111/`.

An earlier SSH/noninteractive control loads the corrected UMD but returns
NVK initialization error 1114. That result is retained, including a separate
NVK-only check. It does not reproduce in the interactive session and is not
attributed to the native desktop ICD. This distinction matters when testing
DLLs that import desktop graphics APIs.

`tools/Enable-RuntimeAppContainerRead.ps1` applies read/execute access to the
staged runtime directory and its children, verifying the three runtime DLLs.
It changes no parent/system ACL and grants no write access. Use it for the
temporary runtime outside DriverStore before the next native desktop trial.
The loading control establishes a deployment defect; the missing menu still
requires physical validation and may expose further D3D sharing/present work.

### Native AppContainer loading retest

`desktop-acl-native-20261111` changes only the staged runtime directory's
AppContainer read/execute access, using `5852cfcc`. KMD `.149` (`ebfd5142`),
UMD `413d32bb6034` and ICD `f828845c38b7` are byte-identical to the preceding
trial. On LUID `bb153`, startup reaches Code 0 at 18:01:44 UTC. The controller
observes the portrait desktop from 18:01:58 to 18:03:18, without a rotation
request, new drawing probe or cube. Its exit 0 means observation collection
completed; it is not a rendering or taskbar-success result.

The user confirms missing/incomplete bottom taskbar content during this active
interval. All six snapshots find `Shell_TrayWnd` present, marked visible and
within the 720x1280 source at `(0,1232)-(720,1280)`, matching the baseline.
Explorer changes from PID 1300 to 2784 between snapshots; the trace also
records a later instance, 3336. This does not establish a crash cause, and a
window's rectangle/visibility flag does not prove its composed pixels.

The retained trace contains no D3D11 `LoadUMD / 80070005` record.
StartMenuExperienceHost has six successful UMD open results and three
successful D3D device results across PIDs 1316, 6372 and 2184. This directly
confirms progress past the earlier loading restriction. Remaining failures
are distinguishable:

| Process/path | Retained evidence |
| --- | --- |
| Explorer 1300, 2784, 3336 | Each creates a 576x960 `DXGI_FORMAT_B8G8R8A8_UNORM` resource, MiscFlags `2` (SHARED), BindFlags `a8`. The D3D11 journal then reports `ConfigureSharedResource` and `ShareObjects` with `c000000d`. This is an Explorer shared surface; the trace does not identify it as the taskbar's own surface. |
| SearchHost 1336, 2032, 6580; some TextInputHost instances | UMD loading succeeds, but Zink creation fails at stage 8, `vkCreateDevice`, with `VK_ERROR_INITIALIZATION_FAILED (-3)`. The outer UMD result is `887a0004`. Other TextInputHost instances do create devices. The exact underlying KMT failure is not retained. |
| DWM 828 | 71 retained UMD Present calls return S_OK. This does not override the user's missing/incomplete-menu observation. |

Source inspection finds that non-primary shared textures currently call
Gallium `resource_create` without attaching their storage to `hRTResource`
through the runtime allocation callback. `OpenResource` is still an
unimplemented E_OUTOFMEMORY path. The next implementation must connect real
shared image metadata, runtime-owned allocation/residency, GPU contents and
consumer lifetime; neither a handle cast nor a successful descriptor-only
sharing probe is sufficient. Device creation also needs its underlying KMT
operation/status retained before further capability or memory-policy changes.

The finalized ETL has 160,415 records, including 32,273 UMD events, with no
reported event/buffer loss. The known Dwm-Core event 203 template mismatch
affects 773 records; other configured schemas decode. The original and
recovered exports are byte-identical. Raw SHA256:
`b438f06ff71cf205b51f7987e7cf265272624aa2711c2d3542ca42dbc0b36f34`.
Controller wall-clock times and within-trace ordering are kept separate.

All 163 returned files (75,003,540 bytes) match device lengths/hashes. Following
the recorded recovery startups, `.59` Code 0, baseline SYS/ACPI, complete
temporary-registration/task/environment cleanup and unchanged power policy
are verified at 18:11:24 UTC. Portrait restore/verify is a no-op at 18:12:17.
No interruption cause is inferred. `native-summary.json`, `umd-summary.json`
and `display-analysis-001/process-failures.json` retain the scoped findings.

### Runtime shared texture bridge

The next candidate implements the path missing from the `.149` Explorer trace.
Shared BGRA/RGBA, single-mip/single-layer, single-sample default textures now
allocate through the real D3D runtime resource handle. A 72-byte, pointer-free
renderer descriptor accompanies the existing KMD resource envelope and 32-byte
linear allocation ABI. Layout uses 128-byte rows, four-row padding and 4-KiB
backing, bounded by the existing 512-MiB cap.

OpenResource validates that descriptor and imports the allocation already opened
by the runtime for the consumer device. It neither casts a kernel resource
handle into a runtime handle nor allocates a replacement backing store. Each
owner has its own paging queue, residency and GPU-VA mappings; cleanup releases
the correct runtime reference. A failed OpenResource unwinds immediately because
the runtime will not issue DestroyResource for that failed handle.
[Microsoft OpenResource contract](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/nc-d3d10umddi-pfnd3d10ddi_openresource)

The Zink/NVK import preserves existing linear contents and explicit BGRA/RGBA
row pitch without labelling shared textures as display primaries. This is a
limited shared-texture implementation, not a claim of support for every D3D
format, resource type or synchronization contract. The next native trial must
record legacy and NT-handle results separately. Failing KMT device/context/fence
creation now emits the original API and NTSTATUS as `[NVK KMT]` debugger output.

Controls completed on `.59` Code 0 at 18:50:06 UTC on 2026-11-11:

- The new ARM64 runtime-owner fixture passes aligned BGRA/RGBA metadata,
  consumer-specific handles, pending paging, producer release, stale mapping
  cleanup and failure unwinding. It uses controlled callbacks, not GPU hardware.
- Existing primary, Present, adapter, device and DLL-entry fixtures pass. Their
  finalized 114-event trace reports no event or buffer loss.
- The real Windows D3D runtime probe passes legacy and NT sharing on Microsoft
  WARP: both formats match 4,096 pixels in each direction and after producer
  device release, totalling 49,152 checks. WARP is a software control; it does
  not validate Tegra rendering, desktop composition or presentation.
- All 14 returned evidence files match remote lengths and SHA256. The source
  also passes host models and the real WDK ARM64 build. The linear ABI was
  centralized without changing its byte layout.

Artifacts: `runtime-shared-umd-control-20261111`, UMD
`runtime-shared-20261111-umd+c46b59544870`, ICD
`runtime-shared-20261111-icd+6e2fad99d742`. The tested probe hash is
`8d05297c6ac550efd378faf62a09988d5250c8b2b414ac3d125535d5d49e4475`.
Its final success message incorrectly retained the earlier descriptor-only
scope; the per-phase pixel records above are authoritative. The rebuilt probe
`10aec6cd4e44ca072eec3434e3991121ca3df60b377c6af0c4e7f2000676442e`
changes only that final message. Native shared pixels remain unverified.

### Native shared trial and page-table capacity

`runtime-shared-native-20261111` pairs signed KMD `.150` (SYS `9b6130182f29`)
with the byte-identical controlled UMD `c46b59544870` and ICD `6e2fad99d742`.
The source is `be55a95b`. A later probe-only rebuild relinked the UMD as well;
the trial deliberately reuses the previously tested DLL, with that substitution
recorded in its candidate manifest. The corrected probe is `10aec6cd4e44`.
All 13 signed package files/catalog members pass verification before install.

The controller records Code 0, hardware status 0 and consumed native permit at
19:10:01 UTC. Its interactive task is requested at 19:10:14. A subsequent boot
at 19:11:00 interrupts collection; no final probe result, pixel log or desktop
screenshot survives. On that boot `.150` is correctly stopped at the one-shot
gate (`c00000a3`, Code 43). The user's visible Start-menu reports are not tied
to a verified native interval. No cause is assigned to the subsequent restart.

The first ETL contains 88,138 exported records, including 17,426 UMD events.
It is unfinalized (`EndTime=0`, `BuffersWritten=0`); reported event/buffer losses
are zero. The exact device templates decode all four system providers except
258 known Dwm-Core 203 payload-size mismatches. Its raw export SHA256 is
`6d4645652181ff8c03c1e680e5ebddfe5f1877e5d8a40a0434d3654a00d9ea20`.

The native adapter LUID is `c0728`. These ordered events identify an actionable
resource-capacity boundary without relying on the trace's final event:

| Evidence | Retained value |
| --- | --- |
| Native Segment 1 size/commit limit | 2,097,152 bytes |
| Last Segment 1 residency before first failed reservation | 2,097,152 bytes |
| Page-table residency increments | 131,072 bytes, matching the GM20B table size |
| Paired failed ReserveResource calls | 12, all Segment 1, `c0000001` |
| Ordinary Segment 4 residency immediately before first failure | 22,900,736 bytes |
| Peak ordinary Segment 4 residency in this trace | 84,910,080 bytes |
| Retained DWM UMD Present results | 14 S_OK across 6.261 seconds, then `88760870` |

The first failed reservation is followed by `VidSchMarkDeviceAsError` reason
12; later DWM reports device reset/removed. This establishes exhaustion of the
old 16-table pool as a concrete desktop allocation limit. It does not prove
that increasing the pool resolves every subsequent failure. The trace contains
no SHARED texture CreateResource or OpenResource event, so it cannot validate
or reject the new shared-pixel implementation. Absence in this interrupted
capture is not proof that a later operation never executed.

The debugger log retains only 32,768 bytes and a partial final line; the absence
of `[NVK KMT]` there does not rule out a KMT error. ETL timestamps also precede
the controller's wall-clock startup by approximately 30 seconds. Preserve event
ordering within this stream rather than naively correlating those UTC values.
The second ETL is finalized, has zero UMD events, and belongs to the closed-gate
boot. The retained startup-contract file contains that later default state,
not the overwritten live native record.

Recovery verifies `.59` Code 0, baseline hashes, no test tasks/temporary runtime
registration, unchanged sleep/display settings, and portrait at 19:23:24–36
UTC. All 147 files (93,316,658 bytes) match the remote lengths and SHA256; the
four earlier downloads are byte-identical to the final copies.

The next KMD candidate expands the MMU pool to 128 tables (16 MiB), retaining
the existing dynamic 512-MiB application cap. This is a bounded infrastructure
allocation, separate from image/buffer storage. Test the native residency
beyond the old 2-MiB limit with the same UMD/ICD, then retry real shared pixels
and desktop composition; do not claim those results from the capacity change.

### Native table-pool retest

`table-pool151-native-20261111` changes only the KMD table-pool capacity from
`.150`: `.151`, source `71322147`, signed SYS `ab14c3fc0667`. UMD `c46b59544870`,
ICD `6e2fad99d742` and probe `10aec6cd4e44` are reused byte-for-byte. The paging
and context fixtures pass on `.59` at 19:27:14 UTC, including mapping past the
former boundary, the last complete table, and rejection beyond the backing.

Native LUID `baff1` reaches Code 0 at 19:33:08 UTC. The real D3D probe finishes
at 19:33:37 with exit 1, retaining these separate results:

- Legacy BGRA and RGBA each match 4,096 solid-color pixels producer-to-consumer,
  consumer-to-producer, and after producer-device release: 24,576 checks total.
  This proves that specific shared storage/read/write/lifetime path, not
  nonuniform sampling, geometry, composition or presentation.
- The following NT/keyed-mutex BGRA creation returns `80070057`, so NT RGBA is
  not reached. The complete `--shared-contract` probe remains failing.

The first retained ETL contains 242,112 exported records, including 83,885 UMD
events. It is unfinalized and omits the startup ReportSegment/DpiReportAdapter
records; there are no reported event/buffer losses and 610 known Dwm-Core 203
payload-size mismatches. Its export SHA256 is
`26b1a95509c7335e95e3185d7d10676a7bbf3900db1ee9ae76a351e7c3671c89`.
Use the retained event ordering and keep the missing head/tail limits explicit.

The one adapter in the retained residency records reaches 2,883,584 bytes in
Segment 1 (2.75 MiB, beyond the old pool), with no failed ReserveResource in
this capture. Segment 4 peaks at 115,965,952 bytes. This validates progress past
the previously observed table-capacity boundary, not complete GPU stability.

Explorer now reaches 27 shared BGRA creations; DWM has 26 successful
OpenResource records for 576x960 images at pitch 2304. All 26 retained Explorer
Present calls have flags `1` (Blt), interval zero, and return `887a0004`.
The source's RuntimePresent function rejects that flag before the runtime
callback because it accepts only primary flips. The D3D11 journal has 26
corresponding driver-error/device-removal records (`887a0020`). There are no
BltDXGI callback events in this capture. Do not confuse that separate callback
with the Blt flag on PresentDXGI.

DWM retains 480 successful UMD flip Presents across 11.175 seconds of ETL event
time; these do not establish a complete physical frame. Four window snapshots
still place the taskbar at `(0,1232)-(720,1280)`, while Explorer's PID changes
from 4408 to 5176. The user reports approximately three flashes: the first
taskbar is misplaced **and incomplete**, and the later taskbar is absent. Two
private evidence photos show those failures while wallpaper/icons remain.
No complete native desktop, hardware landscape or cube is claimed.

The last durable task stage starts desktop capture at 19:34:23; no capture or
overall native-result file survives the further boot at 19:34:53. The retained
probe log/result does survive and matches the earlier download. The cause of
the subsequent startup is not investigated. `.59` Code 0, cleanup, baseline
hashes, unchanged power settings and portrait are verified at 19:40:17–31 UTC.
All 161 files (120,310,462 bytes), including all eight earlier copies, match
device lengths and SHA256.

The next UMD change routes Blt-mode Present for runtime-owned shared surfaces
through the actual DXGI callback after GPU completion, preserving the allocation,
runtime context and opaque token and returning the callback's exact result.
The runtime may determine an unspecified destination. Native KMD copy behavior
still needs validation; accepting the callback path is not a substitute for
copying or rendering pixels.
[Microsoft Present arguments](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/dxgiddi/ns-dxgiddi-dxgi_ddi_arg_present) ·
[DXGI presentation paths](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/dxgi-presentation-path)

The new callback fixture checks non-primary Blt presentation, GPU-completion
ordering, exact runtime allocation/context/token, positive and negative callback
statuses, and rejection of flip/mixed flags for shared storage. Existing primary
flip tests retain their ownership checks. The new `--shared-layout` application
probe draws a nonuniform pattern into a 577x961 shared texture (padded rows),
then samples it on a 720x1280 target with two different translated/scaled
viewports, scissors and constant-buffer values. It compares every output pixel
against integer coordinate expectations in BGRA and RGBA: 3,686,400 checks if
all four passes succeed. It does not present a desktop or replace NT testing.

Both the UMD and probe compile for ARM64. Initial SSH failures prevent upload
and execution. At 19:50:09 UTC, connectivity is restored and the exact candidate
`runtime-windowed-20261111-umd+11116dc1b815` passes the Windows controls on
`.59` Code 0. The real runtime-owner fixture exercises shared Blt Present,
GPU-completion failure and exact callback status/token propagation; primary,
flip, adapter, device and DLL-entry controls also pass. The finalized control
trace contains 114 events with no reported loss. Microsoft WARP passes all four
nonuniform BGRA/RGBA placement/scissor cases, 3,686,400 pixels with zero mismatch.
All 14 returned files (229,011 bytes) match device lengths/hashes. These are
controlled callback and software-renderer results, not native presentation.
The paired ICD remains `6e2fad99d742`; `.152` tests this candidate below.

### Native windowed Present retest

`windowed152-native-20261111` pairs source `24c1ca9a`, signed `.152` SYS
`22c86127cd25`, UMD `11116dc1b815` and ICD `6e2fad99d742`. KMD behavior is
unchanged from `.151`. Native LUID `bbde7` reaches Code 0 at 19:57:57 UTC.

The retained first ETL contains 127,594 exported records and 34,514 valid UMD
records. Explorer's one observed shared Blt-mode Present returns S_OK instead
of the previous `887a0004`. Its matching kernel Present also succeeds with
flags `0x8000`: `D3DKMT_PRESENTFLAGS.RedirectedFlip` in SDK 10.0.26100.0,
not a physical-copy request. DWM successfully opens the 576x960 shared image
with pitch 2304 and has 63 successful flip Present callbacks. No retained
ReserveResource failure appears. These callbacks do not establish complete
physical desktop composition or prove which later submission fails.

After Explorer's successful Present, Dxgkrnl marks a device in error, and
D3D11 records `SignalSynchronizationObjectFromGpu2 = c00002b6` and device
removal/reset. The scheduler's numeric Reason fields are not NTSTATUS and
have no independently established meaning here. Explorer subsequently fails
timeline-semaphore initialization. The patterned probe starts at 19:58:20 UTC
but fails `vkCreateDevice` with `VK_ERROR_DEVICE_LOST`; no native pattern-pixel
result or final `native-result.json` survives. Do not treat the WARP result as
native evidence or infer a zero-size DMA submission from a null ETW pointer.

The user reports about three flashes seconds apart: initially correct taskbar
placement with the right-side tray missing, then no Start/taskbar from the
second flash onward. This remains an incomplete and unstable desktop result.
The script requests no second mode/orientation change during this observation.
The physical flashes cannot be assigned to individual restarts from the logs.

The machine's recorded boot times separate planned setup/recovery from the
additional startup:

| UTC on 2026-11-11 | Observation |
| --- | --- |
| 19:55:29 | Planned setup boot, `.152` initially gated closed. |
| 19:59:05 | Additional startup after native probe launch; still `.152`, gate closed. Its cause is unconfirmed. |
| 20:02:07 | Recovery stages the `.59` package. |
| 20:02:35 | Planned recovery boot into `.59`. |
| 20:05:11 | `.59` Code 0, baseline hashes, cleanup and unchanged power settings verified. |
| 20:06:35 | Saved portrait verified; no mode change needed. |

All 152 returned files (102,245,042 bytes) match device lengths and SHA256;
the early copies also match their final counterparts. Raw first-export SHA256:
`9e87cdc8f00cf065f2fe210479d1ad6991d149cdd091ff9690b3de10c6ccb833`.
The first ETL remains unfinalized (`EndTime=0`, `BuffersWritten=0`); it reports
no lost events, but startup adapter/segment records are absent. Neither the
last retained event nor an absent event identifies the first failure.

The next `.153` candidate adds a first-recovery ETW event at the KMD's actual
failure-reporting path, before OS notification. Context/worker models pass on
Windows ARM64 `.59` at 20:16:20 UTC, including failed synchronization retry
and DISPATCH_LEVEL preemption. Native delivery and the actual failure cause
remain pending. [Recovery event schema and limits](RECOVERY_TRACE.md).

### Native patterned layout and recovery trace

`recovery153b-native-20261111` pairs KMD source `110d9882`, signed `.153`
SYS `94bcb716d824`, unchanged UMD `11116dc1b815`, ICD `6e2fad99d742` and
probe `3dbbc5770d96`. The new KMD provider is registered successfully and
enabled at level 2, keyword 1. Native LUID `b3c87` reaches Code 0 at 20:30:46
UTC; the layout probe runs from 20:31:07 to 20:31:10 and returns exit 1.

Both D3D devices, the 577x961 padded shared BGRA image, its consumer import,
SRV, sampler and 720x1280 target are created successfully. Producer and
consumer `GetDeviceRemovedReason` both return S_OK. The first BGRA pass reads
921,600 pixels and reports **73,584 mismatches**, equal to the 584x126 scissor
area. The first eight reported pixels are zero, including alpha, instead of
the expected pattern. The remaining pass and RGBA cases do not run after this
failure. The log does not separately count all zero pixels or prove that every
mismatch lies inside the scissor rectangle.

The complete result file survives; the native task finishes at 20:31:53 and
collection at 20:32:28. The finalized ETL has 309,805 exported records, 65,455
valid UMD records, `BuffersWritten=1177` and nonzero EndTime, without reported
event/buffer loss. Its adapter report matches Tegra LUID `b3c87`. The UMD has
70 successful DWM flips and two successful Explorer windowed Presents, with
no ZinkFailure or RuntimeError record. No retained ReserveResource failure or
new KMD recovery event appears. Registration/enablement is verified; actual
recovery-event emission is not. Callback success, window geometry and an absent
recovery event do not establish correct physical desktop pixels.

After collection, recovery begins and an additional startup is observed at
20:33:41 UTC while `.153` is still installed with its gate closed. No cause is
assigned. The repeated recovery stages `.59` at 20:36:32; the final recovery
boot is 20:37:16. `.59` Code 0, baseline hashes, cleanup and unchanged power
are verified at 20:38:44, followed by a no-op portrait verification at 20:39:26.
All 151 returned files (107,778,029 bytes) match remote lengths and SHA256;
five early copies match the final files. Raw export SHA256 is
`d1c942d92f4ea3959146c67bd670c578e071dbd0e2592466d2b5f29686498fc3`.

The first `.153` setup attempt (`recovery153-native-20261111`) did not grant
native execution: the operator invoked the low-level bind/arm helpers instead
of `bind-cube.ps1`, omitting registration metadata. Automatic recovery returned
to `.59`, verified at 20:25:41; all 89 files (1,516,919 bytes) match device
hashes. The retry uses the complete wrapper and adds an early registration
precondition. A Windows control at 20:27:06 confirms missing metadata is
rejected before changing the binding, table, permit, trace or task state.

The next probe (`shared-checkpoints-20261111`, EXE `073007e7440e`) adds optional
`--shared-layout-checkpoints`: read back the producer's pattern and the opened
consumer image before sampling, each checking 554,497 pixels per format. This
separates producer rendering/writeback, shared-image visibility and sampling.
It deliberately adds readback and synchronization; retain the unchanged
`--shared-layout` mode for comparison so a checkpoint cannot silently mask an
ordering defect. The current hardware UMD's Flush already calls
`RuntimeDeviceFinish`, including a GPU-fence wait; generic D3D Flush semantics
alone do not explain this native failure.

Only the probe executable is rebuilt. On Windows `.59` at 20:39:27, WARP
passes the original mode's 3,686,400 pixels and the checkpoint mode's 5,904,388
pixels, including both producer/import checkpoints in BGRA and RGBA. Both exit
0 with empty stderr and matching returned log hashes. Native checkpoint
execution and the underlying pixel fix remain pending.

### Shared sampling checkpoints and vertex-buffer lifetime

`shared-checkpoints153-native-20261111` keeps KMD `.153` (`94bcb716d824`),
UMD `11116dc1b815` and ICD `6e2fad99d742`, replacing only the probe with
`073007e7440e` from `ad3fd568`. Native LUID `af705` reaches Code 0 at 20:51:25
UTC. The original mode exits 1 at 20:51:49 with the same 73,584 mismatches.

The checkpoint mode verifies **554,497 producer pixels and 554,497 imported
consumer pixels**, all matching the nonuniform BGRA pattern, with zero
zero-alpha pixels. Its first sampled-layout case still has 73,584 mismatches
out of 921,600 pixels; both device-status checks return S_OK. Thus this pattern
is rendered and imported correctly, and the added readback/synchronization
does not repair its sampled output. The remaining rectangle and RGBA cases
are not reached. This narrows the next comparison to sampling/shader state;
it does not establish correct desktop composition.

After reporting the mismatch, the checkpoint process exits `c0000005` during
exception cleanup. The exact UMD binary resolves the fault at RVA `12c00c` to
`zink_bind_vertex_elements_state`, with LR `13910` in `cso_unbind_context`.
The faulting instruction reads an old vertex-buffer resource at offset `e8`.
`IaSetVertexBuffers` previously released frontend references immediately but
deferred the Gallium binding update until another draw. Zink's borrowed binding
can then outlive the resource when ClearState is followed directly by device
destruction. CSO/u_vbuf also retain borrowed bindings, so changing only the
Zink teardown dereference would leave the lifetime error intact.

The correction calls `cso_set_vertex_buffers(..., 0, NULL)` before releasing
old frontend references. That existing helper clears the underlying pipe
binding in both direct and u_vbuf paths. The next draw still installs the new
bindings through the existing dirty-state path. A controlled Gallium backend
now records its borrowed pointer and rejects resource destruction while bound.
On Windows ARM64 `.59` at 21:02:53 UTC, the pre-fix executable exits 1 at the
immediate-unbind assertion and the fixed executable exits 0 through all four
draw paths and teardown. Both logs match device hashes. The fix builds as
UMD `vertex-unbind-20261111-umd+7fd712e9882c`; native confirmation of the
captured exception fix remains pending.

The exact candidate DLL and its newly built shared, primary, Present, adapter,
device and DLL-entry controls pass on `.59` at 21:08:38 UTC. The finalized
controlled trace has 114 valid events with no reported loss, and all returned
files match device hashes. This checks the UMD artifact and controlled callback
contracts, not native rendering or the captured exception path on Zink.

The native task still records its results at 20:52:36. Its finalized ETL has
301,418 exported records and 65,454 valid UMD events, with no reported loss;
68 DWM flip callbacks and two Explorer windowed Presents succeed. No
ZinkFailure, RuntimeError or KMD recovery event is retained. All 154 returned
files (107,256,504 bytes) match remote lengths/hashes, including the seven early
copies. Raw export SHA256:
`209108f152bc658907e7a3ae637bb60d7915af579361828aa9eaaa4450269707`.
An additional startup is observed during recovery at 20:54:34, still `.153`
with the gate closed; its cause is unconfirmed. `.59` is staged at 20:57:26,
boots at 20:57:57, and is verified Code 0 with cleanup and unchanged power at
20:59:39. Saved portrait is unchanged at 21:01:05.

Probe `sampling-variants-20261111`, EXE `3f22d2913f19`, adds
`--shared-layout-variants`. It first verifies the producer, consumer import
and ordinary private copy, then compares four combinations: shared versus
private-copy texture, each sampled with constant-buffer versus literal
coordinates. It retains the original modes and reports every combination
before failing the aggregate pixel check. On WARP at 21:02:57 UTC, the original
mode passes 3,686,400 pixels and all variant checks pass 18,072,582 pixels in
BGRA and RGBA. These controls validate the comparison itself; native variants
and the sampled-content correction remain pending. The KMD and ICD are unchanged.

### Native sampling variants and ordered probe

`sampling-variants153-native-20261111` pairs the fixed UMD `7fd712e9882c`
and probe `3f22d2913f19` from `d65fd675`, with unchanged `.153` KMD
`94bcb716d824` and ICD `6e2fad99d742`. Native LUID `b21ac` reaches Code 0
at 21:15:36 UTC. The original mode again reports 73,584 mismatches in its
first 921,600-pixel layout and exits normally with code 1 at 21:15:58. Its
retained UMD events include both device teardown completions. This is useful
native lifetime evidence, but is not the checkpoint exception path that
motivated the correction.

The variant mode begins at 21:15:59. It verifies all 554,497 producer pixels
and all 554,497 imported pixels, with zero mismatches or zero-alpha pixels.
Its last retained stdout line is successful shared SRV creation. No private
copy completion, four-combination result or final process status survives.
An additional startup at 21:16:28 interrupts the trial. No timeout result is
retained, so neither the 30-second probe bound nor the last successful record
is established as the cause. The native ETL is also unfinalized; missing later
events cannot prove that those operations were never executed.

The first export has 285,388 records and 45,487 valid UMD events, including
43 successful DWM flip callbacks and one successful Explorer windowed
Present. It retains no ZinkFailure, RuntimeError or first-KMD-recovery event.
Its header has EndTime=0 and BuffersWritten=0, with no reported loss; zero
reported loss does not repair the missing finalization. The second export
is finalized with 172,029 records and no UMD events, from the subsequent
closed-gate boot. Its desktop events are not continued Tegra rendering.
The display decoder also retains 1,270 Dwm-Core event-203 payload-length
mismatches rather than interpreting those records with the wrong template.

All 144 returned files (144,521,396 bytes) match device lengths and SHA256.
Raw export hashes are respectively
`f978c77fde3fa1862bbd9b35b5a915b4c5aaa3cea2fc76c2b806e00611bfb5b7`
and `5209cb3d3a24e9009c3adb44221ab19bd0a8a3db03e84326781f295ee73a1e9c`.
The planned setup boots at 21:13:09; the additional 21:16:28 startup precedes
the final `.59` recovery boot at 21:19:49. `.59` Code 0, environment/task
cleanup and unchanged power settings are verified at 21:21:32. Saved portrait
verification is unchanged at 21:23:12. Flashing alone is not a reboot count;
these times come from the operating system's recorded boot identity.

The next probe, `sampling-order-20261111+5fcfd6671788`, defers ordinary-copy
creation until the shared-image constant-buffer and literal-coordinate cases
have completed. It logs each case before its setup and retains all four
combinations and their pixel checks. This improves the order of independent
evidence; it does not fix the sampled pixels or infer a private-copy fault.
On `.59` WARP at 21:24:58 UTC, the original mode passes 3,686,400 pixels and
the variant mode passes 18,072,582 pixels, both exit 0 with empty stderr and
matching returned hashes. The next native trial uses only the variant mode
with a 75-second bound, omits the unrelated 40-second desktop observation,
and keeps the same UMD/KMD/ICD and automatic recovery.

### Native shared versus private sampling

`sampling-order153-native-20261111` uses probe `5fcfd6671788` from
`55c1be85`, UMD `7fd712e9882c`, KMD `.153` (`94bcb716d824`) and ICD
`6e2fad99d742`. Native LUID `b2018` reaches Code 0 at 21:35:25 UTC. The
ordered probe starts at 21:35:45 and records the following BGRA results;
each target readback checks 921,600 pixels.

| Coordinates | Shared image, pass 0 / pass 1 mismatches | Private copy, pass 0 / pass 1 mismatches |
| --- | --- | --- |
| Constant buffer at b3 | 73,584 / 263,560 | 0 / 0 |
| Literal coordinates | 73,584 / 263,560 | 0 / 0 |

The producer, imported image and ordinary GPU copy each pass all 554,497
checkpoint pixels, with no zero-alpha pixels. Four private-copy passes add
3,686,400 correct target pixels. Shared-image sampling fails equally with
both coordinate sources, whereas the same pipeline samples the ordinary copy
correctly. This identifies shared-image sampling as the next boundary; it
does not support changing constant-buffer coordinates as the current fix.

After these comparisons, cleanup again exits `c0000005`, at 21:35:51 UTC,
without timeout. Exact UMD base `00007ff9af150000`, fault RVA `12c020` and LR
RVA `13924` resolve to `zink_bind_vertex_elements_state` and
`cso_unbind_context`. The preceding frontend unbind fix is insufficient for
the real Zink lifetime contract; the controlled backend's immediate unbind
behavior did not cover this retained-binding path. RGBA is not reached.
The native task still writes its final result at 21:35:52, and collection
completes at 21:36:26.

The source comparison finds a concrete image-usage omission. `init_ici`
initializes usage to zero for subsequent format negotiation. The internal
WDDM import branch bypasses that negotiation without supplying usage. Image
views [inherit the image's usage](https://docs.vulkan.org/refpages/latest/refpages/source/VkImageViewUsageCreateInfo.html);
NVK consequently skips its sampled-descriptor creation branch, leaving the
sampled index zero. The correction requests transfer source/destination and
the Gallium resource's sampler/color-target uses before both the format query
and `vkCreateImage`. It preserves the existing pitch, import ownership and
linear/tiled-shadow path. No feature-level, extension or memory-budget change
is involved.

Candidate `shared-usage-20261111-umd+6f77f9f90849` builds for Windows ARM64.
Its shared/primary/Present, adapter, device and DLL-entry controls pass on
`.59` at 21:42:21 UTC, with 114 valid, finalized ETW events and no reported
loss. Those controls check existing callback/loading contracts; they do not
exercise the corrected native image-creation path. Native comparison and
complete desktop composition remain pending, and the cleanup exception is a
separate unresolved issue.

All 158 returned native-trial files (99,970,500 bytes) match device lengths and
SHA256, including five early copies. The finalized ETL has 303,802 exported
records and 42,873 valid UMD events, no reported loss, 45 successful DWM flip
callbacks and one successful Explorer windowed Present. It retains no
ZinkFailure, RuntimeError or KMD recovery event; the process exception above
is retained separately. The decoder leaves 1,402 Dwm-Core event-203 records
uninterpreted because their payload length differs from the available template.
Raw export SHA256:
`280ae3d1d4813caae11679aef74af7296bef1226efe0a2bf877b2a80eba78cf1`.

Only the two planned boots are observed: setup at 21:32:58 and recovery at
21:38:02 UTC. `.59` Code 0, environment/task cleanup and unchanged power are
verified at 21:39:54; saved portrait verification is unchanged at 21:40:21.
No native test remains running. Shell window geometry and successful Present
callbacks still do not establish complete physical taskbar content.

### Buffer-release ownership correction

The native cleanup exception exposes a missing ownership handoff.
`DestroyResource` used `pipe_resource_reference(..., NULL)` on the frontend's
owning buffer reference. Zink can retain a borrowed vertex binding after
`set_vertex_buffers(0, NULL)`, until a later vertex-element change. Its
`resource_release` callback marks a bound buffer for deferred release; the
direct reference decrement bypassed that callback and could free the object
before `cso_unbind_context` inspected the retained binding.

D3D buffer destruction now transfers its owning reference through
`pipe_resource_release` and clears the frontend pointer. The backend retires
that ownership after its remaining uses. This also corrects the earlier
comment that assumed the count-zero binding call immediately removed every
backend reference.

The real DDI fixture now supports both immediate and deferred vertex
unbinding. The latter retains the pointer until vertex elements are unbound,
rejects early destruction, and checks that the transferred reference is freed
during device cleanup. At 21:47:41 UTC on `.59` Code 0, the pre-fix executable
`aa20ebde478f` exits 1 at `resources && borrowed_vertex != resource`; fixed
`e78a960b728c` passes both modes with exit 0 and empty stderr. All returned
outputs match device hashes. This is a controlled lifetime regression, not
native Zink execution.

UMD `buffer-release-v2-20261111-umd+e99798fa9668` includes this correction and
the image-usage fix above. Its ARM64 build and exact-artifact shared, primary,
Present, adapter, device and DLL-entry controls pass at 21:48:39 UTC, with
114 valid finalized trace records and no reported loss. The next native trial
uses the original shared-layout mode followed by the unchanged ordered probe
`5fcfd6671788`; KMD `.153` and ICD `6e2fad99d742` remain the same. Full BGRA/RGBA
pixels, normal teardown and physical desktop content still require native
evidence.

### Native shared sampling and cleanup pass

`shared-usage153-native-20261111` tests the combined image-usage and buffer
ownership corrections in UMD `e99798fa9668` from `931383e6`, with unchanged
KMD `.153` (`94bcb716d824`), ICD `6e2fad99d742` and probe `5fcfd6671788`.
Native LUID `af442` reaches Code 0 at 21:55:00 UTC.

| Native check | Pixel comparisons | Mismatches | Process result |
| --- | ---: | ---: | --- |
| Original layout, without intermediate checkpoints | 3,686,400 | 0 | Exit 0 at 21:55:24 |
| Shared/private-copy with buffer/literal coordinates, both rectangles and formats | 14,745,600 | 0 | Exit 0 at 21:55:28 |
| Producer, import and private-copy checkpoints in BGRA/RGBA | 3,326,982 | 0 | Included in the variant process |

All **21,758,982** comparisons pass. Both processes report successful device
status, release their runtime context/device/adapter and reach their final
success messages. The trace contains six probe device creations and six
completed probe device destructions. Neither process times out or raises the
previous `c0000005`. This verifies the normal native cleanup paths exercised
here and the previous shared-image sampling cases; it is not an exhaustive
failure-unwind or Vulkan conformance result.

The controller observes the native desktop for another 40 seconds and writes
its final result at 21:56:11. DWM PID 492 has one successful device creation,
113 successful resource creations, two shared-image opens and 78 successful
flip callbacks spanning 72.7939875 seconds. Explorer has two successful
windowed Presents. No RuntimeError, ZinkFailure or KMD recovery event is
retained. These callbacks and window geometry do not establish complete
physical taskbar content. The user's later "normal now" report followed the
rollback; they then clarified that they had slept through the native interval
and requested another viewing run.

All 167 returned files (110,632,441 bytes), including six early copies, match
device lengths and SHA256. The finalized ETL has 325,974 exported records and
71,139 valid UMD events, with no reported event/buffer loss. The decoder leaves
1,035 Dwm-Core event-203 records uninterpreted because their payload length
differs from the available template. Raw export SHA256:
`c77eab7b7734a1350166c2e9fbcb8802a0604c272733e8650df64985035b8b2d`.

Collection completes at 21:56:46 UTC. A further startup at 21:57:58 interrupts
recovery, with `.153` still installed and its native gate closed; its cause is
unconfirmed. The repeated startup task preserves the completed test and resumes
rollback. `.59` is staged at 22:00:48 and boots at 22:01:26. Code 0, baseline
hashes, environment/task cleanup and unchanged power are verified at 22:03:37;
saved portrait is unchanged at 22:04:35. No native test remains running.

### Physical desktop observation after the pixel pass

`desktop-observe153-native-20261111` repeats the same KMD `.153`, UMD
`e99798fa9668` and ICD `6e2fad99d742` without running the pixel probes again.
Native LUID `b946a` reaches Code 0 at 22:16:57 UTC. The 120-second physical
observation begins at 22:17:18. The user reports missing, displaced or
disappearing taskbar content during this native interval. Four photos retained
locally show an incomplete taskbar inside the desktop, followed by wallpaper
and icons without a taskbar. Camera orientation is not evidence of a mode
change. **Physical desktop validation fails.**

Of twelve shell snapshots, eleven report a visible taskbar at the expected
bottom rectangle `(0,1232)-(720,1280)`; one reports it hidden with a zero
rectangle. Four distinct Explorer owners appear in sequence:
`2184`, `3344`, `5676`, `3708`. This establishes repeated shell replacement,
not its cause. A read-only query of the enabled Application log finds no
event 1000/1001 between 22:16:30 and 22:20:20; absence of these records does
not establish that the processes never crashed.

The retained trace has 145 successful DWM flip callbacks and five successful
Explorer windowed Presents, with no retained RuntimeError, ZinkFailure or
KMD recovery event. These results coexist with the user's physical failure
and do not override it. The controller's exit 0 at 22:19:30 means the bounded
observation completed, not that the desktop was visually correct.

All 169 returned device files (112,301,472 bytes) match their device lengths
and SHA256. The finalized ETL contains 296,115 exported records and 85,828
valid UMD events, with no reported event/buffer loss. Its 64-MiB circular
capacity has overwritten the setup prefix; the retained UMD interval covers
22:16:41.604440 through 22:19:34.260862 UTC. Do not infer completeness of the
earlier setup from the zero loss counters. The decoder leaves 622 Dwm-Core
event-203 records uninterpreted due to a template/payload-length mismatch.
Raw export SHA256:
`63f4b766d80d1438fc85013897867856a3f9d41221940515d0421eb2b7c5f123`.

An additional startup occurs at 22:20:47 with the `.153` gate closed; its
cause is unconfirmed. `automatic-result.json` contains 81 NUL bytes and
cannot establish collection completion. Recovery stages `.59` at 22:23:35
and boots it at 22:24:04. Code 0, baseline hashes, cleanup and unchanged power
settings are verified at 22:27:50; saved portrait is unchanged at 22:28:33.
This trial is finished. The subsequent archived `.147` comparison is ended
at the user's request after they report an automatic reboot. It supplies no
verified physical desktop pass. All 146 returned files (123,733,692 bytes)
match device hashes; `.59` Code 0, cleanup and unchanged power are verified
at 22:45:26 UTC and saved portrait at 22:45:52. The next investigation remains
on `.153`, with an Explorer exception/termination capture to identify the
shell's exit cause before further code changes.

### Explorer SAMPLE_I abort

`explorer-exit153-native-20261111` retains the same `.153` KMD, `e99798fa9668`
UMD and `6e2fad99d742` ICD. A same-user ProcDump attach/cancel control on
`.59` passes at 22:49:07 UTC with the original Explorer alive and detached.
Native LUID `b2d44` reaches Code 0 at 22:53:22. The capture attaches to
taskbar owner PID 356 at 22:53:45 and records its unhandled `c0000409`
exception, parameter 7, at approximately 22:53:58 UTC. The 18,016,220-byte
minidump matches the device hash and the later complete evidence copy:
`9156ff67a9e7a05c872798a37ea10713310884aafaf651dc77bc04d4b7a823aa`.

The ARM64 frame chain leads from Direct2D/D3D11 into `TegraGpuD3D.dll` and
UCRT. In the exact `e99798fa9668` UMD, return RVA `0x221928` follows a call
to `abort` at `0x221924`. The preceding code prints the unknown TGSI opcode
whose value is retained in `w26`: `0x92`, or `TGSI_OPCODE_SAMPLE_I` (146).
`ShaderTGSI.c` emits that opcode for D3D's integer texture `ld`; the converter
previously handled `SAMPLE` but not `SAMPLE_I`. This directly identifies the
captured Explorer fatal exit. It does not prove the cause of every previous
shell replacement, the absence of Application-log events, or the numeric
meaning of Dxgkrnl's reason-14 device cleanup records.

The correction lowers `SAMPLE_I` to NIR integer texel fetch with an independent
texture-view binding and no sampler-state binding. It preserves integer
coordinates, array layers, the mip in address.w (ignored for buffers), texel
offsets and result swizzle, and records `textures_used_by_txf`. The semantics
follow the [TGSI instruction definition](https://docs.mesa3d.org/gallium/tgsi.html)
and [D3D ld contract](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/ld--sm4---asm-).

Six new translator regressions cover samplerless binding, array coordinates,
buffer LOD omission, offset/swizzle and signed/unsigned result types. Together
with the six existing sampling cases, all twelve pass on Windows ARM64 at
23:08:00 UTC. The pre-fix executable prints `unknown TGSI opcode: SAMPLE_I`
for the same new case, then requires the controller's 15-second timeout stop;
its native process exit status is not presented as a clean reproduction.

The new `--load` runtime probe uses mip 1, integer offsets, out-of-range
coordinates and mip 3, two source patterns, and BGRA/RGBA targets. All 24,576
WARP comparisons pass with exit 0 at 23:08:00. UMD
`sample-i-20261111-umd+8f9a286a0e2b` also passes exact-artifact callback,
resource and loading controls at 23:08:54, with 114 finalized valid trace
records and no reported loss. These are controls, not native Load pixels or
a physical desktop pass; the original known shared-sampling result is kept.

The native capture completes at 22:55:56 and collection at 22:56:35. Its trace
retains 127 successful DWM flips and five Explorer windowed Presents, but no
UMD RuntimeError/ZinkFailure or KMD recovery event; the CPU abort is captured
separately. An additional startup at 22:57:40 interrupts recovery, cause
unconfirmed. `.59` is staged at 23:00:21, boots at 23:00:59 and is verified
Code 0 with cleanup and unchanged power at 23:03:02. Saved portrait is
unchanged at 23:06:09. All 185 files (128,614,924 bytes) match device hashes.

The user separately observes the taskbar near the bottom of an upper 16:9
region in the portrait photo. At width 720, that hypothesis predicts height
405; this is a calculated clue, not a calibrated photo measurement. Check
actual composition size/transform after the shader correction before changing
the scanout registers. The captured abort alone does not explain that position.

A user-requested local Gemini 4 Carbon review returned partial findings after
CLI timeout/stream interruption. The remaining independent-view opcodes and
the D3D-versus-Vulkan out-of-bounds semantics merit targeted follow-up. Its
claim that shared allocations use 64-byte pitch alignment is rejected:
`RuntimeSharedCreate` already uses 128-byte alignment. Its proposed universal
720-wide shadow-copy failure and missing-cache root cause are not established
by the current code or the passing 720x1280 native sampling probe. No such
changes are included in this correction. MSAA and the remaining sampling/query
opcodes remain unfinished; this fix does not claim complete shader conformance.

### SAMPLE_I native retest

`sample-i153-native-20261111` tests UMD `8f9a286a0e2b` and probe `7a9752a1e128`
from `3531f0ee`, with the same `.153` KMD and `6e2fad99d742` ICD. Startup
reaches Code 0 at 23:15:32 UTC on LUID `b02db`. The user reports immediate
black output. An additional boot occurs at 23:16:23 before any native Load
result, observation-start record or debugger attachment is retained. The
absence of those records is not proof that every preceding call was unexecuted.
No native Load-pixel or physical desktop pass is claimed.

The retained native trace has 13 successful DWM flips, seven Explorer
windowed Presents, three TextInputHost Presents and one StartMenuExperienceHost
Present. A DWM-associated device is marked erroneous before `Resource.cpp:358`
reports `8007000e` for a 128x64 BGRA render-target/shader-resource texture;
the same-trace gap is 0.2719613 seconds. That source line maps any failed
Gallium resource creation to generic OOM. Subsequent Explorer/TextInputHost
Zink failures identify timeline-semaphore initialization (`zink_screen.c:3805`).
These are failure boundaries, not proof of memory exhaustion.

All 352 retained `ReserveResource` returns are zero. Before the initial
native device-error mark, observed residency peaks total 149,147,648 bytes:
135,598,080 system-memory bytes, 2,490,368 page-table bytes and 11,059,200
scanout bytes. Residency is not the entire committed-memory accounting, so
these numbers neither prove nor definitively exclude the 512-MiB cap.

The `.153` provider emits five valid recovery records, all paging submissions
with `STATUS_DEVICE_NOT_READY`, in graphics generations 3–7. They occur after
the earlier device-error mark and OOM. This verifies live kernel event delivery,
but the first submission/reset cause remains unidentified. The provider does
not separately record malformed-submit rejection or OS-requested reset; zero
cached FIFO/MMU/PBDMA fields are not fresh hardware observations. Numeric
Dxgkrnl device-error reasons are kept uninterpreted.

The native `.001` is unfinalized and contains 227,894 exported records,
including 23,228 UMD events. The finalized later `.002` has 177,076 records
and zero UMD events; it belongs to the closed-gate recovery interval. The
native decoder leaves 1,187 Dwm-Core event-203 payloads uninterpreted. Raw
export SHA256 values are respectively
`572d46ffd1c5bf88991d03243abd7d2ebb8302305921b93e208235bead327bc5` and
`e179b58f91a0f4ef7dd2f6b43a4071bd0d3ad0624494450c7ac11645bff7bcf3`.

Recovery stages `.59` at 23:19:22 and boots it at 23:20:10. Code 0, baseline
hashes, cleanup and unchanged power are verified at 23:22:05; saved portrait
is unchanged at 23:22:30. All 156 returned files (136,848,242 bytes) match
device lengths and hashes. The requested `.148` comparison is cancelled
before installation. No native test remains running. Continue from the first
device-error boundary rather than treating the later OOM or last trace record
as its cause.

### Queued inherited-state validation

The `.154` candidate corrects admission-time validation in
`TgGpuCaptureContextWork`: an earlier packet can still be queued when the next
packet needs its MME, CB0 or program-region bindings. Checking only the retired
context at that point can incorrectly return `STATUS_INVALID_PARAMETER` for
valid ordered work. The check now runs in `TgGpuActivateContextWork`, after
earlier work publishes its state and before root writes, fence arming or GPU
execution. Packet syntax/address ownership checks remain at admission; missing
bindings and out-of-range inherited state still fail before execution.

The regression queues an actual NVK MME initialization/binding packet followed
by an MME draw before servicing either. It verifies that the second packet
requires inherited MME/CB0/program state, both admissions succeed, fences 300
and 301 complete in order, and context/snapshot ownership is released. Existing
negative cases still reject missing bindings and invalid inherited ranges at
activation. An initial direct-draw test did not exercise the admission check
and passed the old backend; it is not a reproduction of this bug. Its launcher
also failed to retain an exit code, so the retained stdout is not presented as
an exact exit-0 result.

The corrected regression, linked against the old `.153` backend, exits at its
second-admission assertion (line 1789) at 23:47:21 UTC. The new backend passes
the queue regression and existing context controls with exit 0 at 23:47:39.
Both run as user-mode Windows ARM64 models on `.59` Code 0; they do not execute
real GPU work. Executable SHA256 values are respectively
`cda92b214a2952696d20e3e3dafdbf8f5d5955377fd253b9a2282a7e11465a14` and
`076fb1bed47f245f776939064a2c1ae0ca55c556632b7f2fa4ba09d5a0618756`.
Host checks and the full WDK build pass. The `.154` unsigned SYS hash is
`b7a508cb14b27cd345d350795a03aacd7bca191541f3091faebdb78222194d65`.
The native retry below does not establish a visual pass.

A genuinely missing inherited binding discovered after admission now follows
the existing worker-failure/recovery path. It is not retroactively returned as
a malformed-packet error to the submitting device. Mapping checks may reject
such work before activation with `STATUS_INVALID_ADDRESS`; that does not bypass
the later generation check for work that reaches activation or permit GPU
execution. Maintaining speculative queued-state summaries is outside this
correction. The original rejection/reset diagnostic gaps remain open.

At the user's request, the existing local Gemini 4 Carbon conversation is
continued twice with model `gemini-4-carbon-high` and `--effort high`. Both
calls return review text but report backend stream interruption; neither is
recorded as a clean completed review. Independent checking rejects its initial
recommendation to prohibit `STATUS_INVALID_PARAMETER`: Microsoft's documented
[virtual-submit contract](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/nc-d3dkmddi-dxgkddi_submitcommandvirtual)
permits malformed-packet rejection and requires ordered
last-fence accounting, which the existing rejection marker implements without
a fabricated DMA interrupt. Carbon withdraws that recommendation on follow-up.
Its assertion that VidMm never rebinds a system-context root after reset is
unsupported; no root-validity protection is removed. `DMAptr=0` is not proof
of zero DMA length.

The review also identifies a plausible first-error attribution race: the worker
publishes `q->Failure` and releases `DispatchGate` before requesting recovery,
so an arriving submit may report a later `STATUS_DEVICE_NOT_READY` first.
This is a source-derived interleaving, not proof that it occurred in the retained
trace. Neither the review nor the passing queue model establishes the physical
taskbar/black-output cause. Raw review output, the corrected interpretation and
before/after model records are retained with the external build evidence.

### Queued-state native retest

`queued-state154-native-20261111` tests KMD `760d8405`, signed SYS
`dec82546353ea087ec0bab69c48da136a3445c73d0cb96b0a3281e883e2de6d4`, with
the unchanged `8f9a286a0e2b` UMD and `6e2fad99d742` ICD. The closed-gate
installation completes at 23:56:09 UTC, followed by the planned setup boot
at 23:56:53. Native LUID `baf72` is identified in the trace; the controller
records Code 0, consumed permit and hardware status 0 at 23:59:21. An extra
boot occurs at 00:00:09 UTC on November 12. The user did not watch the native
image, but heard a login/desktop sound and suspected a restart. No native Load
result, screenshot, observation-start or debugger-attachment record is
retained. Absence of those records does not prove that preceding calls never
executed. The planned capture procedure is not counted as a completed capture.

The native trace retains 12 successful DWM flips, seven Explorer windowed
Presents, three TextInputHost Presents and one StartMenuExperienceHost Present.
At timestamp `134389151518691212`, Dxgkrnl marks device
`ffffb10d5a1b0660` erroneous; paging-queue events associate it with DWM PID 760.
The numeric reason 7 remains uninterpreted. Its preceding scheduler packet
has submission ID 62 and `pDmaBuffer=0`; that does not establish DMA length or
which DDI rejected it. DWM's 128x64 BGRA resource creation reports generic
`8007000e` **0.2818193 seconds after** the device-error mark.

All 359 retained resource-reservation returns are zero. Before that mark,
observed resident segment peaks total 159,502,336 bytes: 145,952,768 system
memory, 2,490,368 page tables and 11,059,200 scanout. This is not complete
committed-memory accounting and does not establish 512-MiB budget exhaustion.
Five later recovery events report paging-submit `STATUS_DEVICE_NOT_READY`,
in generations 3–7, with fences 824, 861, 943, 963 and 1025. They are later
failure boundaries, not the original cause. The successful queue regression
does not resolve this native failure.

The native `.001` has 232,819 exported records and 23,249 valid UMD events,
but is unfinalized (`EndTime=0`, `BuffersWritten=0`). The decoder leaves 1,193
Dwm-Core event-203 payloads uninterpreted because their length differs from the
available template. The later `.002` has 202,730 records, zero UMD events and
a finalized 530-buffer log; it belongs to closed-gate recovery. Both report
zero event/buffer loss, without establishing a complete interrupted native tail.
Raw export SHA256 values are respectively
`efe79d5a588eb11e41830b2966eea4193cd85b43ca1fa7c03b73e76777051bfd` and
`f78862f023663bd2b198ca20e85d030c0a57b156a30354e778ff5c56f11c5267`.

Automatic recovery stages `.59` at 00:03:13 and boots it at 00:03:47. Code 0,
baseline hashes, removed test tasks/registrations and unchanged power settings
are verified at 00:06:36. Saved portrait verification at 00:07:19 makes no
change. All 159 returned files (140,632,658 bytes) match device lengths and
SHA256. No native test or debugger remains running. Next, capture the exact
malformed-submit substage and OS reset entry/exit, and preserve the worker's
first failure before a subsequent admission can report only device-not-ready.

### Old UMD comparison on .154

`sample-i-ab154-old-20261112` keeps signed KMD `.154` (`dec82546353e`) and ICD
`6e2fad99d742`, replacing only the UMD with pre-`SAMPLE_I` `e99798fa9668`.
Its prepared `8f9a286a0e2b` counterpart uses the same normalized desktop-only
harness. Neither forces a Load probe through the unsupported old translator.
The old case reaches native LUID `bb8de` at 00:32:31 UTC on November 12. The
native display-state record at 00:32:53 reports 720x1280, DPI 96 and identity
rotation; the screenshot stage begins but retains no completed PNG or result.
An extra boot occurs at 00:33:30. The user's photo shows absent right-side
taskbar content; they also report left-shifted central icons and later complete
taskbar disappearance. The photo's precise native/recovery phase is unconfirmed,
and perspective alone does not establish incorrect scale.

The interrupted `.001` contains 257,476 exported records, including 34,411 UMD
events. It retains 44 successful DWM flips spanning 21.512 seconds and one
Explorer windowed Present, with no retained UMD RuntimeError/ZinkFailure or KMD
recovery event. It does not contain the later numeric-reason-7/OOM sequence of
the two new-UMD trials. Missing interrupted records do not prove absence of a
failure, and the reboot alone does not establish that the old and new stacks
failed for the same reason. `.002` is finalized with 169,385 records and zero
UMD events. Export hashes:
`c3ef317361ac0b3a03281661b98154503e678e0a5ba528dc67c43229e6b5262b` and
`c0a1e3f3ef75c0bd1b73578d0a4227a8d4637f492b2237f7b9c5eae96199967e`.

All 162 files (140,255,556 bytes) match device lengths/hashes. `.59` Code 0,
removed trial tasks/registration and unchanged power are verified at 00:47:55;
saved portrait verification at 00:48:32 changes nothing. No native trial remains
running. The prepared new-UMD counterpart is not installed after independent
source review confirms the operand defect below; the A/B experiment is
incomplete and cannot establish causality.

### Sampler-view operand correction

The user-requested local Gemini 4 Carbon High review completes successfully
after continuing the same conversation from an observation timeout, explicitly
using `gemini-4-carbon-high` and high effort. Its strongest independently
confirmed finding is in `ttn_get_src`: unlike SAMPLER/IMAGE/BUFFER operands,
`TGSI_FILE_SAMPLER_VIEW` was not excluded from ordinary ALU-source conversion.
`ttn_emit_instruction` visits all sources before dispatching `SAMPLE` or
`SAMPLE_I`, so the view reaches `ttn_src_for_file_and_index`'s `bad src file`
unreachable branch. Release compilation can optimize undefined behavior in
unpredictable ways; the earlier release-only NIR-structure passes did not
validate this path. This omission predates `3531f0ee` and affects ordinary
independent-view `SAMPLE` as well as the newly supported integer loads.

The correction adds the view to the resource-reference early return; `ttn_tex`
continues to resolve the actual image binding and result swizzle. On restored
`.59`, complete Windows ARM64 `debugoptimized` builds with `b_ndebug=false`
give a direct regression: the old binary prints `bad src file` at line 751 in
`TgsiSample.IndependentBindings` at 00:52:52 UTC and requires termination after
the 15-second post-assert deadline. That is not a clean abnormal-exit result.
The corrected binary exits 0 with all twelve tests passing at 00:53:40, including
separate bindings, high view indices, integer loads, buffers, offsets, arrays
and signed/unsigned results. Executable SHA256 values:
`a906e27f11eea35b09f0b46dbdfb2af344ed684454388f4f775727dd1118c467` and
`71857b6d9148304b228ff59a1400adaf1986448bfd01cc3fdb746ad383bed29d`.
Both tests run entirely in user mode without GPU submission. Release UMD
`77dea046dde0777d6c67e62769c983c5249595a9cc9e3dd7f8323805854f9cdf` builds;
its exact DLL-loading, shared/primary/Present callbacks and device controls
exit 0 at 00:55:26 UTC, with 114 valid ETW events and device hashes verified.
Native validation remains pending. An initial attempt to link only a debug
translator against release NIR fails for missing debug symbols; it is replaced
by the complete debug build rather than by validation stubs.

The review also identifies unfinished MSAA `LD_MS` sample-index handling and
buffer-load interaction with the conditional Zink LOD-robustness pass. Source
inspection confirms those gaps, but no captured desktop shader establishes
their involvement here. They are not changed by this one-line correction.
Context creation flags alone do not prove all Vulkan image robustness disabled,
and passing isolated shader cases would not exclude every desktop workload.
This fix removes confirmed undefined behavior; it does not yet explain taskbar
geometry, establish a native `Texture.Load` pass or repair the physical desktop.

### Sampler-view native retest

`sampler-view154-native-20261112` tests corrected UMD `77dea046dde0` from
`ee7b0d50`, unchanged signed `.154` KMD `dec82546353e` and ICD `6e2fad99d742`.
It schedules the unchanged Load probe before geometry/PNG capture and desktop
observation. It is not the unstarted desktop-only A/B counterpart. The planned
setup reboot boots at 01:00:30 UTC; native LUID `bcf28` reaches Code 0 at
01:02:54. The user reports black output followed by an automatic restart;
the extra boot is recorded at 01:03:41. No native Load result, screenshot,
mode-stage or debugger attachment record is retained. Missing tail records
do not prove that every preceding call was unexecuted.

The native trace retains ten successful DWM flips, six Explorer windowed
Presents, three TextInputHost Presents and one StartMenuExperienceHost Present.
DWM PID 780 is associated with device `ffffe089b8f79050` by 208 paging-queue
records. Its device-error mark is at `134389189556613319`, before the controller's
Code-0 completion and **0.2907953 seconds before** the generic `8007000e`
resource-create failure. All 378 retained resource-reservation returns succeed.
Pre-error observed resident peaks total 161,796,096 bytes, not the complete
committed-memory accounting. The unchanged 512-MiB ceiling is not established
as the cause.

A preceding `DmaPacket` start at `134389189535701058` has packet type 3 and
submission ID 62. Microsoft's public
[DXGKETW_DMA_PACKET_TYPE](https://learn.microsoft.com/en-us/previous-versions/hh706296(v=vs.85))
defines type 3 as a system-preemption buffer; its
[DMA-submit event contract](https://learn.microsoft.com/en-us/previous-versions/hh706295(v=vs.85))
also reserves `pDmaBuffer` for system use, so zero is not evidence of zero
command length. This start is **2.0912261 seconds before** the first device
error. Prior preemption IDs 59–61 have completion records, whereas 62 has none
before that mark in the retained stream. This narrows investigation to the
active-work/preemption/reset boundary; it does not identify what stalled the
GPU or prove a faulty preemption implementation. Numeric device-error reason
7 remains uninterpreted.

Five later recovery records are again paging-submit `STATUS_DEVICE_NOT_READY`
in generations 3–7, with fences 774, 804, 870, 902 and 979; they do not identify
the initial fault. The unfinalized native `.001` has 246,944 exported records
and 23,268 UMD events; the finalized recovery `.002` has 152,434 records and zero
UMD events. Both exporters report zero loss without proving a complete native
tail. Raw export hashes:
`a69cc07cd154daf2b83da7d25b8a56b7db76668c694481686a2ac50a95257651` and
`39c9b7994d1286212df0d03961430309f03ce43aa60f600b5854fec3841089bc`.

Recovery stages `.59` at 01:06:36 and boots it at 01:07:09. Code 0, cleanup and
unchanged power are verified at 01:09:49; saved portrait verification at
01:10:25 makes no change. All 158 files (133,358,153 bytes) match device hashes.
The recovered software desktop separately lacks its taskbar until Explorer is
restarted; [before/after PNGs](DESKTOP_CAPTURE.md#recovered-shell-with-missing-taskbar)
and the user's confirmation establish that shell recovery at 01:15:31.
No native GPU or helper task remains active. The compiler defect is corrected,
but native desktop composition and `Texture.Load` pixels remain unverified.
