# 固定逐行横屏：保留路径、切向故障与实时取证

> 最新结论：用户指定90°逐行工作基线；90°／270°／180°实屏正常，270°→90°正常，
> 但最后180°→90°异常重启。用户要求停止排查并记录。详见
> [本轮基线与失败记录](ROW_SCAN_BASELINE_20261116.md)。下文的设备占用、未执行状态及“下一步”
> 是较早阶段记录，不构成继续测试的授权。

## 当前范围和设备归属

复用已有短时正常上屏的逐行横屏路径：逻辑1280×720，已有GPU预旋转，物理720×1280逐行扫描。
优先定位进入／恢复时的黑屏或自动重启，不重建预旋转、呈现链或现代UMD接口。
列扫描和完整四方向后置；固定横屏、必要窗口交互及恢复通过后再验收原生Win32 vkcube。
当前权威目标为附件 `a754274e-adfb-4ec9-879c-9ddce4145fed/goal-objective.md`。

原FEX侧聊12:41 UTC报告已恢复并归还；用户随后说“还没”“我要用”，主聊天保持冻结。
之后用户在新侧聊 `01a1e470-9787-78d3-8544-b1a26840e6c2` 直接授权“现在可以，临时交给本聊天”，
主聊天核对该直接消息后登记独占，供FEX ARM64EC／WOW64的临时测试和原样恢复。
主目标已恢复active，只做本地工作；设备正式归还前不连接实机。12:22以后的设备状态来自侧聊回执，
不能写成主聊天新增的实机核对。最新占用见OUT `device-lease.json`。

## 当前已恢复组合

活动目录为 `C:\ProgramData\SwitchWOA\GpuUpdate\tgd3d-copy-lifetime-5b66bbec-20261116-122524`。
10:54 UTC核对11项文件、DWM实际模块、Code0和电源策略；12:22交接前再次核对三组件、DWM1424、
默认启用任务Ready，本聊天临时任务／ETW会话均为空。恢复后的物理画面未另行验收。

| 组件 | 源码身份 | 完整SHA256 |
|---|---|---|
| KMD | `c0c6647cec87fcc8d40951d8040ced5d1bbd5f2f` + EMC冻结差异 `6942cee5eb41547e40ebada89a5a8e976caa09985fda3fee509d2f2451bd0852` | `e95287cbeb1d2c325baacb9685affd1d24783b22ecadc1801ba1dffcfe7cc25e` |
| UMD | `c1f25986eee51c0ee01088c3ecb3c9508dea2479` | `5b66bbec86af299941812e8376cc1ac4a5630385828c895b7a52ec96d5c5ad7a` |
| ICD | `c0c6647cec87fcc8d40951d8040ced5d1bbd5f2f` | `95b97df4e83a2a90315a8784d83e75771021093dec34f98362b584d1f546bdff` |

保留Handheld、85%充电、风扇模式1和EMC四档204000／1331200／1600000／2500000。
未提升新的横屏基线；配置核对不能替代实屏或稳定性验收。

## 已存在的逐行路径

- E95对应源码 `panel_vidpn.c` 明确声明 `DriverRotation=1`，由GPU完成旋转后固定逐行扫描。
- UMD已有 `tgd3d_dxgi.cpp::rotated_blit`；KMD已有GDI Present的Rotate复制路径。保留这些实现。
- 5b UMD的 `runtime_primary_create` 已区分存储方向和模式方向，包含横向1280×720／pitch5120分支；
  95b ICD接受显式线性pitch。分支存在不证明某轮runtime实际选择它；当前逐行验收不要求该横向扫描存储。
- KMD、UMD、ICD使用的 `allocation_abi.h` 字节一致，SHA256为
  `860bbf9c80475c7a853d182cea53ef70a03ef9a5e662aa06c04cf9001aa31ecd`，primary描述48字节。

