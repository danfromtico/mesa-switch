# Windows ARM64 NVK backend

This experimental NVK backend opens the Windows display kernel adapter
identified by the OS LUID. It uses KMT for VidMm allocations, GPU virtual
addresses, command submission and monitored fences. The matching GM20B
miniport must advertise the native-submit adapter ABI. The adapter,
allocation and submit headers in this directory are shared with the KMD.

Build from the Mesa checkout with LLVM-MinGW ARM64, Rust's
`aarch64-pc-windows-gnullvm` target, Meson, Ninja and bindgen. Build
`mesa_clc` and `vtn_bindgen2` for the build host from this Mesa revision
and put them on PATH. Configure bindgen for the target Clang resource
directory and MinGW sysroot. Pass the versioned Windows SDK and WDK
Include directories for the KMT bridge.

```sh
python3 src/nouveau/vulkan/nvkmd/windows/build_icd.py \
  --out build/windows-nvk --cross-file "$WINDOWS_CROSS_FILE" \
  --cc "$ARM64_MINGW_CC" --clang-cl "$CLANG_CL" \
  --sdk-include "$SDK_INCLUDE" --wdk-include "$WDK_INCLUDE"
```

The SDK layout fixture runs at compile time. The build emits the ICD DLL,
a local ICD manifest, a controlled `test-kmt.exe` and a live adapter probe
`nvk-windows-check.exe`. Run the latter with the absolute ICD DLL path.
Use `--loader <absolute-vulkan-1.dll>` to exercise the public loader instead
of the ICD entry point. A registered manifest is required for elevated
processes, which ignore `VK_DRIVER_FILES`. On the .59 diagnostic baseline,
instance creation can succeed while loader device enumeration returns
`VK_ERROR_INITIALIZATION_FAILED` and the full identity check exits 10;
that is not a native-GPU pass.

The Windows manifest uses `.\\libvulkan_nouveau.dll` in JSON (one
backslash after JSON decoding). Keep it beside the ICD. A POSIX `./` path
is not resolved against the manifest by the deployed Windows loader and
can fail outside the ICD directory. For packaging checks, run the probe
from a separate executable directory and use System32 as its working
directory; placing the probe beside the ICD can hide the defect.
[Loader diagnosis and path control](../../../../../tegra-x1-wddm/docs/PORTING.md#windows-icd-manifest-path)

`test-kmt-shared.exe` checks shared storage, named objects and ownership with
controlled SDK KMT substitutes. `nvk-windows-shared-check.exe` tests live KMT
storage. `nvk-windows-vulkan-shared-check.exe <absolute-vulkan-1.dll>` tests
native Vulkan buffer/image sharing, GPU writes and readback after releasing
the producer, and re-export. The latter requires a registered ICD and an
active native-submit KMD. These live probes are not default build tests.
[Win32 memory contract and physical evidence](../../../../../tegra-x1-wddm/docs/VULKAN_WIN32_MEMORY.md)

`test-kmt-displayable.exe` checks the submitting-device reference and
`WrittenPrimaries` transport for runtime-owned backing. A source kernel
resource is exported and opened on the submitting device; foreign allocation
handles are never substituted. The helper does not allocate replacement pixel
storage, and cleanup does not debit the original owner's memory counter.
Failed cleanup retains its handles for retry. Paging removal is not completion.
These helpers are not yet connected to UMD write tracking or ordinary NVK queue
submission. Modern tgd3d admission remains withheld until that integration is
complete. [Contract audit and CPU controls](../../../../../tegra-x1-wddm/docs/DISPLAYABLE_RESOURCE_CONTRACT_20261116.md)

Shared CPU-visible allocations use page-rounded byte counts and
`PAGE_READWRITE | SEC_COMMIT | SEC_WRITECOMBINE`, without large-page flags.
Failed `CreateFileMappingW` calls retain their Win32 error in the SDK's
`NTSTATUS_FROM_WIN32` encoding. The Vulkan boundary maps only memory or
pool/commit/quota errors to host OOM; unexpected access/privilege/parameter errors
return `VK_ERROR_UNKNOWN` with the original code. The controlled fixture
injects thirteen section errors, verifies budget rollback, checks the exact
512 MiB boundary and tests 4 KiB/64 KiB/2 MiB GPU-VA alignment without
committing a 512 MiB Windows allocation. Successful small section requests
still use the real Windows API and verify write-combined mapping.
[Allocation and budget audit](../../../../../tegra-x1-wddm/docs/PORTING.md#host-allocation-and-budget-audit)

`test-kmt-memory.exe` checks the per-group `D3DKMTQueryVideoMemoryInfo`
bridge and the heap-budget rules with controlled substitutes.
`nvk-windows-memory-check.exe <LUID> [MiB]` is a live probe: it prints both
VidMm segment groups unmerged and reports which one is charged for a resident
allocation. `NVK_WINDOWS_BUDGET_GROUP=local|non-local` selects that measured
group for `VK_EXT_memory_budget`; unset, the heap keeps its estimates.
[Budget mapping](../../../../../tegra-x1-wddm/docs/PORTING.md#radv-windows-reference)

`test-exec-transport.exe` checks the inline (`TG_GPU_SUBMIT_ABI = 1`) and
indirect (`TG_GPU_SUBMIT_ABI_INDIRECT = 2`) submission transport model used
for pre-Turing `DrawIndirect`, `DrawIndexedIndirect`, transform-feedback
`DrawAuto` / counter loads, and `DispatchIndirect`. `build_icd.py --arch
{arm64,arm64ec,x64,x86}` validates the Windows SDK KMT ABI and undecorated ICD
v7 exports across native ARM64, ARM64EC, x64, and 32-bit WOW64 x86 targets
(full `libvulkan_nouveau.dll` linking requires the matching Rust `rust-std`
target for NAK and LLVM-MinGW sysroot).
[DXVK game path and indirect transport](../../../../../tegra-x1-wddm/docs/DXVK_GAMES.md)

Native offscreen Vulkan and D3D11 runtime rendering at feature level 10_0
have physical passes on Switch Lite. Win32 Vulkan presentation, D3D runtime
shared-resource integration and sustained DWM composition remain in
development. [Matching KMD and hardware status](https://github.com/ruje0504-2/mesa-switch-woa/tree/main/tegra-x1-wddm)
