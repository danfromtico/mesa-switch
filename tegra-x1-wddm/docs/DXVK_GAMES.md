# Direct3D 9/10/11 applications through DXVK

On Windows ARM64, the game loads DXVK DLLs beside its executable. DXVK creates
a Vulkan device through the process-compatible NVK ICD, which submits work to
the ARM64 GM20B WDDM kernel driver. Keep Windows' Direct3D and DXGI libraries
in their system directories. The desktop uses its registered tgd3d UMD.

On a Switch Lite, the D3D11 Utah teapot ran with the desktop x64 and ARM64EC
DXVK packages. Each run matched the DXGI and Vulkan adapter LUID and identified
Mesa NVK on NVIDIA `10de:0fe0`. Space paused the object, Right turned it, and
Escape closed the window with exit code zero. At 384 × 256, the two static
desktop captures per run matched GPU readback to within two levels at 98,268
of 98,304 pixels. The remaining 36 pixels were at the rounded window corners.
These captures measure DWM output. [Teapot measurements](dxvk-games-20261118/teapot.json)

Both 32-bit and 64-bit vulkaninfo reported Vulkan 1.4.354, 164 device
extensions and 211 formats. The reported properties meet DXVK's D3D9,
D3D10.1 and D3D11.0/11.1 baseline profiles. Zink's published GL 2.1 profile
requires `borderColorSwizzleFromImage`, which GM20B NVK reports as false.
Zink has a texture-border workaround. The signed x64 WGL frontend created a
GL 3.2 compatibility context in FTEQW. The game and the profile comparison
exercise different capabilities. [x64 export](dxvk-games-20261118/vulkaninfo-x64.json)
and [x86 export](dxvk-games-20261118/vulkaninfo-x86.json). [Profile comparison](dxvk-games-20261118/profile-comparison.json)

After the application ICDs began enumerating GM20B by default, FTEQW loaded
the LibreQuake map `lq_e0m1` through x86 and x64 DXVK, x86 and x64 Zink,
and ARM64EC DXVK in the x64 game. All five runs acknowledged input, changed
the captured world, exited with code zero and returned to the same desktop.
The earlier x86 DXVK run had reached the map before Windows restarted.
[Five game and FPS measurements](DXVK_ZINK_GAME_MATRIX_20261118.json)
and [earlier D3D9 measurements](dxvk-games-20261118/librequake-dxvk.json).

The component manifest records SHA-256 for each Windows vulkaninfo export with
its original CRLF endings and for each LF copy in Git. The profile comparison
uses the Windows hashes. [Component hashes](dxvk-games-20261118/components.json)

The signed KMD at source `001ffd4b` passed native Vulkan transform-feedback
capture and resume on all four output slots in ARM64 and x64 processes.
[Kernel measurements](GM20B_XFB_COUNTER_BASELINE_20261118.json)

On x86, x64 and ARM64EC DXVK, an explicit triangle geometry shader wrote all
six expected vertices. `DrawAuto` rendered all 4,096 pixels correctly. When
given the vertex shader directly, the same desktop DXVK packages wrote only
the first vertex of each triangle. Both variants reported two primitives.
Microsoft WARP on x86, x64 and native ARM64 produced the expected six vertices
and image from the vertex shader. The probe selects the explicit shader with
`d3d11 --explicit-so` and the implicit path with `d3d11 --indirect`.
[Test cases, shader and component hashes](DXVK_STREAM_OUTPUT_20261118.json)

PAL4 also produced a game-local exception on a boot that remained responsive.
In two such runs the observer read the game's initialization log while the
game was opening it. The revised observer opens the file after process exit.
On the matched 998.4 MHz native NVK run, the observer made zero accesses to
that log while PAL4 was running. The game finished effect initialization and
started its player manager before the machine rebooted. Its exception file
contained only its startup heading. All eleven recorded game queue operations
completed, with zero recorded game Presents.
The power driver had already entered fixed recovery mode and reported a DFLL
error before one of the earlier launches. Explicitly selecting fixed mode cleared the latch, then
selecting Handheld restored CPU and GPU policy and EMC transitions on that boot.
The healthy fixed-mode case on the same signed KMD ended in a system restart.
[PAL4 and power recovery measurements](PAL4_LOAD_CONTROLS_20261118.json)

