# tgd3d ARM64 构建交付（2026-11-12，洛杉矶时间）

本页保留原 `claude-vulkan` 的初始构建清单；该分支现已改名为默认分支 `tgd3d-vulkan`。
后续原生试验及失败边界见 [2026-11-13 实机记录](TGD3D_NATIVE_20261113.md)。
代码提交 `41c8c67e` 修复启动、同步、资源和查询契约；`7c677212` 单独将本实验分支的
Windows 默认构建选项设为开启。最终 UMD/ICD 均来自干净提交
`7c677212e5843725f3dbb12ee0b9dad6b1339a05`，Mesa 版本 26.2.4。

| 组件 | 身份 | SHA256 |
| --- | --- | --- |
| UMD | TegraGpuD3DVk.dll / 7c677212 | `769d86b685912bd6ec24ffd8287b2155b7d5d8df0ef3875d8ecca4752966d40c` |
| NVK ICD | libvulkan_nouveau.dll / 7c677212 | `17dc3110393365eef279a90104850d6f517c74659441916266afeae0ae2c8f73` |
| KMD（仅引用主聊天现有候选，未打入本包） | 0.1.0.161 / 7a1f7176 | `d3290a107f672f07ee1c15f1f51cd6cae495cf55eed698d03a1ca0ecae5f4b6d` |

本轮未重建或安装 KMD，也未构建 Zink。KMD 引用来自主聊天已有组件记录，不是新的
配套实机通过声明；正在实机使用的版本由主聊天确认。预算、睡眠和 GPU PMIC 桥未改。

## 结果

- UMD、ICD、ARM64 控制程序、Windows runtime probe 和 ETW exporter 构建通过。
  PE machine 均检查为 ARM64；UMD 导出 `OpenAdapter10` / `OpenAdapter10_2`。
- x64/Wine 11.21 的 11 项 CPU 控制输出通过：双向 Fence、失败不伪造完成、
  DONOTWAIT、view 退休、42 个常量缓冲槽、129 段查询、SM4 转译、共享 owner/布局、
  Present token/status。Vulkan 与 runtime dispatch 是替身；Wine 初始化的宿主 GPU
  枚举不算 tgd3d GPU 执行。
- passthrough VS/PS、纹理采样加 constant-buffer 13 的 PS 共三个模块通过
  `spirv-val --target-env vulkan1.3`；工具版本 SPIRV-Tools v2026.3。
- 不传 tgd3d 开关的 Meson 配置：Windows 生成 tgd3d 目标，Darwin 不生成。
  Zink UMD 与 ICD 专用入口明确禁用 tgd3d，避免重复构建。
- **未执行 ARM64 控制程序、原生 Windows runtime、DWM、GM20B 像素、旋转、
  Win32 vkcube 或实际 60 Hz 验收。** 不将编译、Code 0、CPU 控制当作实屏通过。

## 可复现入口与文件

从仓库执行 `tegra-x1-wddm/tools/build_tgd3d.py`，传入独立 `--out`、LLVM-MinGW
`--cross-file` 和 SDK/WDK `--sdk-include` / `--wdk-include`，版本为 10.0.26100.0。
配套 ICD 用 `src/nouveau/vulkan/nvkmd/windows/build_icd.py`，同样使用独立输出与该
源码提交，额外传 ARM64 `--cc`、`--clang-cl` 和 `--mesa`。本轮没有使用其他聊天的 build 目录。

本地交付目录 `tgd3d-7c677212-arm64` 包含两份 DLL、ICD JSON、三个 ARM64 检查工具、
README、许可证、`delivery.json`、CPU 日志、SPIR-V 和各组件构建清单。
`build/umd-manifest.json` 保留完整 Meson 配置参数；`build/icd-command.json`
保留完整 ICD 构建命令和实际工具链环境。所有文件 SHA256 在 `delivery.json`。
包中无 KMD、无安装脚本，也未修改任何驱动注册。

## 使用边界和后续

详见 [tgd3d README](../../src/nouveau/tgd3d/README.md)：仍有 MSAA、stream output、
predication 等明确不支持项，不能宣称完整 D3D10 一致性。
主聊天按原生 DWM/LUID 与物理画面 → 五分钟桌面 → 已知像素 → 硬件横屏 →
持续 Win32 vkcube 的顺序验收，并分开保存物理反馈和截图/ETL。

本轮没有继续合并 main。此前误合入 main 的代码已由 `c31c10a1` 撤销，历史保留。
将来获准整分支合并时先正确恢复这次撤销所移除的主线变更，并单独处理默认开启提交，
不能让历史中的撤销覆盖主线 Zink 修正。也可只集成 tgd3d 的代码补丁。
此前一次只读 tree 预演针对主线快照 `b4aed46e` 确认 Zink、原 D3D UMD、KMD、
NIL/NAK 源码树保持不变，仅 README 有文字冲突；它没有更新任何分支或工作区，
也不是运行时回归通过。按用户要求不再继续合并预演。
