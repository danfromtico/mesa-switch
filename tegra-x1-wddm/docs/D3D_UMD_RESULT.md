# NVK-backed D3D UMD bring-up

2026-11-10, Windows 11 ARM64 23H2 on Switch Lite HDH-001.

**.143 passes three native Windows D3D11 runtime runs with the barrier fix.**
All use feature level 10_0 on GM20B LUID `a59ef`, with RGBA/BGRA clear/draw/
readback and original implicit release. Each checks 16,384 pixels and exits
0 without a timeout, for 49,152 matching pixels total. CPU-only controls
reproduce the old barrier errors and pass after the source fix. The ICD and
KMD core are unchanged. See [runtime lifetime evidence](D3D_UMD_LIFETIME.md).

The earlier .140–.141 results below precede that fix. Shared resources, DWM
composition and Win32 Vulkan presentation remain unverified.

**.140 reaches native Windows D3D11 runtime rendering before the barrier fix.**
The runtime creates a feature-level-10_0 hardware device on GM20B LUID
`9acf3`, loads the staged `TegraGpuD3D.dll`, and preserves that LUID on the
resulting DXGI device. RGBA and BGRA each pass a blue clear and an SM4 red
triangle with staging readback: four 64x64 images, 16,384 matching pixels.
Each triangle has 1,152 red and 2,944 blue pixels at the expected positions.

| .140 runtime run | Result |
| --- | --- |
| 19:37:35–19:37:46 UTC | All four images match and GetDeviceRemovedReason returns S_OK; process subsequently exits `c0000005`, without timeout. |
| 19:40:49–19:41:03 UTC | Same UMD/ICD, with exception-address reporting and explicit context/device/adapter release markers in the probe. All four images match, all release markers complete, exit 0 without timeout. |

The second run did not explain or establish a fix for the first access
violation. No exception address was captured in the first run, and the
diagnostic run did not reproduce it. The Application log query for events
1000/1001 naming the probe or UMD returned no matching events. The later
.142 investigation and .143 fix are linked above.
These tests do not present windows or validate composition.

The first probe attempt stopped before D3D11CreateDevice because DXGI
reported ACPI identity words `414f5753:31304630` (`SWOA0F01`), rather than
PCI vendor/device IDs. Selection now uses the requested LUID; the native
UMD independently validates the private KMD's NVIDIA/GM20B identity.
There is no automatic software fallback. The retained first native fence
`7a`, 8,532 bytes, completes with Reference `5c` and proof mask 7. Those
fixed journal slots belong to the first rendering run and remain unchanged
in the diagnostic run; the pixel comparisons provide the latter's evidence.

**.139 passes native direct-DDI application-shader drawing on GM20B.**
The explicit direct-DDI probe selects matching KMT/active VidPN LUID `a2dc3`,
opens the NVK UMD device, and checks every 64x64 RGBA pixel after three clear
passes and two triangle passes. It runs 19:15:28–19:15:41 UTC and exits 0
without a timeout: 20,480 checked pixels, zero mismatches.

| Pass | Expected image | Checked pixels | Mismatches |
| --- | --- | --- | --- |
| Clear red | 4,096 words `ff0000ff` | 4,096 | 0 |
| Clear green | 4,096 words `ff00ff00` | 4,096 | 0 |
| Clear blue | 4,096 words `ffff0000` | 4,096 | 0 |
| SM4 Draw | 1,152 red and 2,944 blue pixels | 4,096 | 0 |
| SM4 DrawIndexed + scissor | 736 red and 3,360 blue pixels | 4,096 | 0 |

Both triangle checks compare pixel positions, not just color counts. The
probe reuses the existing Mesa/apitrace SM4 passthrough shaders and supplies
real vertex/index buffers. Pixel centers do not lie on triangle edges;
the indexed pass applies the rectangle `[16,48)` on both axes. All mappings
have row pitch 256. The ordinary and indexed shader paths are physical
results; instanced draw routing has controlled-fixture coverage only.

The driver is Code 0 and reports a 512 MiB ceiling. Its first native fence
`3f`, 8,532 bytes, completes with Reference `21` and proof mask 7. The journal
has no recorded recovery/rejection/MMU fault; the pixel comparisons establish
the drawing result. The KMD core and ICD are unchanged from .138. The UMD
now uses CSO/u_vbuf to prevent CPU dummy vertex bindings reaching Zink.

