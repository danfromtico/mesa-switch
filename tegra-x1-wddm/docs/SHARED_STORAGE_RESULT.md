# KMT shared-storage result

2026-11-10, Switch Lite HDH-001, Windows 11 ARM64 23H2 (22631.2861).

**.132 passes NT-handle storage sharing and CPU readback** on GM20B LUID
`00000000000949ff`. A 16 KiB allocation remains readable by the importing
KMT device after the producer releases its allocation and after the
exported NT handle is closed. All 4,096 words match at each ownership stage.
Cleanup succeeds and both device budget counters return to zero.

This probe uses two devices in one process on the same GM20B adapter. It
maps both GPU addresses but submits no drawing commands. The later .144
trial adds Vulkan OPAQUE_WIN32 entry points and passes GPU buffer/image
sharing, producer-device destruction and re-export. Cross-process sharing
and D3D UMD/DWM composition remain pending.
[Later Vulkan sharing result](VULKAN_WIN32_MEMORY.md)

## Physical checks

| Check | .132 observation |
| --- | --- |
| Driver and identity | Code 0; KMT and active VidPN source/target use LUID `949ff` |
| Budget | 536,870,912 bytes, unchanged |
| Device setup | Two KMT devices, paging queues and virtual contexts succeed |
| Storage | Section-backed standard allocation, export, import and residency succeed |
| Addressing | Producer and consumer GPU VA reservations/mappings succeed |
| Readback before producer release | 4,096 matching words |
| Readback after producer release | 4,096 matching words |
| Readback after exported-handle closure | 4,096 matching words |
| Cleanup | All calls succeed; both local budgets return to zero; native exit code 0 |
| Independent KMD records | Standard GDI byte buffer: width 16,384, private data 16 bytes; creation and description succeed; description is `UNKNOWN`, width 16,384, height 1 |
| Separate GDI execution | Fence 23, Reference 5, completion/CPU fence readback/retirement mask 7 |

The separate GDI record covers the merged command-validation path. This
round does not repeat the .130 visible-pixel capture or establish continuous
DWM refresh. The exported ETL contains 93,873 records; its count and every
record boundary match the export header.

## Contract and implementation

On the tested Windows kernel, the GPU-MMU-only adapter's imported ordinary
shared allocation is rejected before VidMm's CPU lock. The existing-section
path retains CPU access across the import. Read-only analysis used the
installed `dxgkrnl.sys` and matching public symbols, with device/local SHA256
`99d425e1c997eae32909fa4f37d6c8e2e846a5374c602295546c8be2547aa5e6`.
The implementation uses public SDK calls and does not access private kernel
structures or advertise system-IOMMU support.

CPU-visible shared allocations now create a write-combined section and
pass it through `D3DKMTCreateAllocation2` as a standard `EXISTINGHEAP`, with
`ExistingSection`, `CrossAdapter` and NT security sharing. The temporary
section handle closes after creation; VidMm owns the backing reference.
Ordinary allocations and GPU-only shared allocations retain their existing
private-allocation path. Import still validates the raw storage metadata
and uses separate local resource handles and per-device budget accounting.
[Allocation creation flags](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmthk/ns-d3dkmthk-_d3dkmt_createallocationflags)
[Standard allocation](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmthk/ns-d3dkmthk-_d3dkmt_createstandardallocation)

The KMD supplies the standard raw-buffer descriptor and describes it in
bytes when Windows queries the cross-adapter allocation. It also validates
linear surface geometry, 128-byte pitch and four-row height alignment,
separately from the fixed-panel primary format. The memory caps declare
cross-adapter copy support; texturing and scanout tiers remain unset.
Linear image formats have model coverage; the physical shared-storage test
uses only the untyped buffer case. Both display and MCDM entry points route
standard-allocation requests to the common handler; only the display
profile was physically tested in this round.
[Cross-adapter layouts](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/using-cross-adapter-resources-in-a-hybrid-system)
[Capability tiers](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/supporting-caso)

## Validation and preceding trials

The full Windows ARM64 NVK ICD builds. The SDK-controlled KMT fixture passes
on the .59 baseline, checking write-combined section mappings, handle
release, repeated imports, budget rejection and failed-residency cleanup.
The WDK fixture passes with the standard-buffer, surface-layout, buffer
description and fixed-primary regression checks. The host models also pass.
These fixtures substitute GPU/KMT operations and are distinct from the live
storage result above.

| Trial | Finding |
| --- | --- |
| .130 | Ordinary shared storage exports/imports and maps GPU VA, but imported CPU lock returns `c000000d`, including before producer release. The second run exits 3; the first wrapper's exit field was null. |
| .131 | Section-backed creation returns `c00000bb`. Code 0 and GDI fence 14/mask 7 still pass. The normal journal fills, so it does not identify the failing storage callback. |
| .132 | Adds the missing raw-buffer description and dedicated sharing callback records. The complete storage/readback test exits 0. |

The lfrb `wddm2` reference at `801c9763c6043f0de8408e905a5324eea06d81d7`
provides useful KMT resource-opening and residency sequences. Its AMD
metadata and CPU-access assumptions are not substituted for Tegra's ABI.
[RADV WDDM2 transport](https://gitlab.freedesktop.org/lfrb/mesa/-/blob/801c9763c6043f0de8408e905a5324eea06d81d7/src/amd/vulkan/winsys/wddm2/radv_wddm2_bo.c)

## Recovery and evidence

Each trial uses one desktop-start grant and a 12-minute rollback. The
rollback was started early after the .132 evidence was downloaded. The
16:31:27 UTC check confirms .59 Code 0, the original driver and ACPI hashes,
no start permit and no remaining test tasks. Before/after power reports are
byte-identical: AC/DC display and sleep timeouts remain zero. Windows
restarted directly into Windows; firmware and Hekate configuration were
not changed.

Local .132 artifacts are under
`/private/tmp/t214-gpu-round/shared-section132.vy36c52p/`;
.131 artifacts are under `shared-section.bkpxtak7/` in the same parent,
and .130 sharing artifacts under `nvk-shared.qj2IaJ/`.
The 22 downloaded .132 test records and five recovery records match the
device SHA256 and lengths in `physical/verified.json` and
`physical/verified-recovery.json`. `physical/returned/` retains the native
logs, startup record, ETL export, identity and recovery records.

| Artifact | SHA256 |
| --- | --- |
| .132 signed KMD | `8bdfbf9dc2de6c6bae09a40a5c148e9c6bb3213fe6f366c58f826b37acdf1270` |
| .132 live storage log | `3a67aa572884eaaaf5977e3b5870bce9dadefe6b14a93ca8eca08ae508a976fd` |
| .132 startup record | `3c467759362e3065b350c586a7861a3fc9579c684dfa32e44842d22239c390b8` |
| .132 ETL export | `735a5bef7a853aa9e735aadd91a0be76d3f265e98e1e9beb2c4a097cd244a22d` |
