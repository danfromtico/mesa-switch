# Mesa Switch for Windows ARM64

This repository contains the GM20B WDDM kernel driver, the `tgd3d` Direct3D user-mode driver and the Mesa NVK Vulkan driver for Windows 11 ARM64 on Nintendo Switch. On the tested Switch Lite configuration, DWM uses `tgd3d` from the `tgd3d-vulkan` branch to render through NVK on the Tegra X1 GPU. This configuration uses WinX firmware and ACPI tables. The companion Windows driver project supplies its board drivers and NVDEC clocks. [WinX firmware](https://github.com/ruje0504-2/winx) · [Windows board drivers](https://github.com/ruje0504-2/Windows-for-Switch-Drivers)

## Game progress

**PAL4 (Chinese Paladin 4 / 仙剑奇侠传四)** now starts and reaches a game scene
on the tested Switch Lite. **Warcraft III** reaches a map through Mesa NVK on
GM20B. **Frame rates still need optimization**: CPU/D3D translation overhead
and NVK/KMD driver performance remain to be distinguished. PAL4 game audio
also remains unresolved. [Game results and remaining work](tegra-x1-wddm/docs/game-runs-20261127/README.md).

## Hardware status

### Switch OLED (HEG-001 / Aula)

Tsunami714! reached an interactive Windows desktop on HEG-001. DXGI enumerated `Tegra X1 (GM20B)`, and the tester recorded `TegraGpuD3DVk.dll` loading and entering `open-native`. The paging-only kernel driver reported flags `0x1`. D3D11 device creation on Tegra returned `DXGI_ERROR_UNSUPPORTED` because tgd3d also requires the native submission bit (`0x2`). The tester staged a native `.201` candidate on the SD card.

On a follow-up OLED run, tgd3d loaded the NVK ICD, created a Vulkan instance and selected the Tegra physical device by LUID. `VulkanAdapter::open` returned `S_OK`. The logs recorded entry to D3D device creation and to the KMD allocation, paging and submission callbacks. Device Manager Code 0, successful D3D device creation and hardware-accelerated DWM still await confirmation. [OLED port and testing by Tsunami714!](https://github.com/Tsunami714a) · [Desktop and UMD test results](tegra-x1-wddm/docs/OLED_WDDM_QUERY_REVIEW_20261127.md#桌面与-umd-实测)

The tester reported `VIDEO_MEMORY_MANAGEMENT_INTERNAL` (`0x10E`, parameter 1 `0x8`) in a separate run. Microsoft defines `0x8` as an invalid reference in a split DMA buffer. The root cause remains open. [Microsoft bugcheck reference](https://learn.microsoft.com/en-us/windows-hardware/drivers/debugger/bug-check-0x10e---video-memory-management-internal)

An earlier HEG-001 setup used the SwBatt power-management bridge and switched to the GPU after logon. It reached GPU Code 0 and displayed a portrait Windows desktop through tgd3d and NVK. Some runs later hung under desktop load. Independent OLED tests and SwBatt integration documented by [DanilKhmyrov](https://github.com/DanilKhmyrov). [Desktop bring-up](https://github.com/DanilKhmyrov/windows-switch/blob/ebff45d3038d1e321f3dc83dc7eea2dbe6cf79c1/README.md#L525-L540) · [Hardware test notes](https://github.com/DanilKhmyrov/windows-switch/blob/ebff45d3038d1e321f3dc83dc7eea2dbe6cf79c1/docs/notes-for-mesa-switch-woa.md#L32-L59)

### Switch Lite (HDH-001)

The selected PAL4 page-directory repair passed a one-use qualification on boot
`106486` and a subsequent **permanent boot at Code 0 on `106579`**. DWM loaded
the exact approved tgd3d and NVK components. The daily KMD retains bounded
paging diagnostics (`PagingDiagnosticsVersion=1`) with the active stop probe
**off** (`PagingStopProbeArmed=0`). Source: `d1c07b6a8c4c`.
[Scoped qualification and GPU readback](tegra-x1-wddm/docs/pal4-pde-kind-daily-20261127/README.md) ·
[Permanent boot and full hashes](tegra-x1-wddm/docs/pal4-pde-kind-daily-20261127/permanent-boot.json)

| Component | Binary | SHA-256 prefix |
| --- | --- | --- |
| WDDM kernel driver 0.1.0.158 | `tegragpurender.sys` | `1d88065b2e4a` |
| tgd3d entry / native ARM64 implementation | `TegraGpuD3DVk.dll` / `tg_d3d_arm64.dll` | `9ff198aeabf1` / `0790651c6981` |
| NVK entry / native ARM64 implementation | `libvulkan_nouveau.dll` / `tg_nvk_arm64.dll` | `3ca2d9eb0fdf` / `13ae4c9c5c34` |

The packaging manifest and the device's permanent permission now select the same `1d88065b2e4a` KMD. The user-mode binaries are unchanged. This is a scoped paging repair; game performance, PAL4 audio and the other open driver issues remain under investigation. [Package baseline manifest](tegra-x1-wddm/docs/graphics-baseline.json) · [Open driver issues](tegra-x1-wddm/docs/open-faults.json)

### Switch Lite feature tests

| Area | Result on the build linked for each test |
| --- | --- |
| Desktop and internal panel | The fixed 90° desktop uses GPU prerotation and row scan, with a 1280×720 desktop on the native 720×1280 panel. The mode persisted across restart. An ordinary DWM window passed move, resize, minimize, restore and close checks. DC column scan and reliable switching between all four orientations remain open. [Display baseline](tegra-x1-wddm/docs/ROW_SCAN_BASELINE_20261116.md) · [Window checks](tegra-x1-wddm/docs/VKCUBE_ACCEPTANCE_20261116.md) |
| Native Win32 Vulkan | On the earlier fixed-landscape build, ARM64 `vkcube` rendered through NVK on GM20B in a GDI/DWM window. The timed run lasted 300.289 seconds and exited normally. Resize and recovery after occlusion were observed on the panel. [Components, timing and observations](tegra-x1-wddm/docs/VKCUBE_ACCEPTANCE_20261116.md) |
| H.264 hardware decoding | D3D11VA drove NVDEC in separate 720p and 1080p runs. Each decoded six frames that matched an independent NV12 reference. The 1080p test presented all six in a 640×360 window, showed the color pattern on the panel and closed to the desktop. Sustained playback and load-related restarts remain under investigation. 2K and 4K device tests are deferred. [720p run](tegra-x1-wddm/docs/NVDEC_720_COMPOSITOR_OPEN_20261117.json) · [1080p run](tegra-x1-wddm/docs/NVDEC_1080_D3D11VA_20261117.json) · [Restart observations](tegra-x1-wddm/docs/LOAD_STABILITY_FOLLOWUP_20261116.json) |
| Direct3D 9/10/11 games | FTEQW loaded a LibreQuake map through x86, x64 and ARM64EC DXVK on the tested Lite. Each run handled input and returned to the desktop with exit code zero. [Game measurements](tegra-x1-wddm/docs/DXVK_ZINK_GAME_MATRIX_20261118.json) PAL4 now reaches a scene after the paging fix; Warcraft III reaches a map through NVK at roughly 12 FPS in the owner's photo. Both need performance investigation; PAL4 audio remains unresolved. [November 27 game runs and photo](tegra-x1-wddm/docs/game-runs-20261127/README.md) |
| Earlier driver milestones | The `.87` KMD passed native Vulkan triangle readback. The `.130` display driver showed verified RGB bars. The `.144` driver passed Win32 Vulkan sharing, GPU access after producer release and re-export, with 32,768 payload words and 512 guards checked. [Display evidence](tegra-x1-wddm/docs/NATIVE_STARTUP_RESULT.md) · [Sharing evidence](tegra-x1-wddm/docs/VULKAN_WIN32_MEMORY.md) |
| OpenGL and the earlier desktop frontend | FTEQW also ran the LibreQuake map through x86 and x64 Zink. Development of the earlier Gallium/Zink desktop path on `main` is paused. [Application tests](tegra-x1-wddm/docs/DXVK_GAMES.md) [Earlier desktop results](tegra-x1-wddm/docs/NATIVE_DESKTOP_158.md) |

The 1080p run retained the Handheld profile, CPU DFLL, fan mode 1 and the 85% charge target. Direct Flip is deferred. Full Vulkan conformance remains a long-term goal, with version-appropriate VK-GL-CTS coverage still pending. [Power and display readback](tegra-x1-wddm/docs/NVDEC_1080_D3D11VA_20261117.json) · [Development handoff](tegra-x1-wddm/docs/TGD3D_COLD_BOOT_HANDOFF_20261113.md)

The Windows NVK backend uses KMT for allocations, GPU virtual addresses,
submissions and monitored fences. The KMD uses dynamically allocated system
memory under VidMm with a **512 MiB** adapter ceiling.

- [Windows NVK backend](src/nouveau/vulkan/nvkmd/windows/) and its ARM64 build.
- [Tegra X1 WDDM source and board integration](tegra-x1-wddm/).
- [Build and porting guide](tegra-x1-wddm/docs/PORTING.md) and [hardware status](tegra-x1-wddm/docs/STATUS.md).
- [Native scanout evidence](tegra-x1-wddm/docs/NATIVE_STARTUP_RESULT.md) and [Direct3D/DWM work](tegra-x1-wddm/docs/PORTING.md#direct3d-umd-and-dwm).
- [RADV `wddm2` reference and compatibility notes](tegra-x1-wddm/docs/PORTING.md#radv-windows-reference) for WDDM, Win32 presentation, Direct3D and Zink.
- [Upstream and downstream](tegra-x1-wddm/UPSTREAM.md) and [license map](tegra-x1-wddm/LICENSE.md).
- [Horizon OS documentation from mesa-switch](README.rst).

The Windows NVK code retains Mesa's MIT license. Each source file retains
its existing license and notices. [Mesa license policy](https://docs.mesa3d.org/license.html)
