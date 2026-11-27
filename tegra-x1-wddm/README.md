# Tegra X1 WDDM and NVK for Windows ARM64

An experimental Windows 11 GPU driver for the Tegra X1 (GM20B).
The kernel driver and board integrations share this tree with the Mesa NVK
Windows transport. Hardware verification started with Nintendo Switch Lite. [NVK Windows backend](../src/nouveau/vulkan/nvkmd/windows/)

Switch OLED (HEG-001) port and compatibility findings by [Tsunami714!](https://github.com/Tsunami714a). [WDDM query review](docs/OLED_WDDM_QUERY_REVIEW_20261127.md)

The long-term goal is a complete native Vulkan implementation, verified
with version-appropriate VK-GL-CTS coverage and physical GM20B tests.
The current milestone is stable native DWM composition through
**DWM → tgd3d → Vulkan/NVK → KMD → GM20B**. Active development is on the
default `tgd3d-vulkan` branch; `main` and its Gallium/Zink desktop frontend
are paused. DXVK is the planned DX9/10/11 application path, while
Mesa OpenGL/WGL → Zink → NVK remains a separate compatibility path.
Both share NVK/KMD with tgd3d and require their own hardware validation.

| Area | Status |
| --- | --- |
| Native Vulkan | NVK rendered an offscreen triangle on the GM20B and read back the expected pixels and guards with driver **0.1.0.87**. A Win32 preview displayed that readback. |
| Display adapter | **0.1.0.130** reached **Code 0**, completed GDI copies and displayed verified RGB bars through the GM20B VidPN and native scanout. |
| Shared storage | **0.1.0.144** passes Vulkan Win32 buffer/image sharing, GPU read/write after producer-device release, and re-export: 32,768 payload words plus 512 guards match. [Scope and evidence](docs/VULKAN_WIN32_MEMORY.md). |
| Native Windows desktop (`tgd3d`) | **Short native desktop confirmed; stability not passed.** The .158 KMD plus tgd3d UMD `3c5d327d` and ICD `7c677212` records DWM creation and 251 successful Present calls. The user reports an operable desktop followed by an unexpected restart after opening the performance utility. Five-minute interaction and known-pixel acceptance remain pending. [Implementation and limits](../src/nouveau/tgd3d/README.md). |
| Previous Windows desktop (Zink/NVK) | **Development paused on `main`.** Historical .158 desktop observations do not establish sustained stability for the new tgd3d combination. [Historical evidence](docs/NATIVE_DESKTOP_158.md). |
| DX9 / DX10 / DX11 games (`DXVK`) | **Planned; not hardware-validated on this port.** D3D → DXVK → NVK/KMD is independent of tgd3d's D3D translation. Vulkan features, Win32 presentation and application/ICD architecture must be validated for the selected DXVK version. |
| OpenGL (`Zink`) | **Separate compatibility path; hardware acceptance pending.** Mesa OpenGL/WGL → Zink → NVK remains available for future compatibility work. |
| Hardware landscape / Win32 `vkcube` | **Acceptance pending.** First pass desktop stability and known-pixel/shared-resource tests, then validate actual 1280×720 hardware rotation, then sustained native Win32 `vkcube` presentation. |

At the last verified recovery from the tgd3d trial, the device was restored
to **.59 / Code 0** with its CPU, fan and power settings unchanged.
The next trial proceeds through 30-second static, 60-second window and
five-minute desktop stages before pixels, hardware rotation and `vkcube`.
Code 0, successful Present calls and screenshots alone are not physical-screen
acceptance. Direct Flip remains deferred.

The driver uses dynamically allocated shared memory with a **512 MiB**
adapter ceiling. Windows manages residency and budgets through VidMm.

- [Build and porting guide](docs/PORTING.md) for WDK, NVK, and Switch Lite integration.
- [Hardware status](docs/STATUS.md) for the current baseline and next verification.
- [Native scanout evidence](docs/NATIVE_STARTUP_RESULT.md) and [Direct3D/DWM work](docs/PORTING.md#direct3d-umd-and-dwm).
- [Driver contracts and bring-up history](docs/DRIVER.md).
- [Upstream, downstream, and provenance](UPSTREAM.md).
- [Licenses](LICENSE.md) and [copyright notices](NOTICE).