The application-scoped loader settings list the exact game executable and its
matching signed ICD manifest. The bundled x86 loader includes the application
driver when the corresponding registry view has no system ICD. Both MinGW
vulkaninfo builds initialize their Win32 window functions before querying WSI.
The patches target Khronos Vulkan-Loader and Vulkan-Tools v1.4.354.
[Component hashes and upstream revisions](dxvk-games-20261118/components.json)

## Pinned DXVK and license boundary

| Field | Value |
|---|---|
| Upstream repository | `https://github.com/doitsujin/dxvk.git` |
| Tag | `v3.1.1` |
| Commit | `b1a1c99ab52b687cf950d62c88bc2fa316b41663` |
| Submodules | `dxbc-spirv` `bf14419e`, `libdisplay-info` `275e6459`, Vulkan headers `8864cdc8`, SPIR-V headers `04f10f65`, DirectX headers `9df86f23`, `spirv_headers` `c8ad050f` |
| Upstream license | zlib/libpng (retained unmodified; DXVK source is not vendored into this repository) |

`tegra-x1-wddm/tools/dxvk/build_dxvk.py` builds only a clean checkout of that
exact commit and submodule state, refusing any modified tracked files or
mismatched SHA. All adaptation lives in NVK, Windows WSI, KMT, and the GM20B
KMD inside this repository.

## Vulkan requirements read from DXVK `v3.1.1`

Requirements audited directly from `src/dxvk/dxvk_instance.h` and
`src/dxvk/dxvk_device_info.cpp` at `b1a1c99ab52b687cf950d62c88bc2fa316b41663`:

- **Vulkan 1.3.** `DxvkVulkanApiVersion = VK_API_VERSION_1_3`; devices
  reporting a lower `apiVersion` are rejected.
- **Required extensions** (`getFeatureList()` entries with `require = true`):
  `VK_EXT_depth_clip_enable`, `VK_EXT_robustness2` (`robustBufferAccess2`,
  `nullDescriptor`), `VK_KHR_load_store_op_none`, `VK_KHR_maintenance5`,
  `VK_KHR_maintenance6`, and `VK_KHR_swapchain`.
- **Required core features:**
  - **1.0:** `depthBiasClamp`, `depthClamp`, `dualSrcBlend`,
    `fillModeNonSolid`, `fragmentStoresAndAtomics`, `fullDrawIndexUint32`,
    `geometryShader`, `imageCubeArray`, `independentBlend`,
    `multiDrawIndirect`, `multiViewport`, `occlusionQueryPrecise`,
    `robustBufferAccess`, `sampleRateShading`, `samplerAnisotropy`,
    `shaderClipDistance`, `shaderCullDistance`, `shaderImageGatherExtended`,
    `shaderInt16`, `shaderInt64`, `shaderSampledImageArrayDynamicIndexing`,
    `textureCompressionBC`.
  - **1.1:** `shaderDrawParameters`, `storageBuffer16BitAccess`.
  - **1.2:** `bufferDeviceAddress`, `descriptorIndexing`
    (`descriptorBindingSampledImageUpdateAfterBind`,
    `descriptorBindingUpdateUnusedWhilePending`,
    `descriptorBindingPartiallyBound`), `hostQueryReset`,
    `runtimeDescriptorArray`, `samplerMirrorClampToEdge`, `scalarBlockLayout`,
    `shaderInt8`, `storageBuffer8BitAccess`, `timelineSemaphore`,
    `uniformBufferStandardLayout`, `vulkanMemoryModel`.
  - **1.3:** `inlineUniformBlock`, `computeFullSubgroups`, `dynamicRendering`,
    `maintenance4`, `shaderDemoteToHelperInvocation`,
    `shaderZeroInitializeWorkgroupMemory`, `subgroupSizeControl`,
    `synchronization2`.
- **Queue and limits:** a graphics queue and `maxPushConstantsSize >= 256`
  (NVK exposes `256` bytes).
