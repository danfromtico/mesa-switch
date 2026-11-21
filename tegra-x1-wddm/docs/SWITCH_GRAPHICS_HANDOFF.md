# Switch Windows ARM64 图形稳定性交接

工作目录为 `/Users/rujian/Documents/Codex/mesa-switch-woa-tgd3d-vulkan`，分支为 `tgd3d-vulkan`。原会话 `01a1ec52-4325-7bf2-b65d-3c934aadfad2` 保留。新会话可以自行创建进行中的长期目标，专门接手下述第四步。前三步从新会话的待办中移除，已有能力查询、游戏和 Pacecar 验收留档。[四组游戏及 ARM64EC 记录](DXVK_ZINK_GAME_MATRIX_20261118.json)

## 第四步 · 查清旧式窗口首次 Present 的重启

普通窗口暂存复制修复已列为基线。源码为 `a7c422ad`，签名 KMD 为 `c09c3c86`，ARM64 tgd3d 为 `b4f6c7bf`。原程序在 GM20B 上连续三次显示、响应输入、正常退出并返回同一桌面。正常翻转窗口也通过对照。旧式窗口历史重启继续单独排查。WDDM 2.0 进程级 Present1 和标准 GDI allocation-only 导入各自需要对应的生产组件实机回执。[基线清单](graphics-baseline.json) · [三次实测与正常对照](legacy-sequential-step4/window-copy-c09c3c86/result.json) · [晋升回执](legacy-sequential-step4/window-copy-c09c3c86/promotion-qualification.json)

**从原始最小场景开始。** Switch Lite GM20B 使用外接电源，原生 tgd3d 协商旧式 DDI `0x000b000f`，普通窗口、两个 BGRA UNORM 缓冲区。应用没有写入图像时，第一次 `Present(1, 0)` 即出现意外重启。原空窗口程序 `fd64bd1b` 的三份历史记录均在首次调用时中断。带绘图的相关程序 `62100a30` 另有持久记录到达用户态发出 `D3DKMT_PRESENT` 的边界，其内核跟踪在此之前结束。历史复位前最后完成的 miniport 操作仍待查明。另有一条 WDDM 2.0 六帧场景在第三次呈现时重启，分别登记。[原始实机记录](LOAD_STABILITY_FOLLOWUP_20261116.json) · [故障登记及原场景销案条件](open-faults.json)

同一个 JSON 里，先读 `native_sequential_windows_kmt_dispatch_20261120` 的 `native`、`versioned_callback` 和 `scope`，再对照 `native_first_present_miniport_receipts_20261120` 的正常窗口记录。两者分别给出故障前最后一个可证实的提交边界和能追到面板的成功路径。[原始记录及字段](LOAD_STABILITY_FOLLOWUP_20261116.json)

查原始窗口提交记录时，按 `native_sequential_windows_kmt_dispatch_20261120` 核对客户端 SHA-256 `62100a301ae7c85a7d40d15d13b91837989b4b06ac935cd9182274e8a0ce6f6f` 和证据包 SHA-256 `4fd7dca31251886fa409627bc2253fff80eafa0d802acc27dae58993a5361a8d`。本轮已验收的全屏修复及恢复材料在本机 `/Users/rujian/Documents/Codex/switch-gm20b-fullscreen-work`。无线启动异常在记录 `85304` 出现，下一次启动 `85382` 有 Windows 正常重启请求，单列在已有无线故障项。[无线恢复记录](dxvk-games-20261118/gdi-fullscreen-gm20b/wifi-startup.json)

**按时间和所有者查首次呈现。** 将同一窗口、绘图线程、适配器 LUID、设备和上下文与 DWM 对齐，依次核对重定向表面分配、分页和驻留、同步、首次 KMD 调度、完成以及面板锁存。每次改变一个变量，分别记录最后进入和最后返回的阶段；只有 DWM 成功翻转时，记录其所属进程。用同机 Switchroot Noble、微软 WSL2 的 `drivers/hv/dxgkrnl` 和匹配版本的 WDDM、DXGI 契约复核判断。日常使用已验证的旧式 `FLIP_SEQUENTIAL` 路径。[Windows 首次提交与对照](LOAD_STABILITY_FOLLOWUP_20261116.json)