This earlier result is an offscreen direct-DDI test. Runtime resource
sharing, DWM composition and Win32 Vulkan presentation remain unverified.
.137 remains the earlier native three-color clear/copy/readback baseline,
including the QMD writable-mirror fix described below.

## Barrier rejection and execution fault

.134 uses matching KMT/VidPN LUID `9d74b`. Native fence `32` completes with
Reference `14` and proof mask 7; fence `47`, 592 bytes, is rejected with
`c000000d`. The dump has 148 DWORDs. DWORD `0x92` is `0x80000014`, immediate
SET_REFERENCE(0), from MME-data invalidation. KMD reserves that register
for retirement.

Replaying this packet reproduces the rejection. Changing only DWORD `0x92`
to `0x8001001e` (WFI/SCOPE_ALL) passes unchanged KMD preparation, including
the inline QMD relocation. The command-stream regression preserves the
captured words and checks both outcomes and the relocated QMD. Windows NVK
now shares that wait emitter across barriers, query reset and indirect
commands. Non-Windows SET_REFERENCE and Hopper/Horizon special cases remain.
[NVIDIA B06F definitions](https://github.com/NVIDIA/open-gpu-kernel-modules/blob/main/src/common/sdk/nvidia/inc/class/clb06f.h)

.135 uses matching LUID `98054`, the same KMD core and a 512 MiB ceiling.
Its 592-byte dump differs from .134 only in the barrier DWORD. Initial
native fence `2e` completes with Reference `10` and proof mask 7. First
worker recovery is fence `43`, `c000009c` (STATUS_DEVICE_DATA_ERROR), FIFO
interrupt `10000000`, MMU mask `1`, PBDMA interrupt zero. Engine reset
returns `c0000001`; adapter reset returns success. A later boot at
18:01:40 UTC gates the driver and uses the fallback display path. There is
no native-rejection record; absence alone does not prove every submission
was accepted or completed. Push dumps also include later buffers in a batch
whose execution may have stopped early.

The test runs 18:00:49–18:00:58 UTC, exits 1 without timing out, and reports
VK_ERROR_DEVICE_LOST from command-buffer finalization. Mapping returns
`88760870`, as in .134, instead of exposing stale data. Neither completes
a pixel-check pass. The first ETL export has 97,813 records with matching
count/boundaries, but its retained events end before the native test starts.
The worker journal supplies the fault evidence; the ETL tail is not used
as the failure location. The fallback export has 88,774 records, also with
matching count/boundaries.

The optional `NVK_DEBUG=log` enables Mesa Vulkan diagnostics in release
builds. .136 physically verifies it: after device removal, Lock2 and
allocation/residency return `c00002b6`; the log identifies the source lines
that propagate it into command-buffer finalization. Those calls are later
symptoms, not the original MMU fault.

.136 runs 18:14:32–18:14:41 UTC on matching LUID `97b6f`, exits 1 without
timing out, and preserves the first worker fault at fence `7d`: address
`04004000`, MMU info `1dc6`, instance `fc241` (physical `fc241000`), FIFO
`10000000`, MMU mask `1`, PBDMA zero. The early journal returned in the same
SSH response has SHA256
`f77eeddd39c61b92a9a8e1fbb8c42c292b2e15959666aef761e3c6e9ee91172a`,
matching the later device-hashed download. Windows boots again at
18:15:21 UTC and gates the test driver.
[nouveau MMU field decoding](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/nouveau/nvkm/engine/fifo/gf100.c)
[Access, client and reason names](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/nouveau/nvkm/engine/fifo/gk104.c)

The .137 KMD fix keeps the aligned, checked QMD in the immutable CPU snapshot,
but PCAS points into a separate GPU-writable mirror. The serialized worker
refreshes that mirror before execution; command pages, ring and USERD keep
their existing protections. Host tests and the ARM64 KMD build pass. The
controlled context fixture passes on the Switch, including the distinct
command/QMD PTE permissions. The .137 native test passes as described above.

## Application draw path: .138

.138 repeats all three clear/readback passes on matching LUID `a0a94`,
then exits with `c0000005` in its first Draw. The diagnostic repeat records
UMD fault RVA `1256e4` (`zink_set_vertex_buffers_optimal+268`), with caller
RVA `2518` in `ResolveState`. The instruction writes the resource's vertex
binding mask. D3D's unused vertex slots hold CPU-backed zero arrays, but
the frontend disabled CSO's vertex-buffer fallback and forwarded those
pointers directly to Zink as if they were GPU resources. Clear background
and shader/resource/state creation had returned success before the fault.

The frontend now enables Mesa's capability-driven `u_vbuf` handling, binds
vertex elements/buffers together, and routes all five Draw entry points
through CSO. The extended ARM64 controlled fixture rejects CPU pointers at
its hardware-backend boundary and passes ordinary, instanced, indexed and
indexed-instanced draws plus cleanup. .139 physically verifies ordinary
and indexed/scissored draws as described above.
Both the native and GDI/softpipe UMD targets build.

.138 artifacts are under `/private/tmp/t214-gpu-round/native-ddi138.wts1cb4o/`:
31 test records and seven recovery/model records match device SHA256/lengths.
`diag-result-summary.json`, `diag-push.txt`, `umd-symbols.txt` and
`vbuf-model-result.json` identify the failure and subsequent controlled test.
The device was restored to .59 Code 0 at 19:04:23 UTC with original driver/
ACPI hashes, removed test tasks/permit/ICD registration and unchanged power
settings. .139 artifacts: `/private/tmp/t214-gpu-round/native-ddi139.mp3g_eov/`.

## Earlier physical evidence: .133

| Check | .133 observation |
| --- | --- |
| Identity | Code 0, matching KMT and active VidPN LUID `9493e`; native-submit capability set; 512 MiB ceiling |
| Driver selection | Loader selects the staged `libvulkan_nouveau.dll` and names the NVK GM20B device |
| UMD setup | OpenAdapter10_2, CreateDevice, render-target/staging allocation and render-target-view creation succeed |
| First captured native submission | Fence `0x14f`, 8,532 command bytes |
| First captured native completion | Fence `0x14f`, Reference `0x131`, completion/CPU-fence-readback/retirement mask 7 |
| First captured native rejection | Fence `0x163`, 592 command bytes, status `c000000d` |
| Clear/readback | Queue submit reports device loss; 4,096/4,096 first-pass pixels mismatch; probe exits 1 without timing out |
| Separate GDI execution | Fence 14, Reference 5, proof mask 7 |

The dedicated native records survive the normal startup journal filling.
In this diagnostic package, their `Flags` field stores the submission fence;
their last data field stores byte count or completion proof. The completion
preceding the rejection does not prove the failing clear/copy succeeded.

The exported ETL contains 137,700 records, with matching header count and
record boundaries. Its Tegra adapter report has LUID `9493e`; initial
Microsoft adapters use `4d7e` and `4db7`. The persistent native rejection is
the specific failure evidence. Missing or last-visible ETL events are not
used to identify an unrecorded internal failure.

## Loader discovery

The next-stage `nvk_d3d_runtime_check.exe` uses the Windows D3D11 runtime at
feature level 10_0, with RGBA and BGRA clear/draw/staging-readback checks.
On the .59 baseline at 19:27:41–19:27:42 UTC, both explicit software controls
pass: WARP and Mesa's GDI/softpipe UMD each check 16,384 pixels with zero
mismatches, exit 0, and no timeout. This validates the runtime probe and
the shared D3D frontend against Windows; it is not native GPU evidence.
The hardware mode requires a selected adapter LUID and the loaded native
UMD, with no automatic software fallback. Shared resources and presentation
remain outside this probe's scope.

Control artifacts: `/private/tmp/t214-gpu-round/runtime-d3d.m5l_i8vc/`.
All five returned records match device SHA256/lengths in `verified.json`.
The probe SHA256 is
`981ea2b71fdc1df0bfe90e836c184ed121c2eb937e8844a921f768ee3170b9ec`.
Read-only inspection found no `System32/vulkan-1.dll`; prior diagnostic
packages registered the no-D3D UMD. The .140 trial registers the native
UMD for D3D10/11 and uses the app-local loader for the runtime probe.
It does not yet package a system loader for DWM.

Initial runs could not find an ICD. The loader later explicitly reports that
these Windows processes have elevated privileges, including the task started
with a Limited principal, so it ignores `VK_DRIVER_FILES`. Registering the
ICD manifest as `VulkanDriverName` in the Tegra device's software key allows
discovery and native device creation. The test records the previous absence
of that value and adds its removal to rollback. Production packaging needs
the corresponding driver-package registration.
[Khronos driver discovery and privilege rules](https://github.com/KhronosGroup/Vulkan-Loader/blob/main/docs/LoaderDriverInterface.md#exception-for-elevated-privileges)

## Source changes and validation

The KMD private query now derives `NATIVE_SUBMIT` from its actual paging-only
mode and requires initialized adapter identity. The main miniport remains
paging-only; .133 explicitly enables application submissions in its test
entry point. The WDK capability fixture passes on the .59 baseline.

The original .133 `nvk_d3d_check.exe` creates a 64x64 RGBA render target and a
staging image, initializes readback with a different sentinel, and checks
every word after clear/copy/map. It stops on the first failure; .133 covers
only that failed first pass. .139 extends this probe with the two passing
shader draws reported at the top of this document.

This run also exposed a UMD error-propagation problem: a mapping returned a
host pointer after the asynchronous backend had lost the device. The
frontend now checks Gallium reset status before and after mapping, reports
`D3DDDIERR_DEVICEREMOVED`, and unmaps a newly returned transfer instead of
exposing stale data. The extended controlled device fixture passes on the
Switch's .59 baseline, including removal during a successful backend map.
The ARM64 NVK UMD and GDI/softpipe target build. .134 and .135 also confirm
removal is reported at map; neither is a native readback pass.
[ResourceMap contract](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/nc-d3d10umddi-pfnd3d10ddi_resourcemap)

The earlier adapter, DLL-entry and device fixtures passed with commit
`035062b0`; their captures and tested hashes remain under
`/private/tmp/t214-gpu-round/d3d-umd.2huxro_6/returned/` and
`staged-hashes.json`. The frontend still negotiates D3D10 DDIs only, and its
native presentation callbacks remain unsupported.

Next, connect runtime allocations, shared textures, synchronization and
sustained DWM presentation. DXVK
remains a compatibility/code-reuse reference.

## Recovery and artifacts

The .143 artifacts are under `/private/tmp/t214-gpu-round/runtime-ddi143.rvqimgbg/`.
All 38 test records and five recovery records match device SHA256/lengths.
The exported ETL has
133,424 records with matching count and boundaries. The patched UMD is
`fec294792c0226a282e2aa9ebf96b0dc97719059e74e2327c48a586d3ecd9d95`;
the native probe, ICD and KMD core are unchanged from .142.
.59 Code 0, original ACPI/driver hashes, removal of temporary UMD/ICD
registration and test tasks/permit, and unchanged power settings were
verified at 21:14:45 UTC after .143.

| .143 artifact | SHA256 |
| --- | --- |
| Signed KMD | `17aa5bf2a4cbf57dc641edf179119677991790ec6d58cdaa9d19c6edf8c14544` |
| First native run | `3d9f034d28e9513424373239b1d72a6d483846e600c14221c71ce9efcaebafa7` |
| Second native run | `4b1f4dfcf699d8cf0b7f01b80ca61526ba38d0cd422ffffb9f08fbcf1bb1ecb2` |
| Third native run | `89492cd813d04aa580cee9ced25934ab51a7ecd637b1ef72722ef0bb28fc8bf5` |
| ETL export | `8f793f2b8d2ab4f91de44c1e9a6987483bea61665f51b24a44e457335373ae1e` |

The .140 artifacts are under `/private/tmp/t214-gpu-round/runtime-ddi140.hvoc0ueo/`.
All 39 downloaded test records and five recovery records match device
SHA256/lengths in `physical/verified.json` and `physical/verified-recovery.json`.
The ETL export has 158,904 records with matching
count and record boundaries. `initial-app-manifest.json`,
`identity-app-manifest.json` and the final local `app-manifest.json` preserve
the three probe builds; the UMD/ICD are unchanged across them. The final
probe SHA256 is `4662698f72bee8aa319513a492c4db3aa4f01256df34f97a586d3bb2c8bf22f0`,
verified on the device before its successful diagnostic run.

| .140 artifact | SHA256 |
| --- | --- |
| Signed KMD | `70585c1b1cde2155a75ea95d5c62fe5df634e8208d1be8c45147f410858e0a76` |
| First rendering run, exit-path fault | `a23e69d659ff4b2cbd9ed413d0d404ac0cf12f5c4e28a16205848d8f8bf653a5` |
| Diagnostic rendering run, exit 0 | `991fe93527aab233997de6b292c410a119066a99b57f65e68dc9c0cb4a1c0964` |
| Live startup journal | `08817ee6b0ba39dbfbc04a3fceff7a62f81a35a90258e6874c8a145ff6ed96b3` |
| ETL export | `7bb1b07f6333acf63ac409729fea319926f1e042bfc455eaf37e7e387a65df6d` |

.134 through .140 were restored to .59 Code 0, most recently at 19:46:06 UTC.
Original ACPI/driver hashes match, tasks/permit and temporary UMD/ICD registration
are gone, and before/after power reports match. Firmware/Hekate were unchanged.

The .139 artifacts are under `/private/tmp/t214-gpu-round/native-ddi139.mp3g_eov/`.
All 25 test records and five recovery records match device SHA256/lengths.
The ETL export has 107,298 records with matching count and record boundaries.
`app-manifest.json` identifies the tested UMD, ICD and probe; the controlled
vertex-input regression is retained in .138's `vbuf-model-result.json`.

| .139 artifact | SHA256 |
| --- | --- |
| Signed KMD | `24808e81100e943fa7a655e9d626f3f88fc753ab18ff5ae07bc35756c94b93d3` |
| Passing native test result | `ff6b3a77f54e414f15b3b1250aa09cc363dc95f7e7e0181e976529cefc61a642` |
| Early/live startup journal | `bf6c2f1ac77ca351a70842a7850e3ecfab1702e130adc490c0e5864068bfdde7` |
| ETL export | `cce54b27c6b7b85d96d5519b9367eba5e555d3429e15d8200825bea3879e8b6d` |

Artifacts: `/private/tmp/t214-gpu-round/native-ddi134._opeardc/` (24 test and
five recovery records) and `/private/tmp/t214-gpu-round/native-ddi135.3m6lfgsj/`
(25 test and five recovery records). Each downloaded record matches the
device SHA256/length in `physical/verified.json` and
`physical/verified-recovery.json`. `app-manifest.json` identifies tested
binaries; .135's `icd-with-log/` is a later build, distinct from its tested
`app/`. .136 artifacts: `/private/tmp/t214-gpu-round/native-ddi136.govmyen6/` (26
test records and seven recovery/model records match device SHA256/lengths).
QMD implementation and its controlled model are in `qmd-wip/` and
`qmd-model-result.json`.

The .137 artifacts are under `/private/tmp/t214-gpu-round/native-ddi137.5ktxn6se/`.
All 25 test records and five recovery records match device SHA256/lengths.
The ETL export has 103,455 records with matching count and record boundaries.

| .137 artifact | SHA256 |
| --- | --- |
| Signed KMD | `af5ef53a7ea4803d1c8127265704cfbe6098422cfd8b36f47817db1ceb145b39` |
| Passing native test result | `d404120b7a31f6564b3af562ded2ef82d641a6754171a75c65ffdd4555b5496a` |
| Early/live startup journal | `6b15b8b95bef0c05fd1f6653d29576a4fe2dc3ca321aeb31cab318c88915b7c5` |
| ETL export | `5eed92183066fafe6c2fa97564f1f260e2fd760a97597a9c900c118f2dd8e97e` |

Earlier .133 artifacts are under `/private/tmp/t214-gpu-round/native-ddi133.q2b6fbs1/`.
All 39 downloaded test records and five recovery records match device SHA256
and lengths in `physical/verified.json` and `physical/verified-recovery.json`.
`app-manifest.json` identifies the exact physical-test binaries;
`umd-after-loss/` contains the subsequent mapping-error fix and fixture.

| Artifact | SHA256 |
| --- | --- |
| Signed .133 KMD | `90d402f7540d4219ddccbd6fbf975ba3d2306e1abf07c1ead448d93fd1503d43` |
| Registered-ICD test result | `37d1135f0f98b8b9e1b9a8401f24b1462b9be67a33d1e5e2a9ed9ac14a89538e` |
| Live startup journal | `a4b4b99683e6f46c671e8f88873ba7af2bd304bfe9e9ac6d9ae9fe47edd032a1` |
| ETL export | `ebec876c50251219cfd67e5536f39fb24becc0d9fecbfd820bf7c78bf1a20b5d` |