01:34 UTC的历史短时正常上屏组合为KMD
`7cfaa8f565916f8dc28e096cf4a0a05a0845475668a1485c68bc9e1cf279da94`、UMD
`379e521d1d6d82c2eb6d8ef0be179c1bd1270d07965b70ffcf7a4d204585649d`、上述95b ICD。
用户确认短时正常横屏和图案；保留的状态为720×1280／pitch2880／rotation0／列扫描关闭。
历史脚本因要求列扫描而判失败，随后发起回竖屏，状态停在Restoring且没有返回收据，之后重启。
这说明旧验收门槛错误地排除了已工作的逐行路径；**不能因此宣称改测试脚本就修复了恢复重启**。
历史短时成功组合没有重新安装为稳定基线。原记录见 `COLUMN_SCAN_HISTORY_AUDIT_20261116.md`。

## 10:43列扫描候选异常与证据边界

该轮使用既有KMD `102218b0d69af15e1259a9f4047dec049eb8a61228b10eaf3551898cea8172b7`，
保留5b UMD／95b ICD。KMD冻结清单196文件与当前KMD源码一致；清单SHA256为
`d51f42fbc661446e1b332066565c0051212b519795ae237ef8bca5ad4981cd31`，基于e534766c及已有显示修正。
启动后DWM1412确实加载原UMD／ICD。一次普通90°切换停在Applying，用户确认异常自动重启。
没有取得切换后的DC锁存，没有运行图案；不能把脚本阶段直接等同于某个内核DDI已经进入。

保存的44条UMD事件及506条Dxg事件，只覆盖切换前的一段。四组MPO（PresentId226–229）的
源／目标／裁剪均720×1280，segment3三个地址交替；没有据此确认实屏。
最后Dxg事件10:43:49.5862455Z早于Applying文件持久化10:43:50.1346963Z约0.55秒。
ETL未正常结束，`end_time=0`、`buffers_written=0`；零loss字段不能证明完整。
若干PowerShell重定向JSON内容全零，不能恢复其中的画面或寄存器值。
本轮遗漏订阅KMD已有诊断提供程序，故现有记录不足以定位具体未返回调用。

没有可用Bugcheck码、minidump或live-kernel dump；配置为CrashDumpEnabled=3、AutoReboot=1。
这不能单独证明是哪一种内核或硬件故障。该轮加载和恢复各有一次计划重启，横屏应用中另有一次
异常自动重启，均与更早的没电重启及现代UMD切向前失败分开记录。
10:52恢复完成，11:19清理该轮任务、失败安装、输入与采集文件；17项备份已在Mac逐项校验。

## 复用现有导出工具补齐实时传输

只修改 `src/nouveau/tgd3d/tests/trace_export.cpp`，新增 `--follow-all session`：保留UMD既有解码，
同时输出KMD与Dxg提供程序的原始payload、事件版本、PID／TID和QPC。原始字节须结合匹配驱动
或本机事件模板解释，不猜字段布局。实时会话必须为QPC时钟1。

