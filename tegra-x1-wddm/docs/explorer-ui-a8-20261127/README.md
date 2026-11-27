# Explorer WinUI A8_UNORM & FL11_1 TIR Gate Qualification (2026-11-27)

Scoped native ARM64 UMD (`tg_d3d_arm64.dll`) repair qualified on one-time AC trial boot **106676** for `SWITCH-EXPLORER-WINUI-HEADER-BLANK`.

- Source commit: `76f6bcd1a07eb0ef15207a04997fb9bea46ee7bf`
- Candidate directory: `C:\ProgramData\SwitchWOA\GpuUpdate\explorer-ui-a8-1f81681c0862`
- `tg_d3d_arm64.dll`: `1f81681c0862619f591f596c537d8fc37ffc8555fa39c03c964a368c1c7e38c1` (signed `Switch Lite GPU Test Code`, `Valid`)
- Boot `106676` (`Code 0`, AC `Online`): `maintenance5=true`, `formatA8=true`, `levels=7`, `RESOURCE_FAIL=0`, `DEVICEREMOVED=0`, `DWM TIR drops=0`, 610 Explorer draws, `tgd3d-ui-fix-contracts.exe` `ALL_UI_FIX_CONTRACTS_PASS`, `vk-triangle.exe` control `1024` pixels (`red=300, blue=724, guard=128`, exit `0`).
- All KMD, NVK, ARM64X forwarder, x64 UMD and ACR binaries are retained byte-identical from the accepted PAL4 daily baseline.