- **Optional extensions/features used when advertised on Windows GM20B:**
  `VK_EXT_transform_feedback`, `VK_EXT_memory_budget`, `VK_EXT_multi_draw`,
  `VK_EXT_descriptor_buffer`, `VK_EXT_graphics_pipeline_library` /
  `VK_KHR_pipeline_library`, `VK_EXT_shader_module_identifier`,
  `VK_EXT_swapchain_maintenance1` / `VK_KHR_swapchain_maintenance1`,
  `VK_KHR_swapchain_mutable_format`, `VK_KHR_incremental_present`,
  `VK_EXT_attachment_feedback_loop_layout`, `VK_EXT_custom_border_color`,
  `VK_EXT_border_color_swizzle`, `VK_EXT_depth_bias_control`,
  `VK_EXT_extended_dynamic_state3`, `VK_EXT_fragment_shader_interlock`,
  `VK_EXT_line_rasterization`, `VK_EXT_non_seamless_cube_map`,
  `VK_EXT_vertex_attribute_divisor`, `VK_KHR_dynamic_rendering_local_read`,
  `VK_KHR_maintenance7`, `VK_KHR_unified_image_layouts`,
  `VK_KHR_external_memory_win32`, `VK_KHR_external_semaphore_win32`,
  `tessellationShader`, `logicOp`, `drawIndirectFirstInstance`,
  `pipelineStatisticsQuery` (with combined 3D + direct/indirect compute
  `CSInvocations` tracking in `nvk_query_pool.c`).
- **Intentionally unadvertised on Windows GM20B:**
  - `VK_EXT_conservative_rasterization` (GM20B `MAXWELL_B` supports conservative
    rasterization in hardware, but pre-Volta NVK implements it via privileged
    register writes in `NVK_MME_SET_CONSERVATIVE_RASTER_STATE`, which the
    Windows GM20B KMD command-stream validator rejects; gated by
    `!DETECT_OS_WINDOWS` in `nvk_physical_device.c`).
  - `VK_KHR_present_id{,2}`, `VK_KHR_present_wait{,2}`, and
    `VK_EXT_present_timing` (disabled on `VK_USE_PLATFORM_WIN32_KHR` because
    GDI presentation has no kernel vblank completion timestamp/ID).
  - `VK_EXT_memory_priority` and `VK_EXT_pageable_device_local_memory`
    (allocations are made resident on creation without a priority/evict path).
  - `VK_KHR_copy_memory_indirect` and `VK_EXT_device_generated_commands`
    (gated off on `_WIN32` because their Turing+/arbitrary indirect command
    token streams are outside the validated GM20B indirect ABI).

## Binding model and constant-buffer invariant

- DXVK `v3.1.1` selects its `VK_EXT_descriptor_buffer` binding model for
  `VK_DRIVER_ID_MESA_NVK`.
- Direct draws use `vkCmdDrawMultiEXT` / `vkCmdDrawMultiIndexedEXT`, which NVK
  records as inline `NVK_MME_DRAW` / `NVK_MME_DRAW_INDEXED` calls accepted by
  the KMD validator.
- Before Turing (`cls_eng3d < TURING_A`), `nvk_nir_lower_descriptors.c`
  promotes UBO descriptors to hardware constant-buffer bindings
  (`NVK_CBUF_TYPE_UBO_DESC`) **only** for host-pushed descriptor sets
  (`VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT` *without*
  `VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT`), whose addresses
  are resolved on the CPU during recording. Descriptor-buffer sets—including
  descriptor-buffer push sets bound via `vkCmdSetDescriptorBufferOffsetsEXT`—lower
  UBO reads to global memory loads and never emit `NVK_MME_BIND_CBUF_DESC`
  indirect GPFIFO segments. Unpushed/null push sets unbind the constant-buffer
  slot cleanly.

## Indirect command and transform-feedback submission (`TG_GPU_SUBMIT_ABI_INDIRECT = 2`)

The pre-Turing GM20B submission path encodes indirect draws, `DrawAuto`,
transform-feedback counter loads and indirect compute dispatches by opening an MME macro call in
an inline push slice, fetching the indirect argument words from application GPU
memory via a middle GPFIFO entry, and finishing in a validated inline suffix
slice:

