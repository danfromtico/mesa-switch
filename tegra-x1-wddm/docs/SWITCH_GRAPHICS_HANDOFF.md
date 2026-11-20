# Switch Windows ARM64 图形稳定性交接

工作目录为 `/Users/rujian/Documents/Codex/mesa-switch-woa-tgd3d-vulkan`，分支为 `tgd3d-vulkan`。原会话 `01a1ec52-4325-7bf2-b65d-3c934aadfad2` 保留。新会话可以自行创建进行中的长期目标，专门接手下述第四步。前三步从新会话的待办中移除，已有能力查询、游戏和 Pacecar 验收留档。[四组游戏及 ARM64EC 记录](DXVK_ZINK_GAME_MATRIX_20261118.json)

## 第四步 · 查清旧式窗口首次 Present 的重启

**从原始最小场景开始。** Switch Lite GM20B 使用外接电源，原生 tgd3d 协商旧式 DDI `0x000b000f`，普通窗口、两个 BGRA UNORM 缓冲区。应用没有写入图像时，第一次 `Present(1, 0)` 即出现意外重启。原有三次独立开机的证据仍在。先读对应记录和该版本源码，找出从 Windows 提交到内核后最后完成的操作及首个失败点。已有持久记录到达用户态发出 `D3DKMT_PRESENT` 的边界，现有内核跟踪在此之前结束；公开 API 返回、触发故障的 miniport 调用和根因都还没有证据。另有一条 WDDM 2.0 六帧场景在第三次呈现时重启，分别登记。[原始实机记录](LOAD_STABILITY_FOLLOWUP_20261116.json) · [故障登记及原场景销案条件](open-faults.json)

同一个 JSON 里，先读 `native_sequential_windows_kmt_dispatch_20261120` 的 `native`、`versioned_callback` 和 `scope`，再对照 `native_first_present_miniport_receipts_20261120` 的正常窗口记录。两者分别给出故障前最后一个可证实的提交边界和能追到面板的成功路径。[原始记录及字段](LOAD_STABILITY_FOLLOWUP_20261116.json)

**按时间和所有者查首次呈现。** 将同一窗口、绘图线程、适配器 LUID、设备和上下文与 DWM 对齐，依次核对重定向表面分配、分页和驻留、同步、首次 KMD 调度、完成以及面板锁存。每次改变一个变量，分别记录最后进入和最后返回的阶段；只有 DWM 成功翻转时，记录其所属进程。用同机 Switchroot Noble、微软 WSL2 的 `drivers/hv/dxgkrnl` 和匹配版本的 WDDM、DXGI 契约复核判断。日常使用已验证的旧式 `FLIP_SEQUENTIAL` 路径。[Windows 首次提交与对照](LOAD_STABILITY_FOLLOWUP_20261116.json)

**修好后用原场景连续测三次。** 每次都核对实机加载的组件全哈希、外接供电、Code 0、真实输出、窗口正常退出、返回桌面和开机记录连续。记录受控重启、自动重启、人工强制重启、网络失联或断电；事件 41、6008 和零 BugcheckCode 只记录关机结果。专项通过且相同条件下没有新增或加重故障的真实修复可直接晋升。既有故障继续登记并附日常绕行。触发方式、阶段或严重程度变化时单列证据；候选验收中的未解释重启阻止相应晋升。

**以仓库和实机为准。** 替换 KMD、tgd3d 或 NVK 前撤销永久许可，候选只给一次性许可；在外接电源、Code 0 及对应场景验收通过后核对实际加载哈希，运行仓库晋升检查再给予永久许可。恢复使用上次已验收整套七项哈希。实机同一时间由一个会话操作，受控重启先在会话说明。Switch 的 SSH 失联后每 3 分钟重试一次。从首次失败起连续 30 分钟仍连不上，就同时暂停进行中的长期目标和当前任务，停止重试。期间重新连通后，下一次失联重新计时，并将断网与设备重启分别登记。修复与原场景复测完成后，把源码、组件全哈希和验收记录推送至对应仓库。[机械晋升检查](../tools/Select-GraphicsBaseline.ps1) · [开放故障](open-faults.json)

