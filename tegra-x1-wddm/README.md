# Tegra X1 WDDM and NVK for Windows ARM64

This tree contains the Tegra X1 (GM20B) kernel driver and Mesa NVK transport for Windows 11 ARM64 on Nintendo Switch. [NVK Windows backend](../src/nouveau/vulkan/nvkmd/windows/)

Switch Lite (HDH-001) has a verified permanent-boot baseline. [Hardware status and baseline hashes](../README.md#hardware-status)

On Switch OLED (HEG-001), Tsunami714! reported successful KMD startup, a call to the MPO3 present callback and an interactive desktop. The tgd3d UMD loaded. Its adapter check rejected the paging-only KMD because it lacked `NATIVE_SUBMIT`. The tester staged a native `.201` KMD on the SD card. In a follow-up OLED run, Tsunami714! reported that the tgd3d UMD opened Vulkan. The loaded builds, D3D11 device creation on Tegra and hardware-accelerated DWM still await confirmation. DanilKhmyrov previously demonstrated a GPU-rendered desktop on a separate HEG-001 setup using SwBatt. [OLED port by Tsunami714!](https://github.com/Tsunami714a) · [UMD results](docs/OLED_WDDM_QUERY_REVIEW_20261127.md#桌面与-umd-实测) · [One-time startup permit for the OLED port](docs/OLED_WDDM_QUERY_REVIEW_20261127.md#oled-移植版的一次性启动许可) · [Earlier desktop test by DanilKhmyrov](docs/OLED_WDDM_QUERY_REVIEW_20261127.md#独立的-oled-gpu-桌面实测)

The long-term goal is a complete native Vulkan implementation, verified
with version-appropriate VK-GL-CTS coverage and physical GM20B tests.
The current milestone is stable native DWM composition through
**DWM → tgd3d → Vulkan/NVK → KMD → GM20B**. Active development is on the
default `tgd3d-vulkan` branch. Development of the Gallium/Zink desktop frontend
on `main` is paused. DXVK supplies the DX9/10/11 application path, while
Mesa OpenGL/WGL → Zink → NVK remains a separate compatibility path.
Both share NVK/KMD with tgd3d and require their own hardware validation.

| Area | Switch Lite hardware results |
| --- | --- |
| Native Vulkan | NVK rendered an offscreen triangle on the GM20B and read back the expected pixels and guards with driver **0.1.0.87**. A Win32 preview displayed that readback. |
| Display adapter | **0.1.0.130** reached **Code 0**, completed GDI copies and displayed verified RGB bars through the GM20B VidPN and native scanout. |
| Shared storage | **0.1.0.144** passes Vulkan Win32 buffer/image sharing, GPU read/write after producer-device release, and re-export: 32,768 payload words plus 512 guards match. [Scope and evidence](docs/VULKAN_WIN32_MEMORY.md). |
| Native Windows desktop (`tgd3d`) | At boot `103136`, the GPU reported Code 0 and DWM loaded its native ARM64 tgd3d and NVK modules with the signed .158 KMD (`1d7fcf5d75ed`). [Permanent-boot record](docs/tdr-fault-isr-1d7fcf5d/permanent-boot-103136.json) · [Open issues](docs/open-faults.json). |
| Previous Windows desktop (Zink/NVK) | **Development paused on `main`.** Historical .158 desktop observations do not establish sustained stability for the new tgd3d combination. [Historical evidence](docs/NATIVE_DESKTOP_158.md). |
| DX9 / DX10 / DX11 games (`DXVK`) | FTEQW loaded a LibreQuake map through x86, x64 and ARM64EC DXVK. These runs handled input and exited normally. [Game measurements](docs/DXVK_ZINK_GAME_MATRIX_20261118.json). |
| OpenGL (`Zink`) | FTEQW ran the same map through x86 and x64 Zink. [Application tests](docs/DXVK_GAMES.md). |
| Hardware landscape / Win32 `vkcube` | The fixed 90° row-scan build displayed a Win32 `vkcube` window for five minutes and recovered from occlusion. [Display and window tests](docs/VKCUBE_ACCEPTANCE_20261116.md). |

The driver uses dynamically allocated shared memory with a **512 MiB**
adapter ceiling. Windows manages residency and budgets through VidMm.

- [Build and porting guide](docs/PORTING.md) for WDK, NVK, and Switch Lite integration.
- [Hardware test history](docs/STATUS.md) for earlier builds and their verification records.
- [Native scanout evidence](docs/NATIVE_STARTUP_RESULT.md) and [Direct3D/DWM work](docs/PORTING.md#direct3d-umd-and-dwm).
- [Driver contracts and bring-up history](docs/DRIVER.md).
- [Upstream, downstream, and provenance](UPSTREAM.md).
- [Licenses](LICENSE.md) and [copyright notices](NOTICE).