| Vulkan command | Direct3D 10/11 origin | GPFIFO topology | Indirect payload |
|---|---|---|---|
| `vkCmdDrawIndirect` / `vkCmdDrawIndirectCount` (count=1) | D3D11 `DrawInstancedIndirect` | 3 entries (`prefix -> indirect -> suffix`) | `16 * drawCount` bytes (`VkDrawIndirectCommand`) |
| `vkCmdDrawIndexedIndirect` | D3D11 `DrawIndexedInstancedIndirect` | 3 entries (`prefix -> indirect -> suffix`) | `20 * drawCount` bytes (`VkDrawIndexedIndirectCommand`) |
| `vkCmdDrawIndirectByteCountEXT` | D3D10/11 `DrawAuto` | 3 entries (`prefix -> indirect -> suffix`) | `4` bytes (XFB byte counter) |
| `vkCmdBeginTransformFeedbackEXT` (counter buffers) | D3D10/11 `SOSetTargets` append/resume | 3 entries (`prefix -> indirect -> suffix`) | `4` bytes per counter |
| `vkCmdDispatchIndirect` | D3D11 `DispatchIndirect` | 4 entries (`prefix -> indirect[NO_PREFETCH] -> suffix_mme[PROCEED] -> suffix_pcas`) | `12` bytes (`VkDispatchIndirectCommand`) |

### End-to-end submission and validator contract

1. **No CPU readback, no per-draw queue idle, no busy polling.** NVK splits the
   command stream around each `nvk_cmd_buffer_push_indirect()` segment, keeping
   the indirect argument fetch on the GPU channel. `nvk_CmdDispatchIndirect2KHR`
   flushes its inline QMD + `SEND_PCAS_A` continuation immediately after the
   indirect argument slice so every indirect dispatch forms a self-contained
   `(prefix, indirect, suffix)` tuple in `exec_transport.h`.
2. **Transport ABI (`struct tg_gpu_submit_indirect`).** Carries
   `version = TG_GPU_SUBMIT_ABI_INDIRECT` (`2`), `indirect_va`,
   `indirect_bytes`, and `suffix_offset` splitting the private inline buffer
   into `[0, suffix_offset)` (prefix) and `[suffix_offset, inline_bytes)`
   (suffix). Submissions without indirect arguments continue using ABI v1
   (`TG_GPU_SUBMIT_ABI = 1`) unchanged.
3. **KMD command-stream validation (`tegra-x1-wddm/core/command_stream.c`):**
   - Prefix must open *exactly one* allow-listed indirect MME macro call
     (`NVK_MME_DRAW_INDIRECT`, `NVK_MME_DRAW_INDEXED_INDIRECT`,
     `NVK_MME_XFB_DRAW_INDIRECT`, `NVK_MME_XFB_COUNTER_LOAD`, or
     `NVK_MME_DISPATCH_INDIRECT`) as its final method header, preceded by a
     host `NVB06F_WFI(SCOPE_ALL)` (and preceded in suffix for compute before
     `SEND_PCAS_A`) so prior GPU writes to the indirect buffer or QMD complete
     before host/PBDMA fetch.
   - `indirect_bytes` must equal the exact expected macro argument byte count
     (4-byte aligned, non-zero, within `TG_GPU_MAX_INDIRECT_BYTES = 256 KiB`),
     ensuring GPU-fetched words are consumed purely as MME macro data and can
     never be interpreted as GPU method headers.
   - For `NVK_MME_DISPATCH_INDIRECT`, the validator verifies the 64-word inline
     QMD record, relocates it to a 256-byte-aligned KMD QMD slot, patches both
     `SEND_PCAS_A` *and* the MME suffix's `qmd_addr` parameter so MME's
     `STORE_RAM` writes ` gridDimX/Y/Z ` into the exact live QMD launched by
     `SEND_PCAS_A`, and splits the suffix at the `WFI` boundary with `PROCEED`
     so `SEND_PCAS_A` is not prefetched before MME updates the QMD.
   - Validates transform-feedback target buffer ranges (`SetStreamOutBuffer*`)
     and conditional-rendering semaphore ranges (`SetRenderEnable*`, 32-byte
     pair) against mapped context allocations. Note: upstream DXVK `v3.1.1`
     stubs `ID3D11DeviceContext::SetPredication` (`src/d3d11/d3d11_context.cpp:2680`)
     and does not enable `VK_EXT_conditional_rendering`; NVK and the GM20B KMD
     validate `SetRenderEnable*` for direct Vulkan and Zink callers.
