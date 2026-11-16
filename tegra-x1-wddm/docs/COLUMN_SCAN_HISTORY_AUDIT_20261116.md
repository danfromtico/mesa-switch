# 列扫描删除前与恢复实现对比

> 本文保留历史源码对照和当时实测，不描述当前安装状态。当前阶段已改为复用固定逐行横屏，
> 列扫描和完整四方向后置；以 `TGD3D_COLD_BOOT_HANDOFF_20261113.md` 顶部及
> `ROW_SCAN_ROTATION_EVIDENCE_20261116.md` 为准，不执行本文中的旧恢复或列扫描验收门槛。

比较起点为 `01e5aa3ea0f8a5baa69930265fed2493dc858a70` 的父提交
`54dc559731a48408266177bdbae192f04a643536`，终点为 `tgd3d-vulkan`
`e534766cff97a3407deed4478f18909e18ff5190` 加当前工作树。
旧代码仅作实现参照，不是已通过的驱动基线。没有整批回退任何提交。

## 证据范围

- 历史红屏强开试验使用逐行扫描，不能用它判断列扫描黑屏、列扫描 FIFO/带宽或列扫描锁存是否正确。
- 较早 Noble 内核 #5 读回为逐行扫描；较新的同机 Noble 24.04.5/#149 侧聊天交接报告列扫描、实屏正常。两份记录属于不同组合，不合并为一个结果。
- #149 交接值为 WIN_OPTIONS `0x40010011`、pitch `5120`、H_OFFSET `5119`、V_OFFSET `0`、物理720×1280；本窗口读取的是交接文件，没有重新抓取 #149。它不能代替 Windows 验收。
- Windows 原有普通 DWM 横屏记录出现过逻辑1280×720，但 DC仍是720×1280/pitch2880/逐行扫描、内容已经预旋转。后来的独占窗口 E_FAIL 是另一条路径，不再作为桌面横屏的前置条件。

来源与逐文件哈希保存在 OUT `column-scan-history-audit-20261116/`，包括
`before-deletion/`、`current/`、`source-manifest.json`、`history-to-current.diff`。
Noble 交接为 OUT `emc-runtime/qos-admission-review-fixes/column-scan-handoff.json`。

## 主表面布局

旧、新 `tg_t214_dc_get_layout` 的四向结果完全一致，已分别编译成主机程序逐字段比较。
下面是去掉 WIN_ENABLE 等非旋转位之后的值。

| 逆时针方向 | 存储宽×高 | Pitch | 方向位 | H_OFFSET | V_OFFSET |
|---|---:|---:|---:|---:|---:|
| 0° | 720×1280 | 2880 | 0x00 | 0 | 0 |
| 90° | 1280×720 | 5120 | 0x11 | 5119 | 0 |
| 180° | 720×1280 | 2880 | 0x05 | 2879 | 1279 |
| 270° | 1280×720 | 5120 | 0x14 | 0 | 719 |

两者都保持物理输出720×1280、PRESCALED=2880字节×1280、单位DDA。
在全屏无缩放列扫描时，源坐标轴先交换，所以 PRESCALED 不等于源内存 pitch。
反向水平起点取最后一字节，是固定 L4T 窗口源码的规则；不能改为最后一个像素的首字节。
原始 L4T 代码只用作硬件规则参考，没有移植 GPL 实现。

当前比旧版增加了存储方向与模式方向的区分、CheckDisplayMode专用描述和私有格式属性。
标准 shared primary仍用720×1280，shadow允许横向尺寸；DXGI primary才通过不可变描述
携带1280×720存储。不能从 `PathRotation=90°` 直接推导旧地址已经有5120 pitch。
该差异也解释了为什么仅恢复列扫描寄存器代码，普通DWM仍可能选择预旋转的逐行主表面。

## 显隐门控：本轮发现并局部修正