**修好后用原场景连续测三次。** 每次都核对实机加载的组件全哈希、外接供电、Code 0、真实输出、窗口正常退出、返回桌面和开机记录连续。记录受控重启、自动重启、人工强制重启、网络失联或断电；事件 41、6008 和零 BugcheckCode 只记录关机结果。专项通过且相同条件下没有新增或加重故障的真实修复可直接晋升。既有故障继续登记并附日常绕行。触发方式、阶段或严重程度变化时单列证据；候选验收中的未解释重启阻止相应晋升。

**以仓库和实机为准。** 替换 KMD、tgd3d 或 NVK 前撤销永久许可，候选只给一次性许可；在外接电源、Code 0 及对应场景验收通过后核对实际加载哈希，运行仓库晋升检查再给予永久许可。恢复使用上次已验收整套七项哈希。实机同一时间由一个会话操作，受控重启先在会话说明。Switch 的 SSH 失联后每 3 分钟重试一次。从首次失败起连续 30 分钟仍连不上，就同时暂停进行中的长期目标和当前任务，停止重试。期间重新连通后，下一次失联重新计时，并将断网与设备重启分别登记。修复与原场景复测完成后，把源码、组件全哈希和验收记录推送至对应仓库。[机械晋升检查](../tools/Select-GraphicsBaseline.ps1) · [开放故障](open-faults.json)

**收尾前再查一遍尚未接入的修复。** 对照当前实机七项哈希、基线源码、候选清单、开放故障和已经回退的交付，把每项对应的提交或组件、已有验收及仍缺的证据逐项核实。专项确认生效、对照基线没有新增或加重问题的，当场并入并记录永久许可；其余写清阻碍、日常绕行及下一项验证。将这次复查结果和剩余项目写入仓库，作为第四步的结项记录。

## 第四步实机记录

第四步候选 `60220e9e`（KMD `61787148`）已在外接电源、Code 0 的一次性启动 `85737` 上验证。原生翻转窗口的真实桌面采样、输入和退出通过。原生 `SEQUENTIAL` 两次首次呈现均返回 `DXGI_STATUS_OCCLUDED`，程序退出码为 4。将输入推迟后，完整跟踪仍在应用调度包出现前记录 `STATUS_GRAPHICS_PRESENT_OCCLUDED`。当时窗口位于前台，五个客户区采样位置都归原窗口所有。两次提交均带有 `CrossAdapter` 标志。原重启故障继续开放，原场景通过次数为零。四次受控重启均有对应请求。已在启动 `85913` 核实正式基线七项哈希、DWM 模块和永久许可。[候选实机记录](legacy-sequential-step4/virtual-dma-trial-61787148.json) · [沿用的 WARP 对照及输入结果](legacy-sequential-step4/warp-control-5.json)

第二个候选在同一 KMD 上加入线性窗口缓冲，源码为 `e1314a2c`，ARM64 tgd3d 为 `b4f6c7bf`。启动 `86096` 上，程序和 DWM 都实际加载了这份 DLL。原窗口的源缓冲已关联到 Windows 资源，大小为 921,600 字节。首次提交时窗口可见、位于前台，五个采样点均属客户区。内核仍返回 `0xc01e0006`，公开接口返回 `0x087a0001`。输入通过，程序退出码为 4，没有可验收的呈现后画面。两份完整跟踪均未记录应用的 Blt 或呈现调度包，DWM 也没有这个窗口的消费记录。同一开机的翻转对照则有对应的 DWM 消费记录，呈现后 4,508 个像素与留存参考一致，输入、退出和返回桌面均通过。[组合候选实测及原始证据哈希](legacy-sequential-step4/linear-aperture-trial-b4f6c7bf.json)

留存的 `warp-control-5` 在原绘图线程上记录了标准暂存表面，随后完成 Blt。KMD 此前会拒绝 Windows 的标准暂存表面请求。第三个候选 `aa49f69c`（KMD `09e40be1`）实现了这项分配。一次性启动 `86457` 上，原生旧式窗口首次实际取得了 640×360、行距 2560 字节的目标表面。Windows 的 Blt 事件将源和目标对应到同一 GM20B 应用设备的两个独立分配，DMA 缓冲区也属于同一普通上下文。随后内核返回遮挡，未记录呈现调度包。原生程序退出码为 4，真实输入生效，桌面连续。翻转对照的 4,508 个像素、输入和正常退出均通过。[标准暂存表面实测、调用顺序及恢复](legacy-sequential-step4/standard-staging-trial-09e40be1.json)