4. **Residency, lifetime, and synchronization (`context.c` / `core.c`):**
   - KMD resolves `indirect_va` + `indirect_bytes` inside the submitting
     context's locked VA table before queuing, rejecting unmapped, non-GPU-visible,
     read-only-violating, or cross-boundary ranges.
   - Submission completion is tracked on the channel's hardware syncpt /
     monitored fence (`tg_kmt_signal_gpu` / `tg_kmt_wait_gpu`), keeping both
     KMD inline/QMD slots and application indirect buffers ordered without
     CPU stalls.

## Windows application architectures (`ARM64`, `ARM64EC`, `x64`, `x86`)

The user-mode architecture follows the application process and the Windows ABI
it supports. The tested mixed x64/ARM64EC process used an x64 Vulkan loader
and an x64 NVK ICD over the native ARM64 kernel driver.

| Application | DXVK | NVK ICD build | Hardware evidence |
|---|---|---|---|
| Native ARM64 | ARM64 cross-build | ARM64 | Native desktop and NVDEC use the registered ARM64 stack. |
| Emulated x64 | Desktop x64 DLLs | x64, 64-bit KMT | D3D9 and D3D10 pixel controls, D3D11 teapot, D3D9 LibreQuake |
| Emulated x86 | Desktop x86 DLLs | i686, 32-bit pointers and 64-bit GPU VAs | 32-bit Vulkan capabilities, D3D9 LibreQuake map |
| x64 application with ARM64EC DXVK | Desktop ARM64EC DLLs | x64 | D3D9 and D3D10 pixel controls, D3D11 teapot, D3D9 LibreQuake |


- `kmt_abi.h` and `test-kmt-sdk.c` verify `D3DKMT_*` structures against
  Windows SDK `10.0.26100.0` (`shared/d3dkmthk.h`) on `arm64`, `arm64ec`, `x64`,
  and 32-bit `x86` (`WOW64`, where pointer members such as `pPrivateDriverData`,
  `pData`, `CpuVirtualAddress`, and `FenceValueCPUVirtualAddress` are 32-bit
  with 8-byte `UINT64` alignment on GPU virtual addresses and fence values).
- All private KMD payloads (`tg_gpu_adapter_info`, `tg_gpu_alloc_priv`,
  `tg_gpu_submit`, `tg_gpu_submit_indirect`) use fixed-width 32/64-bit integers
  with identical sizes and field offsets across 32-bit `x86` and 64-bit builds
  so WOW64 user processes talk directly to the 64-bit ARM64 GM20B KMD.
- `kmt_native.c` rejects allocation sizes exceeding 32-bit `SIZE_MAX` before
  calling `CreateFileMappingW` / `VirtualQuery` in 32-bit processes.
- With `rust-std` for `x86_64-pc-windows-gnullvm` and `i686-pc-windows-gnullvm`
  in the build sysroot, `build_icd.py --arch x64` and
  `build_icd.py --arch x86` can link `libvulkan_nouveau.dll` directly
  (`nvk_windows_icd.json` for 64-bit `VulkanDriverName` and
  `nvk_windows_icd.x86.json` for 32-bit `VulkanDriverNameWow`).

## Win32 WSI presentation and synchronization

- **60 Hz blocking frame pacing:** `nvk_wsi.c` sets `cpu_present_fps = 60`.
  `wsi_win32_pacing.h` uses a per-swapchain high-resolution waitable timer
  (`CreateWaitableTimerExW` with `CREATE_WAITABLE_TIMER_HIGH_RESOLUTION` and
  `WaitForSingleObject(..., INFINITE)`), avoiding CPU spin-waiting, global
  system timer resolution changes, and catch-up frame bursts after a slow
  frame.