删除前已有 `SourceVisible` 与 `TargetActive` 两个独立状态。当前恢复实现增加
`PlaneEnabled` 和 `FormatAwaitingPrimary`；后者防止私有格式已变化但新主表面尚未绑定时
重新读取旧布局。这项保护保留。

此前 `PlaneEnabled` 的逻辑会在 MPO `Enabled=FALSE` 后，让源 `Visible=TRUE` 返回成功
却仍不启用主平面。微软 [MPO VidPN呈现契约](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/multiplane-overlay-vidpn-presentation)
（2021-12-15）明确要求此时开启主平面，其他overlay保持禁用。当前只有Window A一个主平面。
旧独立审查认为此时必然已经释放backing，但这不能作为忽略显式主平面恢复请求的充分依据。

本轮局部修改 `panel.c::set_visibility`：显式源 `Visible=TRUE` 恢复主平面启用意图；
单独目标开/关不能取消MPO禁用。继续使用OS当前指定的地址、方向和PresentId；不写POST替代图，
不跳过格式保护，不伪造翻转完成。窗口enable变化仍排队到VBlank并等待活动bank确认。
该操作依赖WDDM为当前源绑定维护的驻留；不能拿这项修改证明失效/已销毁backing可重新使用。

受控测试固定保留所有allocation和backing，覆盖MPO禁用→目标开关→源恢复、错误重绑、
四向恢复、目标关闭期间的源请求、格式门控及PresentId不变。修正前后用同一测试比较。
结果单独保存于 `visibility-arm64-build.json` 和 `native-controls-response.json`。
这验证契约分支，不证明历史物理黑屏必由这项引起。

## VidPN到DC

删除前不声明SmoothRotation，拒绝改变角度的path-only更新；当前声明SmoothRotation，
更新时记录 `PathRotation`。两者都由后续有效primary或MPO参数决定实际扫描布局，
Commit/SetTimings不把旧帧直接解释成新布局。

当前缺少的是普通DWM选择新布局的端到端证明：源/目标模式与方向被接受，不代表DWM提供了
横向backing，也不代表列扫描已锁存。下一轮只沿普通桌面模式切换和已有GDI/DWM图案核对。
主平面布局、显隐、活动扫描地址/方向都必须对应同一轮，不能再用独占交换链失败阻挡桌面验证。

另有两条必须区别的旧路径：`tgd3d_dxgi.cpp::Blt` 收到非零Rotate时调用 `rotated_blit`；
KMD `windows/present.c::TgGpuDdiPresent` 的GDI Blt收到Rotate时按PathRotation生成真实复制。
两条都可能产生已预旋转的720×1280表面，不是DC列扫描。当前已协商的UMD仍是
D3D10.0/DXGI1.0或1.1，未注册的Present1/MPO函数不能算实际调用链。
应补齐普通桌面新主表面的协商/交接，而不是删掉OS明确要求的Rotate操作后直接报成功，
也不能把恢复旧寄存器函数解释为已经恢复普通DWM列扫描。

按 [微软旋转规则](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/supporting-rotation-in-a-display-miniport-driver)
（2021-12-15），共享primary的非旋转尺寸和有无Rotate标志必须分别处理；
MPO的 [Rotation方向](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/ns-d3dkmddi-_dxgk_multiplane_overlay_attributes3)
（2024-02-22）为顺时针，与VidPN/GDI的逆时针约定不同。旧、新代码都显式转换，
没有因两个枚举数值相同而统一为同一物理方向。

## 锁存与退休

旧、新都先UPDATE再ACT_REQ，布局/可见性变化走VBlank，只有相同布局与可见性的地址变化
允许HBlank。当前加强活动bank的几何验证：像素格式、尺寸、预缩放、DDA、pitch、offset、
surface kind均需一致；不再只比较地址，也不在窗口ACT_REQ未清时接受同地址的新PresentId。