第四轮在同一暂存表面实现上加入驱动返回状态记录。一次性启动 `86815`（KMD `13286e3e`）上，原程序的绘图线程进入 `DxgkDdiPresent`，请求一次 Blt，带一个裁剪矩形。驱动在普通渲染上下文处返回 `STATUS_NOT_SUPPORTED`（`0xc00000bb`）。随后的 KMT 返回 `0xc01e0006`，公开接口返回 `0x087a0001`。两份完整跟踪都没有这次呈现的 GPU 调度包。该检查解释了候选窗口返回遮挡的原因。普通上下文仍缺少能验证读写权限、维持资源生命周期并确认 GPU 实际完成的窗口复制实现。[驱动直接返回、组件与对照记录](legacy-sequential-step4/staging-ddi-result-13286e3e.json)

四轮共十六次重启均有 Windows 计划重启请求。启动 `86993` 已恢复正式基线，核对了七项安装哈希、DWM 加载的四个模块、Code 0、外接电源和永久许可。原生旧式窗口连续验收为 0/3。历史启动 `82058`、`82221`、`82382` 的应用及 miniport 跟踪在首次公开调用之后均无可读事件。归档中仍缺与这些启动对应的崩溃转储、串口或早期复位记录。历史整机重启故障继续开放。[候选集成复查](legacy-sequential-step4/integration-review.json) · [开放故障](open-faults.json)

## 已选基线

实机选用源码 `c05e2549`、KMD `76d5c526`、ARM64 tgd3d `cd352fa8` 和原生 NVK `51ff97f6`。一次性许可下的启动记录为 `85471`，外接电源、GPU Code 0，DWM 实际加载的组件和正式永久许可均已读回。上一套可恢复 KMD 为 `fc6e8d08`。[当前七项哈希](graphics-baseline.json) · [实机永久许可](dxvk-games-20261118/gdi-fullscreen-gm20b/permanent-boot.json)

此版本包含 `102218b0` 的空上下文无 DMA 翻转修复、`a82f2510` 的旋转主表面模式尺寸修复，以及 Win32 枚举所需的 `A8R8G8B8` 源模式。前两项合并后的 `8e0e8e40` 仍返回独占全屏错误 `0x80004005`。其原生四方向模式只有 `X8R8G8B8`，该版 Windows 的 GDI 模式枚举筛掉这个格式后生成 800×600 兜底项。补上同一面板可扫描的 32 位格式后，原生 KMT 模式从四项增至八项，普通 GDI 列表从空变为一项 1280×720、60 Hz 模式，当前横屏模式的 `CDS_TEST` 从 −2 变为 0。同一签名程序连续三次取得独占全屏，完成三帧、输入、退出并回到同一桌面。旧式 `SEQUENTIAL` 首次提交重启仍按第四步单独追查。[模式校验及故障定位](dxvk-games-20261118/gdi-fullscreen-gm20b/cause.json) · [修复后的 Win32 和 KMT 模式](dxvk-games-20261118/gdi-fullscreen-gm20b/mode-after.json) · [专项验收及普通窗口对照](dxvk-games-20261118/gdi-fullscreen-gm20b/qualification.json)

## 此前的基线与实机记录

完整 `e6852733` 对应 ARM64 tgd3d `cd352fa8`、KMD `fc6e8d08`、原生 NVK `51ff97f6`。开机记录 `84195`，Code 0，DWM 加载匹配的四项用户态组件。三次原始颜色用例通过，严格 FL11_1 硬件绘图 589 个像素全部匹配，独立 flip 窗口 5 个桌面采样匹配。首次颜色显示工具未取得匹配采样；同一开机、同一驱动下改用处理窗口重绘的采样工具，两项各 5 个颜色均匹配。原套 `57dc16b4` 已核对七项哈希并保留恢复记录。[当前图形基线](graphics-baseline.json) · [永久许可读回](dxvk-games-20261118/dxgi-copy-cd352fa8/permanent-boot.json) · [实机验收](dxvk-games-20261118/dxgi-copy-cd352fa8/qualification.json)