- **Bounded swapchain images (`2..4`):** For hardware devices on the Win32
  GDI/CPU presentation path (`!wsi_device->sw && !get_d3d12_command_queue`),
  `wsi_common_win32.cpp` reports `minImageCount = 2` and `maxImageCount = 4`
  with `VK_PRESENT_MODE_FIFO_KHR`. Each swapchain slot allocates a device-local
  image, a linear host-visible blit buffer, and a 32-bpp DIB section; capping
  at 4 images prevents exhausting the 512 MiB GM20B ceiling while guaranteeing
  at least double buffering (DXVK selects 3 images by default).
- **Resource & timeline synchronization:** `wsi_common.c` waits on the WSI blit
  fence before copying into the DIB, flushes GDI (`GdiFlush()`) and DWM
  (`DwmFlush()`), and supports `VK_KHR_swapchain_maintenance1` present fences
  and image release. Cross-API Win32 timeline NT fences (`VK_KHR_external_semaphore_win32`)
  use KMT shared monitored fences (`tg_kmt_wait_gpu` / `tg_kmt_signal_gpu`).
  Direct Flip / MPO scanout bypass remains deferred.

## Memory budget (`LOCAL` vs `NON_LOCAL` and 512 MiB ceiling)

- `tg_kmt_query_video_memory()` queries `D3DKMTQueryVideoMemoryInfo` separately
  for `D3DKMT_MEMORY_SEGMENT_GROUP_LOCAL` (`0`) and
  `D3DKMT_MEMORY_SEGMENT_GROUP_NON_LOCAL` (`1`), keeping `Budget`,
  `CurrentUsage`, `CurrentReservation`, and `AvailableForReservation`
  **unmerged**.
- Because the GM20B KMD's paging-aperture segment does not yet have a
  hardware-measured budget-group assignment, the two groups are **never**
  summed blindly. By default, NVK reports its conservative internal heap
  estimate capped at `512 MiB` (`TG_GPU_MEMORY_BUDGET_BYTES`).
- `nvk-windows-memory-check.exe <LUID> [MiB]` measures live `LOCAL` and
  `NON_LOCAL` deltas around one resident allocation on hardware. Setting
  `NVK_WINDOWS_BUDGET_GROUP=local` or `non-local` opts into reporting that
  single measured group (`heapBudget = min(512 MiB, Budget)`,
  `heapUsage = CurrentUsage`, never counting `CurrentReservation` as usage, and
  falling back to the internal estimate on query failure or zero `Budget`).

## Build recipes and offscreen smoke test

### 1. Build DXVK `v3.1.1` and `dxvk-offscreen.exe`

```sh
git clone --recurse-submodules --branch v3.1.1 https://github.com/doitsujin/dxvk.git
python3 tegra-x1-wddm/tools/dxvk/build_dxvk.py \
  --dxvk dxvk --arch arm64 --out build/dxvk-arm64
```

Pass `--arch x64` or `--arch x86` (and optional `--mingw-bin`) to build the
64-bit x64 or 32-bit x86 packages. [Build recipe](../tools/dxvk/build_dxvk.py)

### 2. Build NVK Windows ICD and KMT test suite

```sh
python3 src/nouveau/vulkan/nvkmd/windows/build_icd.py \
  --arch arm64 --out build/icd-arm64 \
  --cc "$ARM64_MINGW_CC" --clang-cl "$CLANG_CL" \
  --sdk-include "$SDK_INCLUDE" --wdk-include "$WDK_INCLUDE" \
  --package-cache-dir "$MESA_PACKAGE_CACHE"
```

Add `--prepare-only` with `--arch {arm64,arm64ec,x64,x86}` to validate the SDK
KMT layout and build the per-architecture KMT object and standalone PE tests.

### 3. Offscreen smoke test (`dxvk-offscreen.exe`)

