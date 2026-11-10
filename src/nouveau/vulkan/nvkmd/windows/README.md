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

The currently validated workload is the offscreen GM20B triangle on a
Switch Lite. Command validation, Win32 presentation and desktop
integration are in development. [Matching KMD and hardware status](https://github.com/ruje0504-2/mesa-switch-woa/tree/main/tegra-x1-wddm)
