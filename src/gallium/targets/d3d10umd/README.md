# D3D10umd Readme

## Tegra NVK backend

`-Dgallium-d3d10umd=true -Dgallium-d3d10-backend=nvk -Dgallium-drivers=zink`
builds an experimental hardware target. It queries the runtime's adapter
callback for the Tegra ABI and opens Zink on that exact Vulkan device LUID.
The KMD must enable native submissions. This target exports the D3D adapter
entry points without the software target's KMT emulation.

The frontend currently negotiates D3D10 DDIs. D3D10.1/D3D11 DDIs, runtime
resource sharing and native presentation still need implementation. The
hardware target returns an unsupported result from presentation callbacks;
it does not use the GDI target's frontbuffer path. It is not a working DWM
driver yet. [Windows build and verification](../../../../tegra-x1-wddm/docs/PORTING.md#direct3d-umd-and-dwm)

`nvk_d3d_check.exe` explicitly exercises native DDI clear/copy/readback and
SM4 vertex/pixel shaders with ordinary and indexed/scissored triangle draws
on a selected KMT LUID. Every pixel is checked against the expected image.
Run it only with a native-submission KMD and registered NVK ICD; elevated
Windows processes ignore its `VK_DRIVER_FILES` override.
This probe does not use the Windows D3D runtime or presentation callbacks.

`nvk_d3d_runtime_check.exe --luid 16-hex-digits` instead calls the Windows
`D3D11CreateDevice` runtime at feature level 10_0 and checks RGBA/BGRA clear,
SM4 drawing and staging readback. It requires the registered native UMD,
checks the resulting adapter LUID and loaded `TegraGpuD3D.dll`, and does not
fall back to WARP. `--warp` and `--software path/to/gdi-reference.dll` are
explicit software controls. These rendering modes do not test resource
sharing or presentation; none runs as a default Meson test.
For the experimental NVK hardware path, register the ICD as above and make the
ARM64 Vulkan loader available.
Append `--implicit-release` to the hardware command to retain scope-exit
ComPtr destruction while investigating the exit-path fault. Exception
diagnostics print raw addresses and a bounded ARM64 frame-pointer chain;
module bases are recorded before drawing, avoiding loader queries during
failure reporting. This is not a full unwind or a teardown fix.

`--luid 16-hex-digits --shared-contract` checks runtime resource association,
opening and release for legacy and NT shared BGRA textures, including the
consumer's descriptor after producer release. `--warp --shared-contract`
provides a software control. This mode submits no drawing/copy commands and
does not validate shared image contents or synchronization. The production
UMD's runtime resource bridge is still incomplete.
[Runtime contract evidence](../../../../tegra-x1-wddm/docs/D3D_RUNTIME_SHARING.md)

Append `--feature-levels` to `--luid 16-hex-digits`, `--warp` or `--software
path/to/dll` to probe 11_1, 11_0, 10_1 and 10_0 separately, then negotiate
the full ordered list. It records HRESULT, selected level, adapter identity
and native UMD path. It does not render or claim DWM acceptance. The normal
rendering regression deliberately requests only 10_0, so that test alone
does not establish a feature-level ceiling. The present frontend's ceiling
also follows from `SUPPORT_D3D10_1=0`, `SUPPORT_D3D11=0` and its supported
device-interface table; advertising 11_0 requires implementing its DDIs.

The UMD has an opt-in ETW provider
`{78edcf0e-b51c-4b24-9b28-c86d48f521a9}` (level 4, keyword 1). It records
adapter/device negotiation, exact Zink LUID selection, shared-resource open
and DXGI presentation calls, including their actual HRESULTs. Enable it
through a trace controller; it does not depend on a DWM environment flag.
The provider unregisters before DLL unload. The versioned payload includes
the process image basename; ETW supplies PID, TID and timestamp.

```powershell
New-EtwTraceSession -Name TegraUmd -LocalFilePath C:\test\umd.etl -LogFileMode 1
Add-EtwTraceProvider -Guid '{78edcf0e-b51c-4b24-9b28-c86d48f521a9}' -SessionName TegraUmd -Level 4 -MatchAnyKeyword 1
# Run the bounded driver test, then stop the trace before exporting.
Stop-EtwTraceSession -Name TegraUmd
.\d3d10-trace-export.exe C:\test\umd.etl > umd.jsonl
```

`Trace.h` defines the version-1, 88-byte numeric payload. `ScreenQuery` logs adapter
flags/budget; `ZinkBegin` logs the LUID and a reserved zero field; `SupportedVersion` logs index/DDI version;
`GetCaps` logs type/size; `CreateDeviceBegin` logs interface/flags. A failed
`CreateDeviceResult` identifies interface rejection (arg1=0), context (1),
CSO state (2) or empty shaders (3). `Blt` logs flags/rotation; its current
success result still comes from an unimplemented stub, not a copy proof.
The decoder rejects incompatible payload versions and reports lost events.

`ZinkFailure` records the first returned screen-creation failure before
cleanup. The exporter adds `stage`, `line`, `detail` and signed `code`
fields. `line` refers to the matching `zink_screen.c` revision. Stages are
1 allocation, 2 loader, 3 loader entrypoints, 4 instance, 5 physical-device
enumeration, 6 LUID matching, 7 features, 8 logical device and 9 remaining
initialization. `code` is a Win32 error for stages 2/3, a `VkResult` for
Vulkan calls, or zero for boolean failures. At stage 3, `detail` bits 0/1
identify missing GetInstanceProcAddr/GetDeviceProcAddr; at stage 4 they
identify missing instance entrypoints/reuse of a cached instance failure.
Stages 5/6 report the enumerated device count. This preserves the original
API result when Zink reuses a failed creation attempt; it does not turn a
zero code into proof that the stage succeeded.

When available, `ZinkMessage` carries the first instance-creation
`VK_EXT_debug_utils` error, truncated to 1,023 UTF-8 bytes before conversion
to the separate 2,112-byte UTF-16 event payload. The decoder validates its
length and terminators and emits JSON `message` text. The callback returns
`VK_FALSE` and retains its data with the cached instance through destruction;
it does not point into a screen-creation caller's stack. Other Zink callers
that do not request creation-error output keep the existing instance chain.
Absence of a message does not establish where an API error originated.

`ZinkInstanceCall` snapshots the original result of version enumeration (0),
extension count/list (1/2), layer count/list (3/4), and instance creation (5).
`value` is the version/count for a non-error return and zero otherwise.
`ZinkInstanceInfo` carries the called-bit mask, enabled extension/layer
counts, and flags: debug-utils available (1), callback chained (2), cached
snapshot (4), extension allocation failure (8), layer allocation failure
(16). These diagnostics are returned on successful screen creation too.
An unset call bit is not a successful result; cached snapshots are not new
Vulkan calls. Failure of a zero-sized list's optional malloc is not reported
as an allocation failure.

Post-device diagnostics retain the original DDI behavior. `RuntimeError`
uses a separate 328-byte payload containing HRESULT, source basename,
function, line and process image. It is written before `pfnSetErrorCb`,
which may not return. `ResourceCreateBegin` packs format/dimension in arg0
and misc/bind flags in arg1; `ResourceCreateSize` packs width/height and
array/mip counts respectively (high/low 32-bit halves). `ResourceCreateEnd`
marks successful completion with format and subresource count. Failure
sites are reported by `RuntimeError`, not a fabricated successful end.
`FormatBegin`/`FormatEnd` bracket the format query and retain its capability
bits; `Multisample` records format and sample/quality counts.
`DestroyDeviceBegin`/`DestroyDeviceEnd` bracket teardown. No resource pointers
or contents are included, and missing endpoints alone do not establish a
crash location. Use the exporter built with these payload definitions.

## GDI software backend

When compiled with `gallium-driver=llvmpipe` or `gallium-driver=softpipe` the resulting libgallium_d3d10.dll implements D3D10's software rendering interface, like WARP.

It can be used directly from WLK 1.6 and WHCK 2.0 D3D10+ tests, via the -Src
and -SWDLL options. For example:

    wgf11blend.exe -Debug -DoNotCatchExceptions -DXGI:1.1 -FeatureLevel:10.0 -Src:SW -SWDLL:libgallium_d3d10.dll -LogClean -LogVerbose

However, as of WHCK version 2.1 this mechanism no longer works reliably.
Either you use WHCK 2.0 binaries, or you must use the alternative method
described below (of copying libgallium_d3d10.dll into the executable directory and rename
it such that it matches the D3D10 UMD of the test machine).

Examples can be easily modified to load it too:

    D3D10CreateDeviceAndSwapChain(NULL,
                                  D3D10_DRIVER_TYPE_SOFTWARE,
                                  LoadLibraryA("libgallium_d3d10"), /* Software */
                                  Flags,
                                  D3D10_SDK_VERSION,
                                  &SwapChainDesc,
                                  &g_pSwapChain,
                                  &g_pDevice);

    D3D11CreateDeviceAndSwapChain(NULL, /* pAdapter */
                                  D3D_DRIVER_TYPE_SOFTWARE,
                                  LoadLibraryA("libgallium_d3d10"), /* Software */
                                  Flags,
                                  FeatureLevels,
                                  sizeof FeatureLevels / sizeof FeatureLevels[0],
                                  D3D11_SDK_VERSION,
                                  &SwapChainDesc,
                                  &g_pSwapChain,
                                  &g_pDevice,
                                  NULL, /* pFeatureLevel */
                                  &g_pDeviceContext); /* ppImmediateContext */

Alternatively, it can be renamed into the system's D3D10 UMD driver (e.g.,
vm3dum_10.dll for VMware vGPU, nvwgf2um.dll for NVIDIA GPU), and placed into
the application directory, or system's directory, and used instead.

For the DLL to be picked from the application directory you'll need to do the
following once:

    reg add "HKLM\System\CurrentControlSet\Control\Session Manager" /v "SafeDllSearchMode" /t REG_DWORD /d 0 /f

See also <https://docs.microsoft.com/en-us/windows/win32/dlls/dynamic-link-library-search-order>

## Building with MSVC

Install Windows SDK 10.0.26100.0 and Windows DDK 10.0.26100.0 with MSVC and use it, other version of Windows SDK/DDK
may also work but not verified.

## Building with mingw

Install Windows SDK 10.0.26100.0 and Windows DDK 10.0.26100.0, other version of Windows SDK/DDK
may also work but not verified.

Copy the following list of files from Windows SDK/DDK into mingw's `include` folder or `include\winddk` folder in mesa source tree:
    ```txt
    d3d10umddi.h
    d3d11TokenizedProgramFormat.hpp
    d3dkmddi.h
    d3dkmdt.h
    d3dkmthk.h
    d3dukmdt.h
    d3dumddi.h
    dxgiddi.h
    dxmini.h
    wmidata.h
    ```