`dxvk-offscreen.exe <d3d9|d3d10|d3d11>` loads `d3d9.dll`, `d3d10core.dll`,
`d3d11.dll`, and `dxgi.dll` by absolute path from its own directory, renders
into a `64x64` offscreen render target, and verifies CPU readback without
presenting:

- `d3d9`: render-target clear, fixed-function `XYZRHW | DIFFUSE` triangle draw,
  and programmable SM 2.0 (`vs_2_0` / `ps_2_0`) `DrawIndexedPrimitive` with
  `SetPixelShaderConstantF`, a 2x2 `D3DFMT_A8R8G8B8` quadrant texture, and
  `D3DFMT_D24S8` depth-pass / depth-reject occlusion.
- `d3d10`: render-target clear, VS/PS constant-buffer triangle draw, and
  `DrawIndexed` with a 2x2 `R8G8B8A8_UNORM` SRV texture, dynamic constant-buffer
  scaling, and `D24_UNORM_S8_UINT` depth-pass / depth-reject occlusion.
- `d3d11`: all `d3d10` stages plus `D3D11_QUERY_OCCLUSION` (visible vs
  depth-rejected sample counts) and combined 3D + direct compute
  `D3D11_QUERY_PIPELINE_STATISTICS` (`IAVertices`, `IAPrimitives`,
  `VSInvocations`, `PSInvocations`, and `CSInvocations`).
- `dxvk-offscreen.exe selftest` verifies pixel tolerance, triangle coverage,
  textured-quadrant/depth coverage, full-target coverage, and BGRA/RGBA swizzle
  rules without loading Direct3D.
- `dxvk-offscreen.exe d3d11 --indirect` additionally exercises all pre-Turing
  indirect GPU submission stages at feature level `11_0`:
  1. `DrawInstancedIndirect` with non-zero `AlignedByteOffsetForArgs` and
     `StartVertexLocation` (exit `12` on failure)
  2. `DrawIndexedInstancedIndirect` with non-zero `AlignedByteOffsetForArgs`,
     `StartIndexLocation`, and `BaseVertexLocation` (exit `13` on failure)
  3. Two-pass Stream-Output append (`offset = 0` followed by `offset = ~0u`) +
     `D3D11_QUERY_SO_STATISTICS` + `DrawAuto`, exercising both
     `STREAMING_BYTE_COUNT` writeback, `NVK_MME_XFB_COUNTER_LOAD`, and
     `NVK_MME_XFB_DRAW_INDIRECT` (exit `14` on failure)
  4. `DispatchIndirect` with non-zero `AlignedByteOffsetForArgs` + structured
     UAV write and staging readback (exit `15` on failure)
- `--system` runs the identical sequence against System32 Direct3D DLLs as a
  test control and refuses to run if DXVK DLLs sit beside the executable.

Exit codes: `0` pass, `1` usage, `2` module load/isolation, `3` device
creation, `4` `d3dcompiler_47.dll` unavailable, `5` resource creation,
`10` clear mismatch, `11` direct draw mismatch, `12` indirect draw failure,
`13` indirect indexed draw failure, `14` `DrawAuto` failure, `15` indirect
compute dispatch failure. [Smoke test source](../tools/dxvk/dxvk_offscreen.cpp)

## Native GM20B checks required on hardware

1. Deploy the matched KMD (`TegraGpuCore.lib` with `TG_GPU_SUBMIT_ABI_INDIRECT = 2`)
   and ARM64 `libvulkan_nouveau.dll`.
2. Run `test-kmt-memory.exe`, `test-exec-transport.exe`, `test-win32-pacing.exe`,
   and `nvk-windows-memory-check.exe <LUID>`, recording which VidMm segment
   group reflects resident allocations before setting `NVK_WINDOWS_BUDGET_GROUP`.
3. From the ARM64 DXVK `package/` directory,
   run `dxvk-offscreen.exe selftest`, `d3d9`, `d3d10`, `d3d11`, and
   `d3d11 --indirect`, retaining stdout, process exit codes, and DXVK logs.
4. Run `--system` controls from an isolated directory without DXVK DLLs.
5. Separately validate windowed Win32 swapchain presentation (60 Hz pacing,
   memory usage within 512 MiB, and visible client output).
