# Mesa Switch for Windows ARM64

This repository contains the GM20B WDDM kernel driver, the `tgd3d` Direct3D user-mode driver and the Mesa NVK Vulkan driver for Windows 11 ARM64 on Nintendo Switch Lite (HDH-001). On the active `tgd3d-vulkan` branch, DWM uses `tgd3d` to render through NVK on the Tegra X1 GPU. WinX supplies the firmware and ACPI tables. The companion Windows driver project supplies the board drivers and the clocks used by NVDEC. [WinX firmware](https://github.com/ruje0504-2/winx) · [Windows board drivers](https://github.com/ruje0504-2/Windows-for-Switch-Drivers)

## Hardware status

The development baseline selected on November 17, 2026 uses KMD `9ef86d7e`, UMD `c8a8eb9a` and ICD `e563a81d` (SHA-256 prefixes). The 720p combination is retained for recovery. [Full component hashes and recovery package](tegra-x1-wddm/docs/NVDEC_1080_D3D11VA_20261117.json)

| Area | Result on the tested Switch Lite |
| --- | --- |
| Desktop and internal panel | The fixed 90° desktop uses GPU prerotation and row scan, with a 1280×720 desktop on the native 720×1280 panel. The mode persisted across restart. An ordinary DWM window passed move, resize, minimize, restore and close checks. DC column scan and reliable switching between all four orientations remain open. [Display baseline](tegra-x1-wddm/docs/ROW_SCAN_BASELINE_20261116.md) · [Window checks](tegra-x1-wddm/docs/VKCUBE_ACCEPTANCE_20261116.md) |
| Native Win32 Vulkan | On the earlier fixed-landscape build, ARM64 `vkcube` rendered through NVK on GM20B in a GDI/DWM window. The timed run lasted 300.289 seconds and exited normally. Resize and recovery after occlusion were observed on the panel. [Components, timing and observations](tegra-x1-wddm/docs/VKCUBE_ACCEPTANCE_20261116.md) |
| H.264 hardware decoding | D3D11VA drove NVDEC in separate 720p and 1080p runs. Each decoded six frames that matched an independent NV12 reference. The 1080p test presented all six in a 640×360 window, showed the color pattern on the panel and closed to the desktop. Sustained playback and load-related restarts remain under investigation. 2K and 4K device tests are deferred. [720p run](tegra-x1-wddm/docs/NVDEC_720_COMPOSITOR_OPEN_20261117.json) · [1080p run](tegra-x1-wddm/docs/NVDEC_1080_D3D11VA_20261117.json) · [Restart observations](tegra-x1-wddm/docs/LOAD_STABILITY_FOLLOWUP_20261116.json) |
| Direct3D 9/10/11 games | Integration of DXVK v3.1.1 for ARM64 is in progress. The published baseline has no representative game result covering native NVK rendering, input and normal exit. |
| Earlier driver milestones | The `.87` KMD passed native Vulkan triangle readback. The `.130` display driver showed verified RGB bars. The `.144` driver passed Win32 Vulkan sharing, GPU access after producer release and re-export, with 32,768 payload words and 512 guards checked. [Display evidence](tegra-x1-wddm/docs/NATIVE_STARTUP_RESULT.md) · [Sharing evidence](tegra-x1-wddm/docs/VULKAN_WIN32_MEMORY.md) |
| OpenGL and the earlier desktop frontend | Windows OpenGL via WGL → Zink → NVK awaits hardware validation. Development of the earlier Gallium/Zink desktop path on `main` is paused. [Earlier desktop results](tegra-x1-wddm/docs/NATIVE_DESKTOP_158.md) |

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
