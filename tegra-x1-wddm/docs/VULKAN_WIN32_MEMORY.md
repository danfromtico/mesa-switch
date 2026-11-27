# Win32 Vulkan external memory

NVK implements `VK_KHR_external_memory_win32` for
`VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT` on the Tegra KMT backend.
This is the Vulkan storage-sharing layer needed before the D3D UMD's
runtime-owned resource and image-layout bridge. It does not establish DWM
composition, D3D texture interoperability or Win32 swapchain presentation.

## Contract

Exportable memory uses the existing single-allocation NT-security KMT
resource. Imports validate its raw allocation ABI, size and CPU visibility,
charge the importing device, wait for residency and create that device's
GPU VA mapping. Both allocated and imported memory enter NVK's memory list.
Imports bypass zero/trash initialization and keep compression disabled.

The Vulkan allocation owns an internal, non-inheritable export handle.
`vkGetMemoryWin32HandleKHR` duplicates it with the requested inheritance
setting and access rights. Allocation-time security attributes and names
are consumed synchronously, so no caller-owned pointers are retained.
Named handles use the session's object namespace, including `Local\` and
`Global\` prefixes. `D3DKMTOpenNtHandleFromName` opens a temporary reference
for named imports; the subsequent resource open retains the payload.
[KMT sharing contract](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmthk/nf-d3dkmthk-d3dkmtshareobjects)
[Vulkan export attributes](https://docs.vulkan.org/refpages/latest/refpages/source/VkExportMemoryWin32HandleInfoKHR.html)

Import does not consume an application's NT handle. Closing that handle
or destroying the exporting resource/device must leave the imported memory
valid. Re-export from imported OPAQUE_WIN32 memory is supported. Zink now
routes Win32 handles to this type and no longer duplicates an import handle
that Vulkan does not consume. Shared buffer creation also declares its
external handle types.
[Vulkan import ownership](https://docs.vulkan.org/refpages/latest/refpages/source/VkImportMemoryWin32HandleInfoKHR.html)

External buffer/image queries report only OPAQUE_WIN32. Legacy KMT and D3D
texture/resource handle types remain unsupported: they need their own
metadata and lifetime contracts. The properties entry point returns
`VK_ERROR_INVALID_EXTERNAL_HANDLE` for unsupported types; opaque handles
are excluded from that query by the Vulkan valid-usage rules.
[Handle properties query](https://docs.vulkan.org/refpages/latest/refpages/source/vkGetMemoryWin32HandlePropertiesKHR.html)

## Validation

The Windows ARM64 ICD, UMD and native probe compile. On the .59 baseline,
the SDK-controlled KMT fixture passes on 2026-11-10 at 21:43:07 UTC, exit 0.
It checks named-object translation, access/security/inheritance forwarding,
repeated imports, budget accounting and failure cleanup. These are controlled
KMT substitutes, not a native GPU or cross-process test.

The .144 physical trial passes on Switch Lite, Windows 11 ARM64
23H2 (22631.2861). The adapter is Code 0 with matching KMT/active VidPN
LUID `98914` and the unchanged 536,870,912-byte ceiling. The probe runs
through the Khronos loader from 21:49:22 to 21:49:59 UTC, exits 0 and does
not time out. Four cases cover unnamed/named 16 KiB buffers and optimal
64x64 RGBA/BGRA images. Each case transfers ownership
through `VK_QUEUE_FAMILY_EXTERNAL` and waits for the producing GPU fence
before the next submission. The importer reads the first pattern and writes
a second one, re-exports it, and a fresh device reads it after the prior
device and application handle have been destroyed. Every read checks the
payload and 64 guard words. Eight readbacks match all 32,768 payload words
and 512 guards. Both default non-inheritable and explicitly inheritable NT
handles pass. The native test covers `Local\` named imports; `Global\`
translation and custom security/access forwarding have controlled coverage.

The subsequent native D3D11 runtime regression at feature level 10_0 runs
21:50:41–21:50:51 UTC. RGBA/BGRA clear and SM4 triangle readback match all
16,384 pixels; implicit release completes and the process exits 0 without
timeout. The KMD core is unchanged from .143. Both the ICD and UMD contain
the Mesa barrier lifetime fix, and the UMD also contains the Zink changes.

These checks use multiple devices in one process and host fence waits
between submissions. Cross-process operation, external semaphore/fence
sharing, overlapping execution, D3D runtime sharing and Vulkan CTS coverage
remain pending. The D3D regression does not exercise Zink's Win32 import
branch or establish DWM composition.

## Evidence

The downloaded 32 records total 15,317,375 bytes; every length and SHA-256
matches the device-side manifest. The exported ETL contains 137,531 records
in 10,773,156 bytes; every record boundary and the header count match.
The retained native completion record has success status, fence `37`,
Reference `20` (hex) and completion/CPU fence readback/retirement proof 7.

The 22:00:07 UTC recovery check confirms .59 Code 0, the original driver
and ACPI hashes, no start permit/test tasks, and removal of temporary
ICD/UMD registration. All five recovery records also match device hashes.
The before/after power reports are byte-identical;
AC/DC display and sleep remain Never. Firmware and Hekate were not changed.

| Artifact | SHA-256 |
| --- | --- |
| Signed .144 SYS | `e9cc6c5e576afd6861047983567eb2b4628209a5f227cdadc3ef48e86d59144c` |
| NVK ICD | `18368be320de7ece00572ba49480a8331be2df681093e591299576408a217b04` |
| D3D UMD | `69709cd80d74861ebe880136dd95c9ec56f1fdf741f35a09047a94387d0f9f3b` |
| Vulkan sharing probe | `3b21588f3f10cc7a21a43328dc1b6a6ddf88efce339f0aed885511c7d377a260` |
| Vulkan result JSON | `595334eedfec0832028dbce34ad016788171e31477a9797a39f9b42e1b3a4dd9` |
| D3D runtime result JSON | `9c6ea30e35035f7350266815f951465e923eb9055302a3e888a0a8446d953462` |
| Exported ETL | `3949048ceb05961e4f2c93f40cda123194cca799dc943e72bfb4dd1caf07f80c` |

Artifacts: `/private/tmp/t214-gpu-round/external-memory144.601od71q/`.
