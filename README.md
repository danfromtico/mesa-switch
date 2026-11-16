# Mesa Switch for Windows ARM64

Mesa NVK and a Windows 11 WDDM driver for the Tegra X1 (GM20B), based on
the Mesa Switch port. The current hardware validation was performed on
Nintendo Switch Lite (HDH-001).

The long-term goal is a complete native Vulkan implementation, verified
with version-appropriate VK-GL-CTS coverage and physical GM20B tests.
Windows desktop GPU acceleration is the current development milestone.

**Current checkpoint (2026-11-16):** the WDDM2.0 UMD admission candidate failed
before any landscape switch. The device has been restored to the preceding
5b66bbec UMD with the retained KMD, ICD and platform settings; column scan is
not accepted. A new candidate fixes legal linear-buffer admission and resource
error contracts; modern admission is being revalidated before acceptance.
[Current handoff and evidence](tegra-x1-wddm/docs/TGD3D_COLD_BOOT_HANDOFF_20261113.md)
supersede the historical package milestones below.

Active development uses the default
[tgd3d-vulkan branch](https://github.com/ruje0504-2/mesa-switch-woa/tree/tgd3d-vulkan):
**DWM → tgd3d → Vulkan/NVK → KMD → GM20B**. The native D3D WDDM UMD
translates DDI calls directly to Vulkan. Development on `main`, including its
Gallium/Zink desktop route, is paused.
DXVK is the planned DX9/10/11 application compatibility path over the same
NVK/KMD foundation. OpenGL uses the separate Mesa/WGL → Zink → NVK path;
neither application path has completed hardware acceptance on this port.

| Area | Status |
| --- | --- |
| Native Vulkan | NVK rendered an offscreen triangle and read back the expected pixels and guards with driver **0.1.0.87**. A Win32 preview showed that readback. |
| Display adapter | **0.1.0.130** reached **Code 0**, completed GDI copies and displayed verified RGB bars on the internal panel through the GM20B VidPN and native scanout. |
| Shared storage | **0.1.0.144** passes Vulkan Win32 buffer/image sharing, GPU read/write after producer-device release, and re-export: 32,768 payload words plus 512 guards match. [Scope and evidence](tegra-x1-wddm/docs/VULKAN_WIN32_MEMORY.md). |
| Native Windows desktop (`tgd3d`) | **Static desktop confirmed; window acceptance pending.** The latest diagnostic trial retains DWM creation and 423 successful Present calls. Its test window stayed hidden, a tool defect subsequently reproduced and corrected on `.59`; the shared ETW session also lost an event. Earlier application/window-start restarts remain unresolved. Five-minute interaction and known-pixel acceptance remain pending. [Implementation and validation scope](src/nouveau/tgd3d/README.md). |
| Previous Windows desktop (Zink/NVK) | **Development paused on `main`.** The historical .158 combination reached a complete taskbar and native FL10_0 composition, but subsequent trials did not establish sustained visual stability. [Exact components and evidence](tegra-x1-wddm/docs/NATIVE_DESKTOP_158.md). |
| DX9 / DX10 / DX11 games (`DXVK`) | **Planned; application compatibility unverified on GM20B Windows.** Games can use D3D → DXVK → Vulkan/NVK → KMD, independently of tgd3d's D3D translation. The selected DXVK version requires matching Vulkan features, Win32 presentation and a compatible application/loader/ICD architecture. [DXVK requirements](https://github.com/doitsujin/dxvk/wiki/Driver-support). |
| OpenGL (`Zink`) | **Separate compatibility path; hardware acceptance pending.** Mesa OpenGL/WGL → Zink → NVK shares the Vulkan driver with tgd3d and DXVK. Pausing the old Zink desktop frontend does not remove the OpenGL route. |
| Hardware landscape | **Not physically validated.** A correct 1280×720 hardware-rotated desktop must pass geometry, asymmetric-pattern and sustained-interaction checks after desktop stability and pixel validation. |
| Win32 Vulkan presentation | **`vkcube` acceptance pending.** Verify native NVK rendering, swapchain behavior and sustained physical presentation after hardware landscape passes. |

Package versions above identify KMD test packages, not a combined UMD/ICD release.
The latest retained tgd3d trial used signed **.158** KMD (SHA256 prefix
`46d1757674ff`), UMD source **d6470feb** (`e25c01c90cb6`) and NVK ICD source
**7c677212** (`17dc31103933`). At the last verified recovery, the device was
back on **.59 / Code 0**. Each component has its own build manifest and full
source/artifact identity; the older Zink .158 combination is a separate trial.
A later baseline freeze coincided with commit exhaustion from this chat's
PowerShell SD inventory process. The process was identified from its recorded
command, cleared by the owner's restart, and replaced by bounded native-tool
collection. This does not resolve the earlier native-window restart.
[Incident evidence](tegra-x1-wddm/docs/TGD3D_NATIVE_20261113.md).

Continue in order: fix the current restart → 30-second static desktop →
60-second window interaction → five-minute desktop interaction → known-pixel
and shared-resource checks → hardware 1280×720 landscape → Win32 `vkcube`.
Advance only after the preceding stage passes. Build results, Code 0 and
successful Present calls do not replace physical-screen acceptance.
Direct Flip remains deferred. This is not a stable-driver release or complete
D3D/Vulkan conformance claim.

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