独立线程每20ms请求ETW缓冲交付；这是日志操作，不是GPU提交或Fence完成。
使用的是微软公开的 [ControlTrace刷新机制](https://learn.microsoft.com/en-us/windows/win32/api/evntrace/nf-evntrace-flushtracew)。
新增功能不修改UMD协商、ICD、KMD、预旋转或呈现逻辑。

| 工具产物 | 完整SHA256 |
|---|---|
| ARM64导出程序 | `c64e6cc18315dcb14ca41423cebf0964ade8ebba970307868d39ebcfe5964015` |
| x64导出程序 | `2fb76e7bff66ec78fac335cac741dd08b8152e99498c1407cb11ad314974c8cd` |

11:43原生录制控制通过：旧44事件的输出逐字一致，三类提供程序配置读回一致；Mac流覆盖约8秒，
没有截断或无效记录，已读取的丢失计数为零。相对于TraceStart到达主机的时间，95%交付偏差约95ms，
最大约166ms；这是相对交付测量，不是两机绝对时钟同步或故障时保证。DWM1424及三组件不变。
没有切方向、测试图案、离屏GPU测试或驱动安装；该结果只验证取证传输。

12:17准备逐行进入时，stdin确认未被控制端接受。保护条件阻止启动任务，`TaskStarted=false`，
`test-state.json`不存在，没有实际切向；这是控制工具失败，不能记作显示回归或横屏进展。
记录已收到现有KMD事件2（正常抢占记录），不把它们误报为故障。所有临时任务和会话在交接前已不存在。
本地改为在同一SSH连接的独立通道写入持久化确认文件，接收流不等待该通道；此修正尚未部署／验证。
本地新进入脚本已采用逐行布局判据，进入和恢复分开，不自动因旧列扫描门槛回退。

## 本地补充：解码边界与录制控制

`tools/decode_recovery.py --live-jsonl` 直接读取现有导出器的JSONL，复用原有KMD固定payload解码。
QPC保留为QPC；没有导出的CPU编号不补造。显示调用按LUID、进程、线程、操作及请求参数配对，
缺失进入或返回分别记录。多会话／时钟不明时不配对，截断尾部、全零坏行和未知schema明确报出。
“未见返回”不等于“证实卡死”，解码器不自动宣称覆盖故障。

保留最后一次成功查询的loss计数和最终TraceEnd。两份既有实机流的最终查询均为4201，
不能用该失败查询输出的零计数覆盖前次有效计数。离线重放读到两条正常preempt记录，status均0，
没有新增显示调用或重启根因证据。原二进制格式与新格式共用payload解码，必要控制覆盖坏数据、
截断、丢失计数、跨线程／跨会话配对和原格式兼容。

Mac录制脚本新增最终事件计数及导出器invalid检查；记录完整但方向检查失败时，进程仍失败退出。
确认写入使用同一SSH连接的独立通道，等待该通道期间继续接收并持久化日志。
`--verify-capture` 对应控制端 `-VerifyCaptureOnly`，只验证录制确认，不注册或启动切向任务；
与正式进入使用独立确认文件和收据。实际设备确认通路仍待归还后验证。
本地隔离控制用假SSH覆盖成功、仅录制、方向失败、invalid、计数不符、尾部截断、确认失败、
确认前坏数据和缺少TraceEnd，不能将这些结果当成真实SSH、runtime或GPU通过。

历史379 UMD产物已重新核对完整SHA256；对应源码来自 `private-scanout-format-20261115/source`，
连同继承的 `rotation-teardown-contract-20261115` 冻结清单逐项核对。与当前5b的协商入口文件相同；
原有rotated_blit仍存在，当前版本增加源矩形和专用push constants，legacy Blt仍传全图区域。
这些是源码差异，不能证明实机必定经过哪一支，也不支持撤回既有复制／生命周期修正。
差异及身份保存在OUT `row-switch-evidence-20261116/component-delta/`。

KMD对照也不能只按“.158”合并：E95源码的DriverRotation为1；短时成功7cf源码为0，
并包含私有扫描格式、PlaneEnabled及布局锁存等差异。两者曾走逐行输出不等于组件相同。
本轮没有移植这些差异；先复用当前E95已明确声明的GPU旋转路径取得动态调用证据。
部分历史文件未被选定冻结清单覆盖的限制逐项保留在 `kmd-source-delta.json`，不补造来源。

本地更新包为OUT `row-switch-evidence-20261116/prepared-v2/controller-update.zip`，
只包含控制脚本与输入清单，其余工具沿用原哈希；旧stdin确认失败的输入及收据仍保留。
包未上传，PowerShell原生解析和确认通路控制待完成，不直接重放旧 `entry-input.zip`。

## 下一步与审查状态

设备明确归还后，先核对实际组件、配置、用户显示状态以及仿真器还原结果。保留并归档未启动的
确认失败收据，验证修正后的确认通路，再沿当前已有逐行路径做一次受控进入，保存最后进入／返回的
真实DDI及相关资源、提交、MPO与锁存。发生异常停止扩大测试并安全恢复；没有新证据不猜根因。
当前重启根因、固定横屏实屏、必要窗口与恢复、vkcube均未完成验收。

Gemini4 Carbon High独立审查待2026-11-20T23:53:18Z额度恢复，未重试、未声称通过。
VPS Claude只读复核调用未得到审查结果，用户确认Claude额度不可用，不重试或改配置。
完整产物／收据在OUT `legacy-column-handoff-20261116/`、`row-switch-evidence-20261116/`；
当前设备独占状态见OUT `device-lease.json`。外部DXVK／SC7和NVDEC／D3D11VA工作仍独立保留。