**收尾前再查一遍尚未接入的修复。** 对照当前实机七项哈希、基线源码、候选清单、开放故障和已经回退的交付，把每项对应的提交或组件、已有验收及仍缺的证据逐项核实。专项确认生效、对照基线没有新增或加重问题的，当场并入并记录永久许可；其余写清阻碍、日常绕行及下一项验证。将这次复查结果和剩余项目写入仓库，作为第四步的结项记录。

## 已选基线

完整 `e6852733` 对应 ARM64 tgd3d `cd352fa8`、KMD `fc6e8d08`、原生 NVK `51ff97f6`。开机记录 `84195`，Code 0，DWM 加载匹配的四项用户态组件。三次原始颜色用例通过，严格 FL11_1 硬件绘图 589 个像素全部匹配，独立 flip 窗口 5 个桌面采样匹配。首次颜色显示工具未取得匹配采样；同一开机、同一驱动下改用处理窗口重绘的采样工具，两项各 5 个颜色均匹配。原套 `57dc16b4` 已核对七项哈希并保留恢复记录。[当前图形基线](graphics-baseline.json) · [永久许可读回](dxvk-games-20261118/dxgi-copy-cd352fa8/permanent-boot.json) · [实机验收](dxvk-games-20261118/dxgi-copy-cd352fa8/qualification.json)

原 WDDM 2.0 实验 UMD `9fb6cef7` 的输入修复已合源码 `abd9f213`，EMC 投票修复 `454ea62d` 亦已合源码；两者单独的源码验证和实际运行组件分别登记。旧 KMD `102218b0` 的无 DMA、空上下文翻转，与 `a82f2510` 的旋转主表面显示模式尺寸，由原会话分别验收。旧镜像曾在方向切换中意外重启。合入基线须有对应 Windows 调用与完成、90° 实际呈现和回桌面的证据。[合并明细](dxvk-games-20261118/dxgi-copy-cd352fa8/merged-source-checks.json) · [两个历史 KMD 的原始记录](TGD3D_COLD_BOOT_HANDOFF_20261113.md)

## 原会话保留的后续工作

负载稳定后再继续 DX1–11 兼容性和逐模块性能。覆盖系统 DirectDraw、早期 D3D 至 7、D3D8、9、10、10.1、11，优先 32 位游戏的系统图形接口，逐项查实际渲染设备、画面、输入、退出和软件回退。x86 私有驱动绘图已通过，系统 D3D9 HAL 单独追踪。库存 Edge、Chrome 的 H.264 验收须取得同一播放中的浏览器解码器身份和 NVDEC 提交；定制 Chromium 和 wiliwili 各自保留独立记录。[系统 x86 D3D9](dxvk-games-20261118/x86-system-d3d9-windowed-retest.json) · [浏览器 H.264](dxvk-games-20261118/browser-h264-public-fl11-1.json)

第五步继续审 KMD、NVK/NAK、tgd3d/WSI、DXVK/Zink、NVDEC、电源调度、Pacecar、UEFI、SD、Wi-Fi 和蓝牙，Mesa 限于项目实际使用部分且最后审。以画面、帧时间、吞吐、功耗和回归实测对照，保留真实游戏中 Pacecar 多表面归属问题。第六步逐项接入候选和验收真实 SC7 睡眠与唤醒。收尾保留 Tegra DC 列扫描、开机标志、登录与桌面横屏，区分实际画面、寄存器和 API 状态。固定 90° 逐行与 Handheld 电源是可恢复路径。[平台审核](SWITCH_PLATFORM_REVIEW_20261118.json) · [逐行实测](ROW_SCAN_BASELINE_20261116.md)