普通VSync回报实际扫描地址；MPO VSync回报已锁存的PresentId。保留真实通知与DPC，
不强制添加HsyncFlipCompletion。模式切换保持旧backing直到匹配的地址/布局锁存。
这些是当前源码行为和控制覆盖，不等于实机已经证明所有退休顺序正确。

旧模型和当前模型分别通过全帧寻址、四向扫描、HBlank/VBlank与POST恢复控制；
`dc-model-comparison.json` 和 `layout-comparison.json` 明确标为CPU模型。
没有据此认定OLED时序、列扫描带宽或物理显示通过；本轮没有修改面板/DSI寄存器、
电源、CPU/GPU/RAM/风扇模式、85%充电限制或双向GPU Fence。

当前保持验收的 E95/4c8/95b + Fan33/Core7/Battery14；本轮门控修正还不是基线。
同时开发的现代UMD接口桥保留为未协商、未安装的工作，不混入该项KMD对照。

## 普通DWM实机结果 — 2026-11-16 01:34 UTC

相对前轮列扫描候选cf30/379/95b，本轮只更新KMD为
`7cfaa8f565916f8dc28e096cf4a0a05a0845475668a1485c68bc9e1cf279da94`；
与已验收基线E95/4c8/95b相比，这仍是配对的实验KMD/UMD组合，不是基线升级。
候选完整身份与源文件清单见 `candidate-package.json`，13项原生签名/目录检查通过。

01:30:01 UTC完成计划安装启动；01:33:38达到NativeEnabled，DWM PID1388实际加载
379 UMD及95b ICD，Code0、GPU bridge开启。平台驱动、固件及电源策略保留。
本轮原生LUID为 `00000000000c9806`。没有独占交换链、红屏强开或GPU离屏测试。

01:34:26开始普通DWM测试，01:34:29逻辑桌面为1280×720、orientation1、60Hz。
用户两次澄清的最终物理观察是：成功横屏约1秒后显示四色图案，关闭图案回到正常横屏
桌面约2秒，随后自动重启。**短时物理横屏上屏已确认；稳定性失败。**

原子持久化的 `test-state.json` 记录面板读取成功，但实际布局仍是
720×1280、pitch2880、rotation0、ColumnScanObserved=false。这说明本轮观察到的是
预旋转/逐行扫描路径，不能归为DC列扫描通过。原始 `panel-landscape.json` 与图案日志
在重启后为全零，不能补造其中的地址、像素或锁存细节；仅采用保留下来的状态字段。

图案进程退出0。测试脚本因列扫描布局检查未通过，于01:34:32.642开始恢复原竖屏；
保存状态停在Restoring，没有RestoreExit，也没有完成后的模式输出。
系统新启动时间为01:35:13.859，吻合用户观察的异常重启。
因此重启发生在发起恢复方向之后、恢复调用返回之前；这缩小了排查范围，
**尚未证明由模式恢复、资源退休、某条GPU命令或门控修正本身单独导致**。

本轮不重复安装或重放。01:38:14发出一次当前验收基线的恢复任务，状态另见交接记录。
保留这次短时上屏证据，继续处理普通DWM主表面的列扫描协商及双向切向交接；
不以仍在预旋转的画面、旧逐行红屏或CPU模型通过代替列扫描验收。

侧聊天补充了匹配系统内核的布局校验复核。本窗口核对本地 `dxgkrnl.sys` 的SHA256为
`99d425e1c997eae32909fa4f37d6c8e2e846a5374c602295546c8be2547aa5e6`，
与既存 `fullscreen-mode-contract-20261115/kernel-SetDisplayMode.asm` 的调用点一致：
CheckDisplayMode描述先检查宽高，再检查要求的Rotation。当前候选已保留
`allocation.c::describe_allocation` 对模式尺寸/ModeRotation与存储布局的区分，
没有因此再次交换实际backing的尺寸或绕过系统校验。本轮没有该拒绝路径被实际触发的证据。
PDB身份匹配来自侧聊天复核；接收记录见 `side-review-layout-validation.json`。