原 WDDM 2.0 实验 UMD `9fb6cef7` 的输入修复已合源码 `abd9f213`，EMC 投票修复 `454ea62d` 亦已合源码；两者单独的源码验证和实际运行组件分别登记。两项旧 KMD 的专项实测见下表。[合并明细](dxvk-games-20261118/dxgi-copy-cd352fa8/merged-source-checks.json) · [两个历史 KMD 的原始记录](TGD3D_COLD_BOOT_HANDOFF_20261113.md)

| 历史候选 | 当时的实机结果 | 特殊调用仍缺少的观察 |
| --- | --- | --- |
| `102218b0` 无 DMA 翻转 | Switch 上的 ARM64 驱动模型由旧实现断言 336 变为通过。移植版 `1e7b8187` 以一次性许可进入 Code 0，正常窗口和 90° 无边框窗口的像素、输入、退出通过。取证版实际收到的 171 次无 DMA 翻转均带上下文。 | 保留候选；需要 Windows 对合法空上下文翻转的实际调用和完成记录。 |
| `a82f2510` 旋转主表面 | 原始驱动模型由断言 890 变为通过。取证版的 171 次主表面查询全部使用 720×1280 存储、90° 显示模式和 `CheckDisplayMode=0`。正式驱动与取证版运行相同的独占全屏程序，均在进入时返回 `0x80004005`。 | 当前逐行驱动在创建时就限定主表面采用原始存储方向。待 Windows 提供四分之一圈旋转存储的有效主表面，再验模式尺寸、可见输出及退出。 |

本轮八次计划重启均有 Windows 请求与对应的开机记录。最后一次计划恢复进入 `84914`，47.020 秒后出现另一次未经请求的启动 `84936`，事件 41 的 BugcheckCode 为零，同时留下 6008；该时段没有新增转储。异常出现在原基线恢复阶段，已单列 `GM20B-NATIVE-RECOVERY-BOOT-RESTART`，与旧式窗口首次呈现的关系待证。结束时已清除两个临时驱动包、试用任务和跟踪会话，实机以外接电源、Code 0 和永久许可回到开机记录 `84936`。原生 `SEQUENTIAL` 首次呈现仍由第四步接手。[硬件结果、完整组件哈希、系统事件和最终恢复](KMD_DISPLAY_CONTRACTS_20261120.json)

当时开放故障有六项，已同步至 Switch 的受保护策略 `3770f769`。仓库与实机均已验证，复用此前的验收报告申请新晋升会被新增的恢复启动故障挡住。此次登记后，原基线的恢复许可仍然有效。第四步先接手旧式窗口首次呈现，再单独核对恢复启动故障的关联和销案条件。[开放登记](open-faults.json) · [实机策略读回](KMD_DISPLAY_CONTRACTS_20261120.json)

## 原会话保留的后续工作

负载稳定后再继续 DX1–11 兼容性和逐模块性能。覆盖系统 DirectDraw、早期 D3D 至 7、D3D8、9、10、10.1、11，优先 32 位游戏的系统图形接口，逐项查实际渲染设备、画面、输入、退出和软件回退。x86 私有驱动绘图已通过，系统 D3D9 HAL 单独追踪。库存 Edge、Chrome 的 H.264 验收须取得同一播放中的浏览器解码器身份和 NVDEC 提交；定制 Chromium 和 wiliwili 各自保留独立记录。[系统 x86 D3D9](dxvk-games-20261118/x86-system-d3d9-windowed-retest.json) · [浏览器 H.264](dxvk-games-20261118/browser-h264-public-fl11-1.json)

第五步继续审 KMD、NVK/NAK、tgd3d/WSI、DXVK/Zink、NVDEC、电源调度、Pacecar、UEFI、SD、Wi-Fi 和蓝牙，Mesa 限于项目实际使用部分且最后审。以画面、帧时间、吞吐、功耗和回归实测对照，保留真实游戏中 Pacecar 多表面归属问题。第六步逐项接入候选和验收真实 SC7 睡眠与唤醒。收尾保留 Tegra DC 列扫描、开机标志、登录与桌面横屏，区分实际画面、寄存器和 API 状态。固定 90° 逐行与 Handheld 电源是可恢复路径。[平台审核](SWITCH_PLATFORM_REVIEW_20261118.json) · [逐行实测](ROW_SCAN_BASELINE_20261116.md)
