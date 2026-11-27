# tgd3d 原生桌面试验与异常重启

状态：短时原生上屏，稳定性失败。当前开发分支为 `tgd3d-vulkan`，原名
`claude-vulkan`。本轮没有五分钟、已知像素、硬件旋转或 vkcube 通过结果。

| 组件 | 源码提交 | SHA256 |
| --- | --- | --- |
| 签名 .158 KMD | `6c854e9f394c3d761fab85885bead4bbd4588581` | `46d1757674ff156028ce5b729ba53671981ffab09948fcaf125751c46bb10088` |
| TegraGpuD3DVk.dll | `3c5d327daa77b40ca1e822d5270e285a71a61807` | `8232217dbedeee4c4b61c08a319b2021b2889f6a618ab1a975c89f86823c0a64` |
| ARM64 NVK ICD | `7c677212e5843725f3dbb12ee0b9dad6b1339a05` | `17dc3110393365eef279a90104850d6f517c74659441916266afeae0ae2c8f73` |

Windows 启动事件记录版本 `10.0.22621.2861`。交付工具提交为 `6c1340c3`。
原测试包 SHA256 为 `88b8220db9c5eb7b129e057698bbfe07989d1d89255d28a1428669b8f8740198`。

控制脚本在 2026-11-13 05:06:20 UTC 记录 .158 Code 0、Hardware 0、Panel 0、
一次性许可消费为 0。原生活动显示路径与 KMT LUID 都是
`000000000009b66c`，720×1280。DWM PID 800 的独立 tgd3d provider 保留
NVK 同 LUID、CreateDevice/S_OK 和 251 次 Present/S_OK；用户随后确认画面正常、
能够操作。此后用户打开桌面的 CPU/风扇/温度程序并观察到自动重启。

桌面入口事后只读核对为无参数的 `LitePerformanceCheck.exe`，34304 字节，
SHA256 `ca127482bbb59ac6c1b2f576f6b9793231c1b865a00057ec1171892f210929d4`。
这是时间顺序证据，不能据此认定该程序、UMD、KMD 或某个系统驱动是根因。

保留主 ETL 导出 1621 个 tgd3d 事件及 277190 个原始记录。未保留 DWM error
事件或 KMD recovery 事件；其它进程的 `0x887b0001` 位于 staging Map 的
DONOTWAIT 路径。最后保留的 DMA 和原生 Flip 有完成记录。主 ETL 的 EndTime
和 BuffersWritten 为 0，尾部不完整，不能用“没有错误事件”证明没有错误。
系统记录 6008/41，BugcheckCode 为 0，没有可用 minidump。系统事件、ETW 和
控制脚本时钟存在偏移，保留各自原值/RecordId，不能混用它们计算精确崩溃时长。

## 验收工具问题

1. 固定 `desktop-path.exe`（`6a10be74…`）先输出 DisplayConfig/KMT 两条记录，
   再查 argv DLL 的 `TgGpuReadD3DAdapterProbe` 私有导出。冻结的 no3d 桥没有该
   导出，工具在执行 D3D11CreateDevice 之前退出 4。这正是所选只读路径；原断言
   错误地要求退出 0。修正要求该已知桥/工具的退出 4、两条完整记录、KMT S_OK、
   活动路径/LUID 匹配，并另用真正 DWM 的 ETW 判断 UMD 创建和呈现。
2. ETW 转换后的 FILETIME 相对控制脚本墙钟约偏移 15 秒，会误滤除已发生的
   CreateDevice。下一版 live consumer 使用原始 QPC，启动请求也记录 QPC；
   UMD v2 事件携带发出进程的创建 FILETIME，以精确匹配当前 DWM，而非用两个
   时钟的先后关系推断身份。v1 数据仍可离线解码。
3. 初次目录枚举把 `umd-live.jsonl` 长度报为 210，但其 SHA256 与归档中的
   291273 字节完全一致。随后用同一个文件句柄计量长度和计算 SHA256，全部
   144 个文件、154786727 字节匹配。原清单和文件句柄清单均保留。

时钟处理依据微软 [EVENT_TRACE_LOGFILEW](https://learn.microsoft.com/en-us/windows/win32/api/evntrace/ns-evntrace-event_trace_logfilew)
的 ProcessTraceMode，以及 [AutoLogger 配置](https://learn.microsoft.com/en-us/windows/win32/etw/configuring-and-starting-an-autologger-session)
的 QPC ClockType=1。runtime callback 审查依据
[Changes from Direct3D 10](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/changes-from-direct3d-10)
（页面更新日期 2021-12-15）：线程规则依赖 DDI 与 FREETHREADED 声明，不能把
D3D11 放宽规则直接用于未声明该能力的 D3D10 路径。

## 恢复与下一步

异常重启后门控关闭，重复启动防护要求回退，没有再次原生启动。
05:12:25 UTC 显示 restore/verify 均为 0；05:20:46 UTC 再核对 .59/Code 0、
原 ACPI、CPU 1497600 kHz、风扇13和原电源设置，临时任务/许可/注册/ETW均清理。
证据 zip 为15894489字节，SHA256
`1b5e5b9b623a63d0d7a6709b74c8a9e148348bd7b7d2aa07bce4f796d54dc4f8`。
证据在独立产物目录 `hardware/`，逐文件核验见 `verified.json`、
`evidence-manifest-stream.json`，恢复见 `handoff-verified.json`。

先修复上述验收问题，再针对异常重启形成有证据支持的单项补丁。复测按约30秒静止、
约60秒简单开窗、五分钟交互逐步推进；CPU/风扇/温度程序为独立回归步骤。
工具修正或控制测试通过不构成重启已修复，也不能替代物理观察。

## 验收修正的控制验证

提交 `4309434c8b2f087883e673515df16aa346f5903d` 只修改取证关联与相关工具，
没有修改 KMD、ICD 或 Vulkan 绘制/同步逻辑。ARM64 UMD SHA256 为
`d90404c27cfa999aad9b6645289458599c7a41abdfe782d883543cf815f0273e`。
构建时已跟踪源码干净，唯一未跟踪项是 `.DS_Store`，清单如实记录。

2026-11-13 05:49:57 UTC，在 `.59 / Code 0` 上通过20个脚本的语法检查、
18项读取判定（包括墙钟前后偏移、PID复用、错误时钟/频率）、11项CPU合约检查，
并用真实Windows ETW传输6条合成事件，实时与离线字段一致、3个完整实时缓冲状态。
新版解码器还读回旧v1的全部1621条事件，不为旧事件捏造进程创建时间。
19个控制证据文件/665881字节已按长度和SHA256取回校验。
控制证据zip SHA256为
`44626b595c51155b2e65761d600f302c3950c8523bee7c0bcb2a3c4ebe59c86d`。
这些是工具和CPU控制，不是GPU验证或重启问题已修复的证据。

随后在 `.59` 进行同一性能程序的无参数对照：进程7776持续运行30.18秒，
06:01:53 UTC正常退出0，控制进程连续运行，GPU仍为`.59`，没有重启。
这仅证明该次基线运行成功，不能排除程序I/O与原生路径的交互问题。

## 第二轮：静止阶段通过，开窗阶段异常重启

本轮只更新取证工具和分阶段观察，没有修改绘制、同步或 KMD。组件为上述
`4309434c8b2f087883e673515df16aa346f5903d` UMD、原 `7c677212` ICD 和冻结
`.158` KMD；完整组件 SHA256 见前述清单。阶段工具为 `b4942e7a`，测试包
6718393 字节，SHA256
`2a7ce341d84915c0287fed038e578d311f235897c46893f4451b73ddc8e80833`。

试验目录为 `tgd3d-4309434c-20261113-142254`。Windows 仍为
`10.0.22621.2861`；WMI 启动时间为 `06:24:41.602617 UTC`，控制记录在
`06:26:56.6543149 UTC` 确认 `.158 / Code 0`、Hardware/Panel 0、许可消费为0。
原生 LUID 为 `0000000000086d7d`，DWM PID812，创建 FILETIME
`134390247145347024`。原始 QPC 与进程创建时间匹配了本轮 DWM 的 NVK 初始化、
设备创建和呈现，修正后的 helper 退出4判定也通过。

静止观察在 QPC `2935736109` 开始，频率19200000；0/15/30秒检查均成功，
30秒后截图，观察计时40.27秒。用户确认“画面正常，能操作”，该阶段通过。
随后请求60秒开窗阶段，用户报告“已经死机重启了”，并确认“尚未看到彩色窗口
就死机”。没有 Window observer 的动作/结果，也没有五分钟或性能程序回归结果。
用户另补充系统自带窗口可以使用，而新程序开窗时会重启；这是排查差异，尚非根因。

保留数据如下：

- 实时 tgd3d 日志2219条事件、92条状态，DWM 389次成功 Present；离线主 ETL
  多保留最后6条事件，合计2225条、DWM 391次成功 Present 和394次成功提交。
  DWM 的 Present 回压耗时均为0，保留的一次CPU等待被旧工具记为读回，但同一
  wait_sequence 帮助函数也用于销毁，无法据此区分两者；Explorer 有两次
  两帧上限产生的回压。跨队列 GPU Fence 已实现，不代表全部API路径都无CPU等待。
- 原始 ETW 339152条；已用该机模板解码335068条。1857条 Dwm-Core 203事件
  的载荷与模板长度不同，明确排除。没有保留 KMD recovery/execution 事件。
  最后的原生 DMA1885有完成记录，随后有 PresentId391 的 MPO 回调；尾部仍有
  排队记录。主 ETL EndTime/BuffersWritten 为0，不能用尾部未配对事件认定GPU挂起。
- 系统事件记录6008/41、BugcheckCode 0；没有对应 Minidump、MEMORY.DMP、
  LiveKernelReports、用户转储或 WER 报告。EventRecordId40235是异常重启，
  40289是恢复脚本正常请求重启，两者必须区分。墙钟经过校正，不能混算ETW、
  WMI与系统事件时间来推导崩溃耗时。
- 后续只读补查的 PowerShell 400事件36883确认控制进程PID3864启动过
  `Start-NativeStage.ps1 -Stage Window`。脚本块日志只能证明解析/加载，不能
  当作每条命令已执行。TaskScheduler Operational当时未开启。
  `stage-Window.request.json` 保留408字节全零内容，说明原请求记录未完整保存，
  不能据此证明 observer 已启动。后续工具改用 WriteThrough、Flush(true)、
  pending文件完成后发布，并携带控制进程PID/创建时间；不把这项取证修正当作重启修复。
  更新脚本已于06:47:18 UTC在Windows PowerShell通过语法解析（0错误），未执行
  新的原生动作，也没有用解析成功证明断电持久性。

06:33:42 UTC完成恢复核对：`.59 / Code 0`，SYS/ACPI哈希与原基线完全一致，
显示restore/verify均为0，CPU1497600kHz、风扇13、电源/睡眠设置保持，
临时任务、许可、UMD/ICD注册及ETW已清理。用户随后确认“已回到桌面，能操作”。
没有再次启动该失败组合。

证据163个文件、157525799字节，逐文件长度和SHA256全部匹配；归档18635015字节，
SHA256 `3b94140003b54590383533e28ddefbafd283e1b74cfa3a39c895d7d4ef7f306b`。
独立产物目录为 `hardware/staged-4309434c/`；归档、`verified.json`、
后补系统事件与物理反馈分别保存，未改写已归档的原始记录。

逐项审查 `main` 的 `0ced476d` 和 `7a1f7176`：前者修改 Gallium/Zink 的图像
映射与缓存范围，不在 tgd3d 直接调用链；后者扩充已有 TDR 回调的缓存报告，
不负责建立 SD 转储通道，也不能保证硬复位能留下转储。本轮均未移植或部署。
CreateProcess/CreateContext的冻结KMD实现与当前支线相同，审查参考微软
[CreateProcess](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/nc-d3dkmddi-dxgkddi_createprocess)
（2025-02-07）与
[CreateContext](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/nc-d3dkmddi-dxgkddi_createcontext)
（2024-02-22）；仅有新进程时间关联，不足以认定某个回调违约。

### 已安装的 SD 转储路径

按用户提醒核对主会话的存储安装记录及实机：06:45:46 UTC，SDMMC1
`ACPI\NVDA0101\0` 为 **0.5.1.2 / oem90.inf / Code 0**，48056字节 SYS，
SHA256 `5389b30d3a04fad330b9afef36b0d500ad363c6c5b39c55c9b60096669bfd945`。
与 Windows-for-Switch-Drivers 的
[固定安装记录](https://github.com/ruje0504-2/Windows-for-Switch-Drivers/blob/6cd7305e7e932b4df4bb4e53282fedd9d53e8bd8/drivers/Tegra210Sdhc/README.md#0512-installation-and-runtime-result)
匹配，记录的SD源码为 `5345692e4b7ec0d52e80937ae451bb2c52750fe8`。
该版本保留SDPORT的独立PIO转储支持；无需因GPU回退重新安装SD驱动。

实机 CrashDumpEnabled=3、AutoReboot=1，分页文件512MiB。补查到06:39 UTC的
System记录中，本轮开始之后没有volmgr事件；旧的45/46记录早于这轮。
这证明没有再次观察到初始化错误，仍不是成功捕获转储的证明。两次tgd3d异常重启
都没有可用转储。微软
[Event 41说明](https://learn.microsoft.com/en-us/troubleshoot/windows-client/performance/event-id-41-restart)
明确指出零Bugcheck字段也可能来自无法写盘或关机过快，不能排除内核崩溃；
[小转储说明](https://learn.microsoft.com/en-us/windows-hardware/drivers/debugger/small-memory-dump)
描述了可用于定位的崩溃线程、调用栈和驱动列表。本轮未修改SD、转储或分页配置，
没有触发人为bugcheck。存储源码仍由其原仓库维护。

## 回退后取证进程耗尽内存

第二轮回退之后，用户又观察到 `.59` 桌面卡住、Explorer 系统错误弹窗，随后
黑色背景只剩图标，部分时刻仅触摸可用。这是另一轮基线故障，不能继续把此前
`recovery-verified=true` 当作系统持续稳定的证明。

用户手动重启后，原生命令取回了 System 2004 事件40390、40394、40398、40413、
40418、40429。它们反复指向 `powershell.exe` PID2408，创建时间
`2026-11-13T06:43:36.1130143Z`。该进程提交内存从4542185472增长到5261848576字节；
事件40413的系统提交量与上限同为6118985728字节。Explorer挂起事件15461、15497
分别记录于07:21:36和07:24:26 UTC。没有取得该弹窗的异常码或分配调用栈。

PowerShell 400事件36963记录同一个PID的完整EncodedCommand。解码后，与本窗口
发起的 `sd-dump-state.ps1` 加固定输出编码前缀逐字一致，SHA256为
`f4dc3dd17b57f0a03593880e1c2c6e5f162dc95cddb4517bedaff56f53b0e56b`。
该临时脚本使用CIM/PnP查询并将注册表PowerShell对象送入JSON序列化；本地SSH
超时后远端进程仍存活，后续取证未及时识别其内存增长。这是本窗口取证流程的
明确问题。没有栈证据，不能进一步断言具体是哪条CIM或序列化语句造成增长。
也不能用这次内存耗尽替代此前两轮原生tgd3d开窗重启的根因。

手动重启启动事件为07:27:40 UTC。07:32与07:44两次进程清单均无PowerShell；
07:44可用物理内存2287132 KiB，可用虚拟内存3002552 KiB。`.59` SYS及ACPI哈希
与原基线一致，风扇13/CPU1497600 kHz、SD0.5.1.2及Wi-Fi0.5.7的实际SYS哈希
均匹配安装记录，显示和睡眠超时仍为AC/DC均0。这是恢复时点的状态核对，
不是新的长期稳定性验收。

后续SD/基线取证改用主机端 `tools/collect_recovery_health.py`：调用原生
reg、pnputil、certutil、wmic OS和限条数的wevtutil；SD只指定
`ACPI\NVDA0101\0`，不再执行上述PowerShell/CIM库存脚本。输出直接写本地文件，
SSH异常或超时记录“远端结束未知”并停止，不继续堆叠取证进程。
六项基线命令在07:44全部退出0，五项SD/转储命令在07:47全部退出0；没有新装
驱动、调频或开启原生测试。旧脚本与原始事件保留在独立产物目录
`hardware/post-recovery-explorer-20261113/`及其相邻的`staged-4309434c/`。

CPU寄存器读回1497600 kHz、OverclockActive=1、Throttled=0、LastError=0；
Windows的CentralProcessor记录仍为998 MHz，与用户照片的任务管理器1.00 GHz
相符。当前SwitchFan直接控制PLLX，不能用Windows的旧频率显示否定硬件读回；
这里没有声称已测得应用性能提升。

事件2004与提交量的解释参考微软
[应用内存泄漏排查](https://learn.microsoft.com/en-us/troubleshoot/windows-server/performance/troubleshoot-application-service-memory-leaks)
（2026-11-13查阅，适用于Windows Client/Server）：默认工作集列不能替代提交量。
本次没有增加分页文件或改变GPU预算来掩盖取证进程增长。

## 开窗重启：补齐调用进入记录

冻结的早期Zink上屏实现`ee7b0d50`在RuntimePresent前通过
`screen->fence_finish(..., UINT64_MAX)`等待NVK完成；tgd3d使用双向GPU时间线，
只在回压/CPU访问/销毁时等待CPU。这个差异可以形成后续对照，但本轮尚未把
CPU等待改回去，也未认定时间线是根因。两者的着色器前端和ICD也有差异，
不能仅凭旧Zink短时开窗便排除共享资源或编译问题。

此前日志仅记录编译、等待及提交返回，无法识别卡在调用内部的情况。新增
事件14为设备创建阶段（runtime context、VkDevice、共享Fence、command pool、
descriptor layout）；15为DXBC/SPIR-V前端和CreateShadersEXT后端；16为
runtime signal、Vulkan QueueSubmit2和runtime wait。进入为`E_PENDING`，
返回为对应状态，按PID/TID及阶段配对；编号4/30-kind2才是原来的成功设备/
Present依据。时间记录30-kind4是CPU等待进入，kind3仍是结束，并增加原始
VkResult。DESTROY调用现在显式传入原因，旧reason2不能反推它一定是读回。
诊断改变不修改KMD、ICD、时钟、预算、同步算法或NAK编译算法。

2026-11-13 07:58:13 UTC，在`.59 / Code0`运行12项ARM64 CPU控制全部通过，
新增控制令等待返回VK_TIMEOUT或VK_ERROR_DEVICE_LOST，验证未完成的命令和
view仍保留、没有CPU信号伪造完成。实时与离线各14条合成ETW记录字段一致，
3个实时状态记录无丢失，进入/返回及销毁超时分类检查通过。控制包仅含测试和
取证工具，没有加载新的UMD或创建GPU设备。构建曾因没有使用项目已有Python
环境而配置失败，改用已有环境后完成ARM64构建；未修改系统Python配置。

### 旧WATCHDOG转储

10个已取回小转储的头部均为0x141，日期都早于本次tgd3d试验。对最新的
`WATCHDOG-20261112-0204.dmp`，07:57 UTC已用系统自带ARM64 DbgEng成功读取。
实际捕获时间是2026-11-11 18:04:24.860 UTC，系统运行309.031秒；它是旧的
GPU引擎超时live dump。参数2落在`tegragpurender.sys+0x4cfc`，只能说明
报告归属地址，不能当作最后执行指令。

给DbgEng提供匹配系统镜像后可展开12层CPU栈，经过watchdog、dxgkrnl、dxgmms2
和nt；没有匹配PDB，若干名称只是最近导出符号加偏移，不能据此断言具体内部
函数。还读出GUID`270A33FD-3DA6-460D-BA89-3C1BAE21E39B`的19990字节TDR标签。
尚未从中解出当前UMD着色器、GPU命令流或当前两轮硬复位的原因。

读取器`inspect_kernel_dump.cpp`只打开已有转储，不附加活系统；本进程限制
256MiB提交内存并在45秒自行退出，防止SSH断开遗留分析进程。两次受限读取均
正常退出0，第二次解决了第一次缺少镜像unwind表的问题。日志和完整原始标签
保留在独立产物目录`debugger/watchdog-0204-images.stdout`。微软
[0x141说明](https://learn.microsoft.com/en-us/windows-hardware/drivers/debugger/bug-check-0x141---video-engine-timeout-detected)
说明这是live dump并列出参数与TDR标签；它不等于拿到了GPU指令栈。

## 第三轮：静止通过，隐藏窗口与共享ETW问题

UMD源码`d6470feb38defeb824f34cda39ffbb9e886f611d`，SHA256
`e25c01c90cb63587257baaa67468d480821e41f5e406cea410f220a3db115818`；KMD仍为
冻结`.158`，ICD仍为`7c677212`，各自哈希未变。测试包6721338字节，SHA256
`590f0bd4eec4eeef08a4a54fee0bc2820fa1939a94a2f94eecf73b5a9f9ca04a`。
本轮目录`tgd3d-d6470feb-20261113-160356`，原生启动记录08:07:48 UTC，
LUID`000000000008f379`，DWM PID580、创建FILETIME`134390307694560305`。
静止阶段0/15/30秒检查通过，观察42.93秒；用户确认“画面正常，能操作”。

08:11:03 UTC窗口子进程被启动，PID2576在08:11:06写出Managed Visible=true，
记录8次移动/缩放并于65.03秒退出0。但用户没看到窗口，08:11:33的桌面截图
也没有它。08:12:23观察器因共享会话`events_lost`增加而失败。最初将其说成
“开窗前被工具中止”不正确；后来取回完整动作和进程记录后已更正。

保留ETL有正常EndTime、1365个写出缓冲和1个丢失事件；解码6958个UMD事件，
其中DWM有423次成功Present、429次成功提交。132个着色器返回均成功；
75对设备创建阶段、264对编译阶段、1809对提交阶段的进入/返回数量一致。
保留的两次CPU等待是READBACK，返回VK_SUCCESS；其它错误记录为
DONOTWAIT Map的`0x887b0001`。这不构成实际窗口显示、真实60Hz或完整无丢失验证。

回退首次被清理竞态阻断：停止ETW后，consumer PID1772自行退出；随后
Stop-Process按PID停止时抛出“找不到进程”。08:17:49确认记录的collector都已
退出后重试恢复，08:20:20完成`.59`/原ACPI/显示和平台设置验证，用户确认
“已回到桌面，能操作”。原错误与重试记录均保留。用户报告风扇较快时，
08:15的读回为SoC38C、板温36C、PWM118/236、CPU1497600kHz、未热降频；
这是当时的控制器采样，不是实测RPM或此前峰值温度。

### .59窗口对照与修正

保持原启动参数和隐藏PowerShell父子进程，在`.59`运行同一窗口脚本：
ManagedVisible=true，但IsWindowVisible=false、Iconic=false、Cloaked=0。
它能报告13次移动/缩放并退出0，实际仍未显示，故这些软件计数不能作开窗通过依据。
修正后在Shown回调显式ShowWindow(SW_SHOW)，再检查Win32可见状态并激活自有窗口。
08:26的对照NativeVisible=true、Foreground等于该窗口句柄，90.13秒内17次
移动/缩放并退出0；用户确认“能看到，显示和操作正常”。这是工具在`.59`的
对照通过，不是tgd3d原生窗口通过，也没有修复前两次硬重启。

依据微软[ShowWindow](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-showwindow)
（2023-06-06），父进程STARTUPINFO可以覆盖第一次显示命令；后续显式显示才
使用请求状态。[IsWindowVisible](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-iswindowvisible)
（2021-10-13）检查WS_VISIBLE，但仍不能证明未被遮挡或面板正确，因此保留实屏反馈。

另外将UMD独立为64KiB、4至16个缓冲、1秒刷新会话，原内核会话继续保留；
identity读取跳过无关的调用阶段JSON对象构造，原始日志全部保留。回退通过
持有并核对进程对象来停止collector，已退出视为清理成功，真实失败仍报错。
08:31在`.59`通过3项实际进程控制、18项身份读取控制、14条实时/离线合成ETW
传输和脚本语法检查，无GPU工作。原生显示、垂直同步和重启修复仍待下一轮。

本轮证据196文件、130470854字节均已按清单长度/SHA256取回校验。归档20886011
字节，SHA256`0c8d6289eda282f3a6a9de204edf8a8bac100ae4925bee4b0102105ddc447ef7`，
位于独立产物目录`hardware/staged-d6470feb/`。其中原工具、修正后`.59`对照、
两次回退记录分别保留，未覆盖原始失败记录。

## 第四轮：打开快捷设置后冻结，自动回退正常执行

组件仍为`d6470feb38defeb824f34cda39ffbb9e886f611d` UMD、冻结`.158` KMD
和`7c677212` ICD，三个二进制SHA256与第三轮一致；只更新窗口、独立ETW和
collector清理工具至`04034cfd7e6f853070f2d91152dab6c4bce9d439`。包6723706字节，
SHA256 `27ffeb9707a78a248583e4dca702a47c4a2575ea1bd89415b6bd18c7a5b125ed`。
目录为`tgd3d-d6470feb-20261113-164031`；WMI启动时间
08:42:51.647048 UTC，08:45:04原生入口确认`.158 / Code0`。
LUID`00000000000996bd`，DWM PID468/TID1840，创建FILETIME
`134390330044625805`。

静止阶段08:45:22开始，0/15/30秒身份检查通过，但08:46:00的截图调用在15秒后
超时，整段48.39秒未通过。用户报告点开右下角Wi-Fi/快捷设置时画面冻结，并指出
首次原生输出时曾正常打开此面板；具体触摸时刻未单独记录。因此这轮包含交互，
不能标成纯静止桌面偶发冻结，也不能仅凭一次触发判定稳定复现。没有请求Window
阶段，没有新的彩色窗口、五分钟交互或性能程序回归通过结果。

冻结时08:46:29 SSH仍能执行hostname，实时UMD日志在恢复前取回。完整独立
UMD ETL正常结束，无丢事件/缓冲，2193条载荷与实时日志逐条相同。DWM保留15次
成功提交和Present，最后完成值14；80对设备阶段、336对编译阶段、198对提交
阶段进入/返回数量相符。26条错误均为DONOTWAIT Map的`0x887b0001`，没有保留
CPU等待开始/结束记录。DWM不再产生日志后，其他进程仍继续；这些结果不能证明
最后一帧已扫描完成，也不能证明GPU全局挂起或供电故障。

最后的DWM资源记录为1280×13、bind0x28。审查发现旧事件8在CreateResource
**入口**便写S_OK，尚未执行分配和绑定；不能把它解释为资源创建已经成功返回。
内核记录最后的paging fence7313，在另一条System工作线程上有相同对象的
UnwaitCpuWaiter，间隔0.2005ms，随后paging packet312结束。因此不能把这条
主线程等待当作已证实的Fence超时。内核/UMD墙钟偏移仍需保留各自时钟，不能
直接跨ETL按FILETIME减法推断调用顺序。下一步需要DWM用户栈与分阶段创建记录。

08:46:41恢复脚本完成`.59` staging，System1074 **Record40865**确认SYSTEM
通过shutdown.exe发起计划重启。08:47:11手动恢复SSH请求未连接成功，未送达；
不能把此次重启记作又一次无预兆硬复位。无新的41/6008。08:48:48恢复校验通过，
`.59 / Code0`、原ACPI/显示和平台设置保持，用户确认“已回到桌面，能操作”。

证据157文件、108256188字节全部按长度和SHA256取回核验；归档11813391字节，
SHA256 `1094b801e67e2722f242d3a1c33f4587fada164b732e94b1d9c472d3d675fd66`。
位于独立产物`hardware/staged-04034cfd/`，物理反馈、后补系统事件和分析分别
保存。原始内核228480条记录中，按保留的本机模板解码170178条DxgKrnl记录，
无该provider的未知/损坏载荷。Wi-Fi触发及偶发性目前都是待验证条件，不是根因。

### 冻结调用栈取证

新工具使用PSS快照和系统DbgHelp，只写线程上下文、栈及模块信息，不写完整堆。
通过PID、创建FILETIME和镜像名核对同一DWM，先保留进程句柄；错身份不抓取。
写入器自身限制256MiB提交内存、20秒取消/25秒退出和32MiB输出，成功刷新后
才从`.pending`发布为`.dmp`。捕获失败记录原因并继续恢复，不改变保护策略。
恢复开始时先保存KMD Parameters原始文本，再抓DWM；卸载后才读注册表会丢失
本轮状态。每轮已有取证收据则不覆盖或重抓。

09:17:56 UTC在`.59`完成初步控制：错创建时间被拒绝；自建目标成功捕获后正常
退出0；实际DWM PID560的快照455856字节，捕获468ms，写入器峰值提交1941504
字节。系统DbgEng可展开19个线程；没有PDB的内部名称仍只能看成导出名加偏移。
这只验证取证能力，不是冻结时栈，更不是重启修复通过。

实现依据Windows SDK `10.0.26100.0`的minidump头文件及微软
[MiniDumpWriteDump](https://learn.microsoft.com/en-us/windows/win32/api/minidumpapiset/nf-minidumpapiset-minidumpwritedump)
（2024-02-21）、[PSS快照导出](https://learn.microsoft.com/en-us/previous-versions/windows/desktop/proc_snap/export-a-process-snapshot-to-a-file)
（2018-05-31）和[PssCaptureSnapshot](https://learn.microsoft.com/en-us/windows/win32/api/processsnapshot/nf-processsnapshot-psscapturesnapshot)
（2025-10-31），均于2026-11-13查阅。适用本机Windows 11 ARM64；采用示例的
snapshot callback契约，但省去handle表和全内存，只保留本次DWM停顿定位所需数据。
不依赖旧WATCHDOG dump，不保证掉电或完全内核锁死时还能执行用户态取证。

09:24:08 UTC，正式构建产物在`.59 / Code0`通过12项CPU合约、18项读取判定、
16条真实ETW实时/离线传输和全部脚本语法检查。取证写入器的错身份拒绝、目标
正常退出、恢复入口抓DWM以及已有收据不覆盖均通过。该入口本次生成465088
字节DWM快照，312ms，写入器峰值提交1908736字节；`.59`没有原生KMD Parameters，
注册表读取退出1如实保留。没有安装候选或执行GPU验收。

新UMD SHA256 `58a26cd6c8f2d6d17c13d5e9b90bb13b64759ee34eab59b73de11e8e74ae4af9`，
写入器 `50be687f0df6a9ba8876974e0ed963ab67d978f39c420faa5b63c5ebef604bb5`。
本轮只补资源调用进入/返回和失败取证；绘制/同步算法、KMD与ICD未变，冻结根因
尚未定位。控制证据归档1644793字节，SHA256
`73a79b1ee360c3fd1ef37736822b4ed7de14d8ee2bce42051dd077bbf689ac7a`，位于
独立产物`resource-phase-controls/`。后续原生试验仍须物理观察及失败即回退。

### 末次渲染完成与冻结后的VBlank

继续按内核对象和提交序号配对：DWM的NVK context `0xffff9906adb0cdd0`
提交36对应DMA751，有177/176完成和180队列结束记录，未超时/抢占；随后同一
context提交37的时间线15 signal结束。runtime context `0xffff9906ae3addd0`
等待同一fence对象`0xffffd3040d4ee820`的值15（提交61）亦结束，Flip提交62及
后续信号63/64/65均结束。故UMD末条Present中completed=14只是当时采样，
不能据此认定序号15永远未完成。冻结`.158`源码`TgGpuSignalContextWork`在
通知DMA完成前检查GPU marker、GPU/CPU完成及退休状态；本次仍只把ETW作为
内核完成报告，不据此替代像素或物理桌面验收。

通过同一内核记录的FrameQPCTime和UMD原始QPC作近似关联，可将末次纹理入口
与后续驻留/两次GPU VA映射放在同一段时间，而不直接混用两份ETL的FILETIME。
这一对齐存在通知延迟，不能当成微秒精度的函数进入/返回。paging fence7313
的0.2005ms仍来自同一内核时钟的wait/unwait配对，与跨会话校准无关。

该原生适配器在末次资源入口后另有4396条VSyncDPC，跨度73.25秒；周期中位数
16.66651ms，范围16.64594至16.68682ms，报告的扫描地址始终为4223320064。
冻结源码`panel_interrupt`仅在Tegra DC的VBlank状态位有效时发通知，先读取
锁存状态，再报告Displayed.Address。因此证据表明显示中断仍在运行、上报地址
未变化；不能把“约60Hz中断”写成画面持续更新、无撕裂或完整VSync通过。
没有据此改KMD、提高CPU频率或加入CPU等待替代GPU Fence。

回调边界再次核对微软[SetVidPnSourceAddress](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/nc-d3dkmddi-dxgkddi_setvidpnsourceaddress)
（2022-11-04）及[DxgkCbNotifyInterrupt](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/nc-d3dkmddi-dxgkcb_notify_interrupt)
（2021-10-27），2026-11-13查阅；原始配对和时钟样本保留在
`hardware/staged-04034cfd/analysis/last-render-completion.json`和相邻分析文件。

下一轮冻结包源码为`94cc811acb056d0fbe9374c510a55c0dbe389be3`，6937619字节，
SHA256 `1b6f96ae70b92148dacb31fcf2052da7ef64027237154677580349140529cd16`。
正式UMD SHA256 `2f908f4e0f11bae47f093cf3c0ece7bf86cd94ce829d6349e219def490c0b1cf`；
复构建使PE时间戳变化，CPU合约与取证程序已逐字节确认只有PE/Debug目录时间戳
不同，代码和数据相同，详见`resource-phase-umd-arm64/control-equivalence.json`。
设备只完成备份/准备，尚未启动这轮原生测试；仍需新的物理观察回复。

## 第五轮结果与源码对照方向

上述`94cc811a`包随后实际运行：同一冻结`.158`和`7c677212` ICD，DWM PID464、
LUID`0000000000096c84`。09:55:23至09:56:06 UTC静止阶段完成，用户确认画面正常、
能操作。09:58:54设备记录Window请求；用户随后报告“没有出现就重启”。本轮失败，
不能记开窗通过。10:03:56 UTC系统恢复校验确认`.59 / Code0`和平台设置保持，用户
确认桌面正常、能操作。此前已取回的174文件归档SHA256为
`9f790c55b1abae6ef1ded72429d3378c38195307e1ebf94b19b342c96be7cee8`。
保留的UMD/内核末尾早于开窗请求约51/53秒，不能据此定位故障瞬间。

用户要求停止继续查新日志/Dump，改为直接对比源码、一次改变一个变量并上机。
本轮不再扩展取证工具；恢复脚本不再先抓DWM Dump。

2026-11-13源码对比以实际历史交付为基准：Zink UMD来源
`833c4423ec6ed668a257d3896fabf26ff5e9f1ad`、ICD来源
`8c361754f3059cec0ecacb5543e25adf7eba51b2`；tgd3d审查基线`2e5d1313`、ICD
`7c677212e5843725f3dbb12ee0b9dad6b1339a05`，KMD仍冻结于
`6c854e9f394c3d761fab85885bead4bbd4588581`。没有合入main或改KMD。

| 路径 | Zink桌面UMD | tgd3d审查基线 | 结论范围 |
|---|---|---|---|
| 普通Flush/Present | `RuntimeDeviceFinish`等待Gallium fence完成 | runtime signal → Vulkan wait/render/signal → runtime wait | 有明确同步差异；未从源码证明环路 |
| 多设备 | Zink按物理设备复用VkDevice及queue lock | 每个D3D Device独立创建VkDevice及KMT上下文 | 新设备/上下文压力不同；未发现已证实的句柄越界 |
| 共享布局 | 72字节元数据、128字节pitch、4行补齐 | 同一格式/大小协议，另核对Vulkan rowPitch/offset/requirements | 未发现明确布局越界；不能代替已知像素验收 |
| 编译 | SHDR→TGSI→NIR→Zink SPIR-V→NVK/NAK | 重建DXBC→dxbc-spirv→NVK/NAK，shader objects | NAK源码与目标SM53相同；输入/绑定/动态状态不同，不能排除特定输入的编译缺陷 |

核对的微软契约包括[DDI线程规则](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/changes-from-direct3d-10)、
[GPU wait callback](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dumddi/nc-d3dumddi-pfnd3dddi_waitforsynchronizationobjectfromgpucb)
与[signal callback](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dumddi/nc-d3dumddi-pfnd3dddi_signalsynchronizationobjectfromgpucb)
（后两者2024-02-22）、[共享资源创建](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/nc-d3d10umddi-pfnd3d10ddi_createresource)、
[像素着色器创建](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/nc-d3d10umddi-pfnd3d10ddi_createpixelshader)
（2021-04-01）。tgd3d只协商D3D10.0 DDI且不声明free-threading；不能套用已启用
D3D11 free-threading的宽松条件。此KMD声明WDDM2.6；其monitored fence等待由
调度器控制，不能当成[WDDM3.2 native GPU fence](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/native-gpu-fence-objects)。

静态审查另发现两个失败路径问题，尚未修改或宣判为本轮根因：
`wait_cpu`到期仅设置tgd3d局部lost，资源/设备销毁随后调用无期限`DeviceWaitIdle`；
`create_shader`在失败时没有析构已经保存的tokens/signatures，而微软明确创建报错
后不会调用DestroyShader。前者需要保持在途对象安全，不能简单删除等待后释放；
后者属于失败清理问题，不能据此声称解释了成功提交时的硬件重启。

### 仅关闭跨队列桥接的临时对照

提交`d06e29e223b9ba22e85b58bf49eaca6fa457ec31`提供显式构建参数
`build_tgd3d.py --cpu-sync-control`，默认构建仍保留GPU桥接。对照版不创建两个
runtime monitored fence、不导入Vulkan，也不排runtime↔NVK的GPU wait/signal。
本地Vulkan完成时间线和相邻提交依赖保留；普通Flush/Present/共享释放改为CPU
等待真实完成，仿照旧桌面UMD。DONOTWAIT/Query轮询仍只提交、不做CPU等待。
这是诊断构建，不是永久退回同步渲染或宣布同步问题已修复。

ARM64/x64构建完成；x64 Wine下12项CPU控制通过，包括原生标志下不创建/导入
跨上下文fence、普通Flush完成、轮询不阻塞和超时不伪造完成。三份实际转换的
VS/PS/采样SPIR-V通过`spirv-val --target-env vulkan1.3`；这不验证SM53机器码执行。
可还原包6936555字节，SHA256
`603a778caea98ca15cbfe37b9895f8ffc961f18852a1841623ccb40119704152`。
仅UMD同步方式改变，KMD/ICD仍为上述冻结文件。实机结果另记，不由控制测试推断。

`.59`上ARM64同一控制程序亦通过12项，全部PowerShell脚本解析通过；已准备
`C:\ProgramData\SwitchWOA\GpuUpdate\tgd3d-d06e29e2-20261113-185158`，备份和恢复
校验完成，尚未安装启动。UMD SHA256
`b3ba7de9bb07a5627ddf907fd7e6a07d1a2fe33dcab977d06f41ba0adb450ffc`。
Antigravity CLI的Carbon High独立源码审计已启动；其结论须经本窗口复核。

第一次同步对照在开窗前被UMD消费者状态校验拦下，自动恢复；不能记为同步模式
开窗失败。11:01:35 UTC恢复校验通过`.59`。源码确认消费者无事件时不刷新状态，
而身份读取器强制要求10秒内状态；`a877d211`仅对显式CPU对照允许静止状态，仍
要求同一DWM创建/Present及LUID一致、无丢事件。22项读取控制通过；GPU二进制
不变。该工具修正后的包已准备但尚未启动时，用户进一步修改恢复策略。

**最新用户指令：以后直接以冻结`.158`为基线，不再自动退`.59`。** 新恢复动作
保留`.158`及匹配ACPI，撤销临时tgd3d/ICD注册和启动许可；重启后使用关闭门控的
`.158 / Code43`及BasicDisplay可操作桌面，再核对原显示、CPU1497600及风扇13。
`.59`只保留归档备份。历史记录中的`.59`回退要求不再适用于新试验。

### 同步对照开窗结果及进一步的用户纠正

工具`4f3b7a2e`、UMD`d06e29e2`的CPU同步对照于11:13:55 UTC启动，KMD、ICD及
UMD二进制均保持上述冻结哈希。11:17:13–11:17:57的静止阶段约39.12秒，
11:19:26–11:20:44的Window阶段约72.54秒，均通过API检查，同一原生LUID
`000000000008c1a6`。窗口实际存活45.22秒、移动8次、缩放8次，退出0；用户确认
“现在窗口出来了”。未确认全部物理运动/像素，也没有五分钟稳定性通过。

该对照同时移除了runtime↔NVK桥接并使普通Flush等待真实GPU完成，因此只能
缩小到同步/并发及其影响的资源生命周期，不能单凭一次结果宣判特定Fence死锁。
默认关闭桥接的临时编辑已按用户纠正撤回；正式构建仍默认启用GPU桥接，
`--cpu-sync-control`只用于诊断。下一版需要针对源码中的具体缺陷修复。

主会话在Window结束后错误地继续按试验流程触发恢复重启。11:24:28保留`.158`
及匹配ACPI，11:26:36校验`.158 / Code43`、显示和平台设置保持；这不是继续运行
原生桌面。用户明确纠正：**留在当前`.158`开发，不要测试结束就回退/重启。**
该指令取代上一段“自动关闭门控再重启”的解释。源码已删除自动恢复定时任务和
阶段错误时自动恢复，保留显式手动恢复工具。已确认设备上旧Restore/Automatic
任务不存在，且没有待执行关机；没有再次发出重启。测试结果及用户原话保存在
独立产物`hardware/cpu-sync-d06e29e2-idle/`。

Carbon High的tgd3d审计和DXVK后续任务按用户要求停止，均返回interrupted；
审计尚未给出最终报告。输出和DXVK三个未提交文件的补丁已保存在
`gemini-takeover-20261113/`，不作为已验收结论合入。随后用户要求恢复审计，
11:30:37 UTC通过Antigravity CLI恢复同一Carbon High会话，补充上述对照结果，
保持只读并要求给出具体缺陷及最小修复；DXVK后续任务仍停止。

### 统一对照条件，复用当前.158启动

进一步核对本轮已完成的身份结果：Static-30的TraceState间隔26.103秒，
Window-0为3.404秒，Window-60为11.152秒。旧检查对GPU桥接模式仍强制10秒，
但CPU对照允许静止，因此工具条件不一致；不能把这轮差异全部归因于Fence。
`Assert-NativeDesktop`现在对两种提交模式都接受同一当前进程/适配器的静止身份，
仍校验LUID、进程创建时间、消费者存活、成功创建/Present及事件损失。此身份
检查不证明画面持续更新，窗口动作和物理反馈仍需分别验收。

冻结KMD的`start_device`每次调用`TgGpuConsumeNativeStartPermit`，并非只在
DriverEntry读取许可。对已确认`.158 / Code43`、SYS/ACPI匹配的当前启动，
测试入口改为直接观察、授予一次许可并PnP重启显卡设备；不重装KMD、不重启
Windows、不改CPU/风扇。原生已经启用时仍直接继续阶段，不再次启用。
11:45:58 UTC，在实机`.158 / Code43`解析26份脚本并通过22项读取控制，尚未
启用本次GPU对照。GPU桥接默认、KMD/ICD及渲染代码均未改变。

### 手动测试与默认启用包请求

随后GPU桥接版在当前Windows启动内启用，11:53:07–11:53:50静止API检查通过。
开窗阶段先因SSH会话退出导致UMD消费者子进程消失而被工具阻断；这是工具失败，
不能记作GPU失败。另行启动的自有GDI窗口在11:57:49记录为可见、已移动/缩放
6次、运行30秒；没有本轮物理屏幕确认，也不能据此记为稳定性通过。后来读取到
`.158 / Code43`，未因该状态重新搜索系统日志或宣判重启根因。

用户明确要求先屏蔽未修复的双向Fence，自行手动测试。12:14:16启用CPU同步
UMD `d06e29e2`（SHA256 `b3ba7de9bb07a5627ddf907fd7e6a07d1a2fe33dcab977d06f41ba0adb450ffc`），
返回`.158 / Code0`、Hardware/Panel为0；自动任务清空，不重启Windows。后续
DWM模块查询SSH超时，没有把该次模块加载证明补成成功。
用户报告左下角天气的大面积弹层在出现前或出现一半时也会冻结重启，本轮失败。
不能再把关闭桥接视为稳定修复，也不能在没有补丁时声称Fence已修好。

用户接着要求默认启用包并自行重启。新增入口仅复用签名`.158`、现有CPU同步
UMD和`7c677212` ICD，在登录后自动授予一次启动许可并启用设备；不修改KMD、
CPU/风扇/睡眠设置，不自动开窗测试或回退`.59`。启用过程未完成时下次停止重复
尝试，手动还原仍保留`.158`。该包只是启动/交付方式变化，渲染根因修复尚未完成。
Carbon High同一审计会话的报告输出最终超时，只有不完整开头，没有可验收结论。

### 默认启动的实际启用与着色器失败清理

默认包于12:30安装、12:33完成文件/任务配置核对，尚未运行启动任务。随后该包
的恢复入口于12:44留下restore-staged记录、删除默认任务并恢复门控。用户报告
开机未进入tgd3d后，12:49读取到`.158 / Code43`、任务不存在、default-state缺失；
没有把它算成原生启动失败或再次调查GPU转储。12:50重新登记现有包的登录任务并
执行一次，12:51:26状态Enabled；12:51:45核对Code0、Hardware/Panel均0、任务退出0，
DWM PID484、Explorer、开始菜单、TextInputHost及ShellExperienceHost均加载
`TegraGpuD3DVk.dll`。仍是冻结`.158`、`d06e29e2` CPU同步UMD和`7c677212` ICD，
没有重装KMD或重启Windows。本次证明实际模块加载和启用，不是物理稳定性通过。
`RESTORE-TGD3D.cmd`会关闭自动启用；正常重启应使用Windows的重启操作。

用户再次授权Antigravity CLI继续审计，12:40:22以Carbon High恢复原会话，加入
CPU同步下天气弹层仍冻结的新事实，只读审计保持进行中，初步意见尚需主代理核实。

本轮源码修复限于`create_shader`失败的对象生命周期。微软
[CreateVertexShader](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/nc-d3d10umddi-pfnd3d10ddi_createvertexshader)
（页面2024-02-22；Vista及后续D3D10 DDI）及
[CreatePixelShader](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/nc-d3d10umddi-pfnd3d10ddi_createpixelshader)
规定：创建上报失败后，runtime不会调用DestroyShader。原实现返回前遗漏Shader析构，
token/输入签名/输出签名的vector分配因此泄漏。现在先析构，再调用SetErrorCb；
直接拒绝stream-output创建时不再构造无主对象。暂不改变shader错误码或双向Fence。

12:54:13完成原生ARM64 **CPU控制**：同一新增失败测试链接HEAD `838d775d`的旧shader
实现时退出1；链接修正后退出0，VS、PS和采样PS在模拟backend内存不足时，其三份
真实堆分配均在SetErrorCb之前释放，没有调用DestroyShader。其余现有控制通过。
ARM64/x64构建通过，产物及对照保存在`shader-create-cleanup-controls/`。
三份控制着色器的SPIR-V通过`spirv-val --target-env vulkan1.3`。
没有执行NVK/NAK或GPU，也未将修正部署到手动测试设备；不能归为天气重启根因已修复。

### 网页关闭反馈与 SM53 编译控制

用户补充：关闭网页也可能直接冻结，面积大的画面变化更容易触发。按失败记录保留，
不能据此在着色器执行、大纹理/共享表面释放与复用之间选定根因。

新增手动 CPU 编译检查复用生产 `nvk_device_shader_ops`、普通描述符布局和 GM20B
参数：SM4 DDI → dxbc-spirv → SPIR-V → NVK lowering → NAK SM53。没有创建
KMT/Vulkan 硬件设备、分配 GPU 内存、上传 shader 或提交 GPU 工作。原始三个 fixture
之外增加由 CB13 控制循环次数、动态读取权重、采样和 clip 的像素着色器；其 SPIR-V
实际含 OpLoopMerge、动态 OpAccessChain 和 OpDemoteToHelperInvocation。

ARM64 Windows CPU 前端控制通过；四份 SPIR-V 均通过 Vulkan 1.3 目标校验；后端分别
生成64/160/704/12000字节 SM53代码（6/13/63/1027条指令）。检查工具早期入口崩溃
已定位为静态链接没有拉入 weak Vulkan entrypoint，改用 `link_whole` 修复；不是
着色器编译失败，也不是 os_get_option/TLS 缺陷。控制输出在独立产物目录
`nvk-shader-controls-arm64/`，保留 SPIR-V、NAK 文本与机器码。

静态对照排除了两个直接套用的修复理由：Zink 与 tgd3d 都按微软 DXGI 契约保留
callback 表指针；冻结 KMD `6c854e9f` 已在 golden context 保存前清除
`0x419f78` 的 NO_HELPER_LOADS 位，并检查读回。因此不因 Horizon 片元全局加载问题
直接撤回普通 CB 描述符，也没有修改 KMD。此处只确认源码配置，未新增寄存器实测。

实际桌面 shader、描述符地址、GPU 执行和关闭窗口资源退休仍未验收。`68c1bb7e`
失败创建清理修复已经推送；本轮没有将它部署到用户当前手动测试的 UMD，也没有
修改双向 Fence、重启 Windows 或回退 `.59`。

### 设备丢失的 DDI 错误传播修复

WDK 10.0.26100.0 中 `D3DDDIERR_DEVICEREMOVED` 为 `0x88760870`，与
`DXGI_ERROR_DEVICE_REMOVED` (`0x887a0005`) 不同。原 `Device::setError` 把后者
直接交给 SetErrorCb；shader backend 的 `VK_ERROR_DEVICE_LOST` 则被转换为
`E_FAIL`，未锁存设备丢失。微软
[Handling Errors](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/handling-errors)
（2021-12-15）、
[Draw](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/nc-d3d10umddi-pfnd3d10ddi_draw)
（2024-02-22）与
[CreatePixelShader](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/nc-d3d10umddi-pfnd3d10ddi_createpixelshader)
（2021-04-01）规定当前 DDI 允许的错误码，其他错误会被 runtime 作为严重错误处理。

修复在 callback 边界转换 removed/reset，保留内部状态和原始错误记录；shader
创建将 Vulkan 的两类 OOM 映射为 E_OUTOFMEMORY，将 device lost 映射为 DDI
device removed。未知错误仍上报，不伪装成功。设备丢失后拒绝后续创建/变体编译和
缓存变体使用，没有改同步拓扑或伪造 Fence 完成。

同一新增控制链接旧源码 `6decd617` 时在 timeout callback 断言退出1，修复后退出0。
15:08:59 UTC 的最终 ARM64 CPU 控制覆盖 removed/reset/DDI 原值、两类 shader OOM、
未知错误、shader 延迟变体触发 device lost、失败清理以及丢失后停止编译/提交；
ARM64 和 x64 构建通过。产物与原始输出在 `ddi-device-loss-controls/`。
这些是模拟 Vulkan 故障的生产 DDI 控制，不是实际发生 GPU 丢失或桌面稳定性通过。
当前运行 UMD 未替换，天气/网页关闭冻结根因和 teardown 无限等待仍未解决。

并行审计：原 print 客户端未返回完整报告，但随后通过 Antigravity CLI 的
ACP session/load 读回了会话内已经完成的报告，最后的工具调用也为 completed。
报告原文保存为 `delegation-tgd3d-carbon/recovered-audit.txt`。验收发现报告遗漏冻结
KMD 的 helper-load 配置，并把 DeviceWaitIdle 返回 DEVICE_LOST 后的合法清理误判为
在途释放；把每设备独立 VkDevice 的开销称为契约违例，以及提交失败后必定复用
序号的说法，也缺乏当前代码支持。未采纳其改动方案。

关闭已完成报告的卡住客户端后，15:13:36 UTC 使用同一原会话和 Carbon High
继续定向复核上述矛盾；已确认开始执行读取冻结 KMD/最新源码。VPS Claude 的
只读请求返回未登录，未产生审计结论，未更改其配置。

`fcc53bf1` ARM64 CPU-sync 构建也通过生产 DDI 控制。已打包
`tgd3d-fcc53bf1-default158-cpu.zip`，6910291字节，SHA256
`8b46cd1de2dfb06d7cac0ab9f9df920b57f471b3d561d2c17b0b65324eafcd70`；
51个文件通过归档逐项校验。继续使用原KMD/ICD及默认登录启动/手动恢复工具，
未安装或激活这个候选包，不能记为天气冻结已修复。

### fcc53bf1 同步对照：五分钟桌面交互通过

上述包随后于15:20:27–15:20:30 UTC更新当前原生注册并执行一次显卡PnP重启。
没有重新安装KMD或重启Windows。冻结组件为：

| 组件 | 源码与模式 | SHA256 |
|---|---|---|
| KMD `.158` | `6c854e9f394c3d761fab85885bead4bbd4588581` | `46d1757674ff156028ce5b729ba53671981ffab09948fcaf125751c46bb10088` |
| tgd3d UMD | `fcc53bf14c001881fb8c84ea61e9d4f3ebfc04b7`，CPU-sync | `7c7f88ccf83ad1a89f663c3205e6ba2cb6fec587d946efd3622839c9deb2b089` |
| NVK ICD | `7c677212e5843725f3dbb12ee0b9dad6b1339a05` | `17dc3110393365eef279a90104850d6f517c74659441916266afeae0ae2c8f73` |

Windows11 ARM64 23H2 22631.2861，启动时间15:12:45.5399650 UTC，原生LUID
`00000000000d0fa1`。DWM PID504实际加载新目录
`tgd3d-fcc53bf1-default158-cpu\app\TegraGpuD3DVk.dll`，更新前后的系统启动未变。
原始SYS、ICD和ACPI均保持，CPU1497600kHz、风扇13及睡眠配置保持。

- 15:22:00–15:22:31静止观察，用户确认“画面正常，能操作”。
- 15:24:22–15:25:23彩色窗口运行60.221秒，移动11次、缩放11次后正常关闭；
  用户确认“一直正常，能操作”。
- 15:27:15–15:32:16五分钟窗口运行300.262秒，移动51次、缩放51次、最小化8次、
  恢复7次、遮挡7次，正常结束。开始菜单与快捷设置在同一阶段执行。
- CPU／风扇程序15:29:45启动、存活30.168秒、15:30:16正常关闭，退出0。
  用户对完整五分钟问题回答“都看到了，全程正常”。
- 用户随后补充“我刚试了几遍天气与浏览器都正常”。记录为同组合的手动回归反馈；
  未给每次操作编造精确次数、时间或浏览器负载细节。

因此本轮可标记五分钟原生桌面交互通过。仍是关闭跨队列桥接的CPU同步诊断构建，
不能把它写成双向Fence已修复，或宣判历史天气／网页关闭故障根因已消除。
没有已知像素读回、硬件横屏、VBlank时序或vkcube通过结果。

身份辅助工具的首次任务因命令行引用错误退出1，修正调用后读到同一LUID、
`matching_kmd=1`、`nvk_ready=1`。其退出4表示冻结stub没有可选私有导出，
不归类为GPU失败。末次状态读取一度使用了错误的PnP筛选ID，得到空对象；
改用实际`ACPI\SWOA0F01\0`后确认`.158 / Code0`及Hardware/Panel均0。
天气自动按键任务最初因电池运行条件被排队，尚未执行；修正该任务条件后执行一次。
这些工具问题不冒充渲染失败或实屏通过。必要结果和用户反馈保存在
独立产物`hardware/ddi-device-loss/`，没有扩展GPU日志或Dump收集。

### Carbon修订审计与独立KMD候选

同一Antigravity Carbon High会话已返回修订报告，承认此前helper-load、提交失败
序号复用、每设备VkDevice违约及DEVICE_LOST返回后清理等结论不成立。CLI最终外壳
返回stream-interrupted/error，但其result.response中有完整修订报告，已保存为
`delegation-tgd3d-carbon/review-revision.txt`；进程已结束，不再记为后台审计在途。
等待超时后转入无期限DeviceWaitIdle的问题仍未解决，不能删掉等待后直接释放在途对象。

报告指出NVK shader-object路径所需的`0x419e44` bit14配置在Windows被跳过。
主代理独立核对固定Mesa源码、冻结KMD与真实NETB：固件初值`0x00d3eff2`确实置位，
而`.158`只补了helper-load的另一个寄存器。新增最小KMD补丁在golden save前清除
这一位并读回，保留其他异常位；未照搬旧Fable `9092e512`清零两个完整mask的方案。
旧源码在新模型断言失败，修复后通过，ARM64内核源文件编译通过。
详见[来源、适用条件及验证限制](GR_HELPER_LOADS.md#november-13-shader-object-output-exception-policy)。
此补丁未生成新签名驱动、未部署；本轮实屏成功完全属于原冻结KMD组合。

### 后续“主文件夹”故障：整体稳定性仍未通过

15:39:40 UTC收尾核对仍为同一启动、DWM PID504、新UMD路径及`.158 / Code0`，
Hardware/Panel均0；CPU1497600kHz、风扇13保持，默认登录启用任务已恢复，临时
测试任务已清理。随后用户报告文件夹卡住、桌面刷新关闭文件夹，再次尝试仍卡住。
进一步明确：“此电脑”正常，顶部“主文件夹／Home”卡住后导致Explorer重启。
此前五分钟实屏通过只保留为该时间段结果，本组合整体稳定性记为失败，停止扩大
已知像素、旋转或vkcube验收。

按用户最新要求通过SSH查看内存和应用异常。15:43:57可用内存1852MiB、提交量
1833299968/4508372992字节（40%），非分页池约64MiB。前后约20秒的Explorer私有
内存均93.1MiB，DWM约96.7MiB；没有快速耗尽证据，短采样不能排除慢性泄漏。
DWM仍为PID504；Explorer于15:42:37及15:44:38被Winlogon自动重启。

Application Error分别在15:41:47、15:43:50记录两个Explorer实例的同位置崩溃：
模块为当前包内`libvulkan_nouveau.dll`，异常`0xc0000005`，RVA`0xb542c`。
匹配SHA256和PE时间戳后，原二进制符号/反汇编定位到
`nvk_cmd_flush_gfx_shaders+0x358`，指令`ldrh w23,[x22,#0x4f8]`，对应
`last_vtgm->push_dw_count`。这是CPU侧访问违规，不是本轮新增GPU看门狗转储。
SSH会话0的窗口枚举为空，不能据此说用户会话没有文件夹窗口。

静态调用链发现独立缺陷：`nvk_meta_end_gfx`恢复NVK shader指针，却未恢复公共
runtime的`pipeline_shader_stages`。内部图像复制／blit绑定临时pipeline后，
tgd3d下一次仅换PS时，`vk_common_CmdBindShadersEXT`会按残留标记解绑整个图形
pipeline，再只绑定新PS，使VS丢失；下一次draw会到达上述空`last_vtgm`路径。
修复同时保存／恢复graphics与compute meta的公共标记，保留原有pipeline和混合
绑定，不增加每次draw强制重绑，不改`.158` KMD或双向Fence。

新增ARM64 CPU控制直接调用生产meta保存／恢复和公共shader绑定代码，临时meta
pipeline的绑定效果由测试提供。旧实现会在PS-only更新后丢失VS，修复后保留VS，
并保持graphics/compute原有标记。此控制不创建KMT设备或执行GPU工作；Explorer
真实回归仍须用新ICD验证。初次控制的模拟设备缺少零页VA，测试自身失败；补齐
模拟状态后才取得上述结果，未把它当成驱动或GPU失败。

### 单独ICD修复候选与启动阶段失败

`d3423d269f9e716eadc1237beadbfb6aa6049dbe`已提交并推送。最终ARM64 CPU控制旧版
退出1（PS-only后VS丢失），修复版退出0。新的生产ICD复用冻结版KMT目标文件，
除NVK meta的12行状态保存／恢复及构建身份外，没有带入其他GPU生产源码变化。
新ICD SHA256：`dee3d5805af80696f87d094ec1c5f557d662f435336207992baf588e2dc1cee0`。

默认启用／手动恢复包`tgd3d-meta-d3423d26-158-cpu.zip`为6909633字节、51个文件，
SHA256 `184467196cd2fc0705564a59003764c9e20ac16876847c8c53b469f643e9e125`。
与上一包逐项比较，唯一改变的app文件为`libvulkan_nouveau.dll`；原`.158` KMD、
`fcc53bf1` CPU同步UMD、ICD辅助工具和ACPI均逐字节相同。bit14的独立KMD源码补丁
`253bc813`没有进入这个包。16:03:47 UTC远端包校验通过，当时仍为旧目录、Code0。

随后只登记并启动一次`Tgd3dUpdateMetaD3423d26`，复用前轮恢复快照并请求显卡PnP
重新初始化，没有请求Windows重启。用户接着报告“自动重启了”；读取新ICD加载
状态的SSH连接超时，尚未取得新ICD实际加载或静止阶段结果。此轮立即记为失败，
停止后续开窗与Home测试，不重新触发同一候选。需要待连接恢复，只核对update标记、
启动时间和当前驱动状态，区分切换中复位与新组合运行失败；不能把它写成Home
补丁已通过实机，也不能凭重启就断言补丁本身是根因。

16:07:40 SSH恢复，确认系统启动时间已变为16:05:59 UTC，GPU仍为`.158`但Code43。
用户确认已回到桌面、能操作。新目录的update-request/default-state文件有长度却全为
零字节，没有update-result；因此只能确定切换未完成，无法补出具体GPU执行阶段。
当前注册指向新目录，但Code43说明它不是已验收的原生运行状态。

16:08:45核对候选包全部哈希通过，恢复清单可解析，但新复制的rollback文件中
`fecs_sig.bin`、`fecs.bin`、`pmu_sig.bin`、`gpccs.bin`不匹配。上一包完整恢复快照
逐项校验仍通过；16:10:17仅重写上述四个备份文件，使用WriteThrough/Flush(true)，
重新Check-Recovery通过。没有写System32驱动或再次启用显卡。
default-state改为真实的Failed记录，自动启用任务禁用，一次性更新任务清理。
CPU1497600kHz、风扇13、交流／直流关闭显示和睡眠均保持0。当前留在可操作的
门控`.158`桌面，没有退`.59`或再发重启命令。

这次暴露的恢复副本／切换标记落盘问题属于工具问题：新复制的文件与Set-Content
记录在强制复位前未保证落盘。已修复当前恢复副本；今后任何切换必须先显式flush
完整恢复集和请求标记，不能把读回缓存中的正确哈希等同于断电持久性。此问题
本身不解释为什么GPU设备重新初始化导致系统复位。

恢复流程源码现已增加`Save-Json`和`Sync-Recovery`：准备阶段持久化恢复清单并
flush全部恢复文件，默认启用在授予许可前再次核对／flush；损坏的default-state
JSON会先禁用任务再报告错误。16:13:47 UTC在门控`.158`上，三个脚本解析、真实
15文件恢复集flush和JSON往返控制通过，没有请求GPU启动。没有通过再次强制
复位来验证持久性，也没有替换当前失败候选包内的脚本或重新开启自动任务。


### 修复ICD从门控启动：Home通过，天气仍失败

16:36:23–16:36:57 UTC从原`.158 / Code43`的早期门控状态只启动一次，
保持Windows启动时间16:05:59不变。DWM PID492实际加载`d3423d26`目录的UMD与ICD，
Code0、Hardware/Panel为0。用户确认“可以了”和“主文件目录正常”。随后用户报告
“这次打开天气又重启了”；16:40:22读到Windows启动时间16:38:11、Code43，自动任务禁用。
这一轮仍是原SYS `46d175…`，未包含bit14，不得把天气失败或Home成功归到新KMD。

### `.158 + bit14 + 修复ICD`安装与直接原生启动配置

用户取消继续扩大测试，指定这个组合为后续测试基线，并要求自行进行重启和应用测试。
这里的“基线”是固定比较组合，稳定性尚未通过。全部GPU源码仍在`tgd3d-vulkan`，
没有合并或构建main。

KMD从冻结`6c854e9f394c3d761fab85885bead4bbd4588581`导出，只替换`253bc813e43a878359b9d6227e246cd7d1aa382f`
中的`platform/t214_gr_context.c`及对应控制文件。逐项比较导出确认只有这两处差异；
bit14修复清除`0x419e44`的这一位，真实NETB初值`0x00d3eff2`变为`0x00d3aff2`，
没有清空其他异常位。完整主机构建及ARM64内核构建通过，Windows ARM64 context、hardware、
worker替身控制退出0；这些不代表GPU执行。使用原开发证书签名，Windows SignTool验证CAT
及全部12个载荷均退出0。签名私钥未传到设备。

| 组件 | 来源 | 实际文件SHA256 |
|---|---|---|
| KMD `.158` bit14变体 | `6c854e9f` + `253bc813` | `2e4ae8081687537f777d72636ff1fab0640559215a703f6ccfab2127f95f0572` |
| tgd3d UMD，CPU同步 | `fcc53bf14c001881fb8c84ea61e9d4f3ebfc04b7` | `7c7f88ccf83ad1a89f663c3205e6ba2cb6fec587d946efd3622839c9deb2b089` |
| ARM64 NVK ICD | `d3423d269f9e716eadc1237beadbfb6aa6049dbe` | `dee3d5805af80696f87d094ec1c5f557d662f435336207992baf588e2dc1cee0` |

16:55:55–16:55:59在Code43早期门控状态安装新KMD，OEM从`oem92.inf`变为`oem93.inf`。
UpdateDriverForPlugAndPlayDevices返回成功、不需要Windows重启，SYS匹配上表；随后只发出
一次登录后原生启用。默认状态停在16:56:02的`Starting`，没有完整启用结果。SSH短时失联后
恢复，确认Windows实际重启于16:56:40，回到Code43；用户当时看到正常桌面。不得把该桌面
误报成新组合的原生稳定性通过，也没有再次执行相同登录后切换。

用户接着要求不再使用登录后从BasicDisplay切换，改为直接原生开机。启动工具提交`19a75fe8`
利用现有KMD的开机许可，不再改KMD/UMD/ICD：

- 安装入口创建并flush持久`AllowDesktopStart`注册表键，写入并flush4096字节的一次许可。
- KMD在StartDevice触碰硬件前消费许可，正常开机即可进入原生初始化；不依赖用户登录任务。
- `Tgd3dDefaultEnable`改为`MSFT_TaskBootTrigger`，运行`Native-Boot.ps1`。
  该脚本只检查已启动的Code0及Hardware/Panel状态，保持30秒后续写下一次开机许可；
  没有PnP重启、创建窗口、Windows重启或GPU提交操作。启动失败不续写许可。
- `RESTORE.cmd`关闭任务、删除持久键与许可。原生运行中先提示手动重启，之后才恢复原签名
  `.158`，不会热切换活动GPU，不安装`.59`。

直接启动包`tgd3d-158-bit14-d3423d26-native-boot.zip`为7073784字节、66个受清单校验文件，
SHA256 `a8bd3884f33355bc2265b7d8c7b90240e3332eb54da51e44727a84cd0bddcf22`。
17:04:23远端所有文件与PowerShell解析通过，15个恢复文件核对并flush。
17:06:02核对SYS哈希、持久键存在、许可值1、启动任务Ready/Enabled及原生启动脚本路径。
配置时仍为Code43，`native-boot-state`为`ArmedForNextBoot`，未发出Windows重启命令，
由用户自行重启测试。CPU1497600kHz、风扇13、交流／直流关闭显示和睡眠索引0保持。
目前没有直接开机Code0、实际加载模块或新组合五分钟的验收结论。

启动策略参考微软[注册表键持久性说明](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-zwcreatekey)
及[启动／登录任务触发器](https://learn.microsoft.com/en-us/powershell/module/scheduledtasks/new-scheduledtasktrigger?view=windowsserver2025-ps)，
核查日期2026-11-13。前者说明非易失键跨重启保留；后者区分AtStartup和AtLogOn。
实际Windows仍为11 ARM64 23H2，任务只是续写许可，不能用较新文档页面版本宣称该组合已上屏。


### 一次性许可方案失败，替换为KMD持久原生启动授权

用户17:09重启后报告仍为Code43。17:11:57 SSH确认：实际SYS为`2e4ae808…`、
OEM `oem93.inf`；BootPermitStatus=`0xc0000022`，StartGateStatus=`0xc00000a3`，
Hardware/Panel为`0xffffffff`。这一StartDevice在门控处被拒绝，不能称tgd3d已启用。
`Native-Boot.ps1`已记失败并删除许可、禁用任务。没有从这些状态推断具体访问被拒绝的
系统调用，也没有把一次性许可的拒绝当作GPU指令故障。该工具方案不足以承担持续直接启动。

`b916f53105f991294d6811718362e90f9af0ba01`在KMD中增加显式`AllowNativeBoot`键：
存在时允许重复StartDevice及正常重启，不消费文件；不存在时保留旧的单次测试门控。
非“不存在”类注册表错误仍返回错误，不隐瞒权限失败。ARM64控制测试在相同新断言下，
旧源码退出99，新源码退出0；覆盖重复原生启动无文件操作、移除授权重新门控、拒绝访问
不降级，以及原有文件错误与句柄关闭。完整主机模型/ARM64构建通过，未等同GPU通过。

新的KMD导出相对冻结`6c854e9f`只有bit14生产文件/控制文件，以及native_start_test
生产文件/控制文件这四处变化；没有带入main或其他KMD改动。初始新包与bit14-only包
生成了同一INF内容。17:19安装API返回成功、不要求重启，但SYS仍是`2e4ae808…`，
OEM仍93；Driver Store中没有新SYS，因此工具正确阻止了后续配置。随后将这个变体的
设备描述设为`Switch Lite GM20B .158 bit14 native boot b916f531`，保留版本0.1.0.158，
重新生成并签名CAT。`build.py`今后为同版本native测试KMD的INF加入实际二进制hash标识，
避免再次复用相同INF。Windows重新验证CAT及12个载荷全部通过。

17:26:15–17:26:17从Code43安装成功，OEM93→94，实际SYS变为
`c629328e7309381aa1b9923576ab57a4e664102a393d385ecf5643113641dd53`。
本次INF SHA256为`51c2094af405739492dec1709376590b5156d004ae586603d66bf03607692e40`。
UMD仍为`7c7f88cc…`，ICD仍为`dee3d580…`；两者完整hash见上表，没有再次改变Fence或渲染代码。
`5bd93331`的工具只配置持久键并flush，移除旧任务/测试许可；恢复入口会先删除持久键。
实际ACPI反编译确认GPU0已有`_DEP { FAN0 }`，未修改ACPI或风扇驱动。

最终包`tgd3d-158-bit14-d3423d26-persistent-boot2.zip`为7073289字节，66个清单文件，
SHA256 `445513a8efddcb684cc91cfe5db39703c77ed6155e0d497bc8ee9be1d3b53873`。
远端完整包、15个恢复文件及脚本解析通过。17:26:25实际核对安装文件、UMD/ICD注册、
持久授权存在、旧许可不存在、旧任务移除，状态为等待开机初始化，并未误记Code0。
17:27:36只请求一次Windows正常重启（非强制关闭应用），命令退出0；目标是确认直接
原生启动后的Code0和DWM加载。不会因SSH短时未恢复重复启动GPU或追加重启。

### 持久原生启动失败与标准主表面OpenResource修正

用户随后确认在登录用户名ruje出现前自动重启，本次启动失败。SSH未恢复，未取得Code0
或DWM加载证明。最初的远程关闭请求在SSH连接阶段超时，没有执行；用户随后要求
保留原生启动并检查已挂载SD，故`AllowNativeBoot`仍保留，没有回退或再次启动设备。

只读SD复制的SYSTEM序号为182019/182018，用现有LOG2在本地副本重放103项后为
182032/182032。副本中OEM94、SYS/UMD/ICD哈希及持久键与安装记录相符；17:30:27保存的
Initialize/Resource/Hardware/Trace/Panel/BootPermit/StartGateStatus均为0。
这些只确认所记录的初始化阶段完成，不说明登录或DWM成功。系统事件保留17:27:52的新启动，
没有可定位该重启的新转储；没有继续扩大日志搜寻。证据位于独立产物目录
`hardware/sd-native-boot-20261113/`，未写回注册表hive。

源码发现明确ABI缺口：冻结`.158`的`TgGpuDdiGetStandardAllocationDriverData`给标准
shared primary和shadow返回48字节`TG_GPU_PRIMARY_ALLOCATION_REQUEST`，资源级私有数据
长度为0；tgd3d的`runtime_shared_open`此前只接受72字节TGSR资源数据，会拒绝此输入。
新路径验证该冻结ABI的版本、flags、格式、identity方向、720×1280几何、2880字节pitch
和3686400字节backing；复用消费者allocation、驻留和GPU VA回调，不另建替代存储。
OpenResource同时保留primary标志供NVK的scanout布局使用。普通TGSR路径保留原校验。

2026-11-13核查微软文档：
[标准allocation契约](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/ns-d3dkmddi-_dxgkarg_getstandardallocationdriverdata)
（2024-11-11）允许资源级私有数据为0；
[D3D10 OpenResource](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/ns-d3d10umddi-d3d10ddiarg_openresource)
（2024-02-22）区分resource与allocation数据；
[windowed-mode行为](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/windowed-mode-behavior)
（2021-12-15）说明DWM未运行时会打开共享primary。这是与登录前阶段有关的调用路径，
并不是已观察到本次重启的调用栈或根因。

ARM64/x64 CPU-sync UMD构建通过。Windows x64合约在Wine下使用Vulkan/runtime替身，
新断言配旧runtime源码退出1，定位于标准allocation的打开失败；新实现退出0。
覆盖primary/shadow、ARGB/XRGB、消费者句柄、无额外allocation及畸形长度/保留字段拒绝，
原共享资源、同步和错误传播控制均通过。未执行Switch GPU，不计为实机通过。
结果与编译/链接命令在`standard-open-controls/`，本轮仅UMD改变。

用户要求本轮完成后直接部署供其测试，保持原生启动。修复当前启动失败后，先完成
着色器缓存和实际约60Hz VBlank/Present节拍，再进入硬件旋转；静止桌面按需提交。

### 旋转的静态准备（未改变实机显示方向）

冻结`.158`已含DC列扫描、H/V取址方向、完整几何锁存和VidPN/MPO映射；当前新包
保持这些文件逐字节不变。tgd3d的`runtime_primary_create`为四分之一转选择1280×720、
5120字节pitch、BGRA8888，与DC期望一致，并单独处理NONPREROTATED，不能重复旋转。

还需解决/验证的具体边界：`tgd3d_dxgi.cpp::Blt`明确拒绝非identity旋转。
微软[DXGI DDI契约](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/dxgiddi/ns-dxgiddi-dxgi_ddi_base_functions)
说明，若驱动允许主表面创建返回DDI unsupported，必须支持相应旋转复制；不能只设置
Windows方向标志便宣布全面支持。VidPN和Blt按逆时针约定，而MPO plane按顺时针约定；
应按实际接口映射，不按枚举数字直接推断。参考
[VidPN旋转](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/supporting-rotation-in-a-display-miniport-driver)
与[MPO属性](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/dxgiddi/ns-dxgiddi-dxgi_ddi_multiplane_overlay_attributes)。

重新核对Switchroot固定`3c40adf1081096bc61a7c1280434c163e7e79b48`的
[window.c](https://gitlab.com/switchroot/kernel/l4t-kernel-nvidia/-/blob/3c40adf1081096bc61a7c1280434c163e7e79b48/drivers/video/tegra/dc/window.c)：
列扫描使用交换后的预缩放维度，反向水平起点为末字节而非末像素。这里只用于核对语义，
没有复制GPL代码或把Linux路径当作Windows实机结果。核查日期2026-11-13；仍须先完成
该组件组合的桌面和已知像素阶段，才进行物理横屏方向、裁剪、鼠标与五分钟验收。

### 双向 Fence 审查：修正 Flush 错误类别，未宣称死锁已解决

Antigravity CLI 的 Gemini 4 Carbon High 独立审查已交付报告和 diff。主代理复核后
仅提取错误传播修正：runtime ingress signal 或 GPU gate 回调失败时，内部已通过
SetErrorCb 报告设备移除，却返回原始 E_FAIL；DDI Flush 外层再次把 E_FAIL 交给
runtime。现在内部返回已记录的设备丢失状态，外层不覆盖本次已报告的错误；设备在
DDI 进入前已经移除时，仍为这次调用报告移除。生产默认双向桥接保持开启。

依据微软 [Flush 契约](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/nc-d3d10umddi-pfnd3d10ddi_flush)
和 [SetErrorCb 契约](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/nc-d3d10umddi-pfnd3d10ddi_seterror_cb)
（均更新于2024-02-22，本轮2026-11-13核查）：Flush 应报告设备移除，其他错误被
runtime 视为严重错误；同一 DDI 多次 SetErrorCb 本身是允许的。

ARM64 CPU-sync 与 x64 两种同步模式构建通过。控制直接调用真实 DDI 函数表中的
Flush，覆盖 ingress signal、Vulkan submit、runtime gate 失败，以及已移除设备再次
Flush。相同新断言配旧 cmd/device 源码退出1，修正后的默认双向桥接和 CPU-sync 控制
均退出0；不伪造 Fence 完成，不提前退休资源。证据在产物目录 `flush-error-controls/`。
这些是 runtime/Vulkan 替身控制，未执行 Switch GPU，不能证明重启根因或实机稳定性。

未整份合入 Gemini 的设备隔离方案：它保留 VkDevice，却没有同时保留 VkInstance 和
ICD 模块生命周期；后续 CloseAdapter 仍会销毁 instance、卸载模块，违反
[vkDestroyInstance 的子对象契约](https://docs.vulkan.org/refpages/latest/refpages/source/vkDestroyInstance.html)
`VUID-vkDestroyInstance-instance-00629`。超时后无界 DeviceWaitIdle 仍待修复；正常
双向依赖链没有在本次审查中被证明必然死锁。标准主表面启动候选 `9de42c82` 保持
独立冻结，本次 Flush 修正和 `35696545` 着色器缓存均未混入该 SD 候选。

### 标准主表面候选已离线写入 SD，等待实机启动

`9de42c82a03e84674954d8175237dcfd5bb4dc2e` UMD 已替换原持久启动目录中的
`app/TegraGpuD3DVk.dll`，实际读回 SHA256 为
`6612e45e8059086e9fe8dfb7b1f9c1cd868bed78e19722da0aff9ce90d26fa9c`。
组件清单同步更新，SHA256 为
`ed2f6ee1cef70ceb462029efd34799f18a804c46a7efa2e668b20eaf61692c85`。
`.158` KMD 仍为 `c629328e…`，ICD 仍为 `dee3d580…`（完整哈希见前文）。
实卡 SYSTEM、SYSTEM.LOG1、SYSTEM.LOG2 与更新前逐字节哈希一致，原生启动授权
保留，没有切回 BasicDisplay、`.59` 或登录后启用，也未修改 CPU、风扇和显示方向。

原 Mac ntfscp 因 Windows 缓存元数据拒绝写入；只读 ntfsrecover 模拟也未能完成，
没有强制清空日志。原 ARM64 安装镜像在本地虚拟维护环境启动失败，最终使用微软
WinPE add-on `10.1.22621.5337` 的 x64 维护环境，仅处理 NTFS 和复制 ARM64 文件。
原生挂载后 CHKDSK `/f /x` 返回0，报告未发现文件系统问题；未用此结果推断
Switch 重启根因。WinPE 在写时复制副本内完成替换并核对全部组件后正常关机。

对该副本的971段改动、135856128字节保存原始前后扇区备份，再写回唯一匹配 UUID
的 WINARM 分区。全部扇区读回一致；随后以只读 ntfscat 从未挂载的实卡再次核对
UMD、组件清单、KMD、ICD 和上述注册表文件。备份在 Mac 及 SD FAT 分区各保留一份，
备份 zip SHA256 为 `247ea36fb5e097a594cc14c0dda77909411e866e15dbedf8b102ff8607fe8f8a`。
扇区恢复工具仅适用于记录内容仍匹配时的离线还原；Windows 运行后的驱动回退应使用
单独保存的旧 UMD/清单，不能盲目覆盖旧文件系统扇区。验证记录见产物目录
`standard-open-deploy/deployment-final.json` 和 `sector-backup/`。

本轮仅完成离线部署与读回，仍为 CPU 同步控制组合；尚未验证登录前原生启动、
DWM 实际加载或物理画面稳定性。缓存、生产双向 Fence 和实际60Hz时序仍须分轮验收。

### 9de42c82 实机启动失败；补齐 XRGB 主表面创建

用户随后明确报告“登录前又自动重启”，本轮原生启动判失败。两次 SSH 连接超时，
未取得这次实际加载模块，不重复安装或追加重启。标准 OpenResource 修正没有解决
当前可见故障；不能把文件校验或 CPU 控制通过写成登录前启动成功。

继续静态对比发现：冻结 `.158` 标准主表面/分配路径和 tgd3d 格式表都支持
X8R8G8B8，但 `runtime_primary_create` 仅接受 B8G8R8A8_UNORM，并固定向 KMD 请求
A8R8G8B8，导致合法 B8G8R8X8_UNORM 主表面在创建时被拒绝。本次仅补齐该格式，
按原始格式请求 X8 或 A8；沿用原有2880字节 pitch、3686400字节 backing 和线性导入，
没有改变同步或显示方向。依据微软
[CreateResource](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/nc-d3d10umddi-pfnd3d10ddi_createresource)
（2021-07-16）及 [primary 描述](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/dxgiddi/ns-dxgiddi-dxgi_ddi_primary_desc)
（2024-02-22），核查日期2026-11-13。是否实际走到此请求尚无实机证据，未认定为重启根因。

新控制从真实 CreateResource DDI 进入，验证 XRGB/ARGB 的 KMD 格式、Vulkan scanout
布局、900页 GPU VA、SetDisplayMode allocation 和释放；相同新断言配旧 runtime
源码退出1，修正后 CPU-sync/生产默认双向桥接控制均退出0，ARM64/x64 构建通过。
最初测试夹具的 SetDisplayMode 回调 const 签名不符已修正，这属于工具编译错误。
结果在 `primary-create-controls/`。下一实机候选以 `9de42c82` 为底，仅加入本项
主表面创建修正；完整开发分支中的缓存和 Flush 修正另行验收。

该单项候选已写入重新接回的 SD。源码组成为 `9de42c82` 基线加 `7f5f416b` 的
`tgd3d_runtime.cpp`，不是整个新分支快照。重新链接旧构建与冻结 DLL 的差异仅为 PE
时间戳字段；补丁使用冻结头文件及其他对象，并通过该组合自己的 x64 CPU 合约。
ARM64 UMD SHA256 为 `cfeab491db032ed6903a2d5f5bdb0c50dde89e9f7d17241564e52b908b52a6bc`，
组件清单 SHA256 为 `feafda8dec0fe2ac9493dacaef73b129db4acd08e89a16cdb07e0fd66d0fe78c`。
测试包 `tgd3d-primary-xrgb-7f5f416b.zip` SHA256 为
`dd0878ec036edbc73165521a6702b1fbf54061054715c8e76e1cb33bff31c8d5`。

本次再次因 Windows 缓存元数据未清理而被 Mac ntfscp 拒写，没有修改任何目标文件；
随后复用已通过的 x64 WinPE，在新的写时复制副本内执行 CHKDSK、复制及核对，返回0。
100段、13631488字节改动先保存还原备份，再写回实卡并逐段读回。实卡 UMD/清单及
保持原值的 KMD/ICD 哈希均正确。文件系统恢复将旧 SYSTEM.LOG1 截为0，但 SYSTEM
和有效 SYSTEM.LOG2 与这次更新前一致；使用13条校验过的事务在本地副本重放到182059，
确认当前 ControlSet001 的 AllowNativeBoot 存在。没有把重放副本写回实卡，不能把这次
记录误写为“所有注册表日志逐字节不变”。备份 zip SHA256 为
`b8b54b45fdf731ad2cc710c1f65b73057fcf5041ed48185e24803438d84a6ad6`，Mac/SD FAT 各一份。
完整交付记录位于 `primary-surface-deploy/deployment-final.json`；新组合实机启动仍待验证。

### XRGB 候选仍在登录前重启；修正资源失败状态传播

用户对已写入 `7f5f416b` 的 SD 明确反馈“登录前仍自动重启”。这次组合记为失败，
不能把前文的待验证状态理解为通过。一次 SSH 查询超时，未追加启动或重启；未取得
该次 DWM 的实际模块加载证据。XRGB 修正没有解决当前可见故障。

继续审查发现资源创建错误处理违反 DDI 契约：共享图像的 Vulkan 创建/绑定错误被
统一改为“不支持”，私有图像和缓冲的内存不足被报为旧版视频内存错误，上传分配器
还会丢失底层失败类型。本次让图像、共享导入、上传缓冲和视图保留失败类别；
Create/OpenResource 将内存不足报告为 `E_OUTOFMEMORY`，保留 runtime 回调的设备
移除状态，失败前清理半成品，已移除的设备不再分配。没有改变成功提交的 Fence 链。

依据微软 [CreateResource](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/nc-d3d10umddi-pfnd3d10ddi_createresource)
（2021-07-16）与 [OpenResource](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/nc-d3d10umddi-pfnd3d10ddi_openresource)
（2024-02-22），2026-11-13 核查：非约定错误会被 runtime 视为严重错误，且失败句柄
不会收到 DestroyResource。测试从真实 DDI 进入，注入 Vulkan 允许的内存分配/映射
错误，以及 runtime Allocate/MakeResident 的失败；没有将 Vulkan 创建 API 未声明的
设备丢失返回值冒充规范测试。

相同新增断言配旧 resource/runtime/upload 源码退出1，新版 CPU 同步和默认双向桥接
控制均退出0；ARM64 与 x64 构建通过。覆盖 A8/X8 主表面创建/打开、私有图像、池化和
独立缓冲、staging、视图以及失败后资源释放。证据位于 `resource-status-controls/`。
这些控制没有执行 GPU，尚无证据证明冷启动确实触发了此错误路径，故不认定为重启根因。
超时后无界 DeviceWaitIdle 仍是独立未解决问题。下一候选继续冻结 `.158`/bit14/ICD，
在原 XRGB 组合上只加入本项状态传播修正；缓存及 Flush 修正不混入该候选。

`42ad5d1bfa95c73466b7f76378f28792522742ec` 的单项候选已于
2026-11-13 23:41:44 UTC 写入 SD 并核对。采用冻结 `9de42c82` 头文件/其余对象，
仅替换 resource/runtime/upload 三个生产文件和相应两个函数声明，保留 XRGB 修正；
该组合单独通过 x64 CPU 合约。ARM64 DLL SHA256 为
`ad5e3309f1d5e8b7e549d70e5a5741da1ee6cf7c4e4f49782e0067c7b858b3f7`，清单为
`31d8ad2255f0276c393d8568805e834f56dee841e6ce0f79d03d6830934a7bd5`。
同时纠正清单 `files` 索引中遗留的旧 UMD 哈希，使其与 `app` 索引及实际文件一致。

复用 WinPE 离线更新流程，新副本 CHKDSK 返回0；75段、9043968字节差异先备份后
写回并读回一致。实卡 KMD/ICD、SYSTEM 及两个事务日志均与本轮更新前哈希一致。
本地副本使用13条 LOG1 事务恢复到182073，确认 ControlSet001 的 AllowNativeBoot
存在，没有把恢复后的 hive 写回。备份 zip 为4167945字节，SHA256
`bc51fc89e9a04f5dc93676c810facab722c5f7880a81267e8e071ea5f5cc985d`，Mac/SD FAT 各一份。
测试包 `tgd3d-resource-status-42ad5d1b.zip` 为2141175字节，SHA256
`3455f2273808322155c9cd6904870bc487369345ca0f6b1eab3fc4e064e5ae29`。
完整记录在 `resource-status-deploy/deployment-final.json`。本次仍为 CPU 同步诊断组合，
实机冷启动和物理画面尚未验收。

### 42ad5d1b 冷启动失败；收窄登录前路径审查

用户确认本轮仍自动重启，并进一步明确顺序为：Windows 启动界面之后、黑屏阶段之后、
出现用户名 `ruje` 之前。该候选记为失败，不能从这一画面顺序推断 DWM 已成功加载
tgd3d，更不能声称已定位到某条 GPU 指令。没有重复安装同一候选或追加远程重启。
物理反馈记录在 `resource-status-deploy/hardware-observation.json`。

本轮静态核对实际冻结代码和已取得的 SYSTEM 副本：

- `AllowNativeBoot` 分支只改变许可，不跳过 GPU 电源、时钟、固件或 panel 初始化。
  冻结 KMD 仍通过 `SwitchBacklight` 桥访问 PMIC；这不是本轮新加的路径。既有首次
  冷启动记录中的 Hardware/Panel 状态0只适用于那次记录，不证明新一轮已执行完成。
- 本次 SYSTEM 恢复副本的显示类0000仍为OEM94对应的`.158`驱动，D3D10/11注册入口
  指向已读回校验的持久目录 tgd3d DLL，ICD路径亦对应当前组合。没有发现注册入口
  意外换回旧版 UMD 的配置证据；实际模块加载仍未取得。
- 对照 WDK10.0.26100 的 D3D10DDI_DEVICEFUNCS，101个非D3D10PSGP回调均已填入。
  ResetPrimitiveID/SetVertexPipelineOutput 位于本构建未启用的 rasterization-only
  条件结构内，不能把它们误报为缺失的普通 DDI 入口。
- 当前实机候选是无GPU桥的CPU同步诊断构建。其出站同步不能替代异步外部生产者的
  入站依赖；此前 Gemini 审计也指出此局限。没有证据证明本次触发了该竞态，亦不能
  再仅凭“开窗/启动失败”把新双向GPU Fence认定为唯一原因。仓库生产默认桥接不变。

Microsoft [CreateContext flags](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/ns-d3dkmddi-_dxgk_createcontextflags)
（2024-06-21）将 SystemContext 定义为 paging engine 上下文；没有把它与所有GDI
上下文混为一谈。继续核对冻结 KMD 的 GDI shadow Present、显示接管及 tgd3d 的
首次设备/主表面/绘制提交。未以不相关的错误码补丁、延迟或强制成功替代启动修复。

### 冷启动 LogonUI / DWM 并发捕获已配置，尚待实机产生记录

按用户最新要求恢复有范围的冷启动取证。当前 SD 取回的 Application/System、
Winlogon、CodeIntegrity 和 WER 尚无能归属本次冻结组合的 LogonUI/DWM 崩溃记录。
旧 WER 指向 2026-11-11 的 `TegraGpuD3D.dll`；Application 中较新的 DWM 访问冲突
（2026-11-13 08:46:02 UTC，`c0000005`、偏移`b542c`）来自旧
`tgd3d-d6470feb-20261113-164031`目录的 ICD，不能当作 persistent-boot2 冷启动根因。
EVTX 中记录顺序不等于时间顺序，离线解析已分开两者。尚未证明缺 DLL 或特定锁超时。

新增 `tools/cold-boot-trace/`，根据微软 AutoLogger、GlobalLogger、WER LocalDumps
契约配置四路 QPC 时间戳跟踪：kernel 进程/线程/image-load/CSwitch，现有 tgd3d API/
submit/CPU-wait 事件，现有图形 provider 组合，以及目标 SOFTWARE 登记的
User-Loader/Winlogon/CodeIntegrity。ETL 分别64/32/64/32MiB循环，每种最多四轮；
每秒刷缓冲。WER仅为`dwm.exe`及`LogonUI.exe`各保存最多两份完整用户态转储。
没有 IFEO Debugger、loader snaps 或超时/恢复行为修改。硬复位仍可能没有 dump、
丢失 ETL 尾部；开始等待后没有结束事件不能独立证明死锁。

WinPE首次配置因`icacls`无法解析离线 DWM 虚拟组而退出8，该副本未写实卡；改用
数字SID构造DACL的`setacl.c`后，构建无告警、Windows原生ACL写入/读回和注册表
导入通过。DWM组仅获得自身转储目录的写权限，LogonUI转储仅SYSTEM/管理员可访问。
新COW内检查完成后备份134段、13434880字节，再写回并逐段读回成功；备份ZIP SHA256
`11d507997a7ffa1fe0c86fa87a5b75ce4bee10b4d1df1178d644adef9be8422b`，Mac/SD FAT各一份。
2026-11-14 00:24:24 UTC实卡读回：KMD/UMD/ICD/组件清单哈希保持原值，
AllowNativeBoot仍存在。逐键原始值比较仅增加14个SYSTEM跟踪键、3个SOFTWARE转储键，
未发现其他设置差异。比较工具对原注册表中无法解码的0x13类型改用原始字节比较，
没有忽略这些值。SD已安全弹出，等待用户直接冷启动一次。

证据和交付记录位于输出目录`cold-boot-concurrent-20261113/`，实卡目标为
`C:\ProgramData\SwitchWOA\ColdBootTrace-20261113`。当前仍是CPU同步对照UMD，
不能称为双向GPU Fence验证。按用户明确要求，**用户确认问题修复后才关闭本轮
LogonUI/DWM转储及启动跟踪**，由本窗口清理并核对重启不再自动采集，保留已取回证据。
恢复脚本与离线删除配置已备妥；不因单轮收证结束自动关闭，不改变原生启动。

### 首轮冷启动捕获：日志已取回，尚不能归因 DLL 或 Fence

用户最初回答“登录前仍自动重启”，随后补充：**加载转圈出现了几帧，尚未显示
`ruje`字样，随后重启**。不能再把该故障写成固定发生在账户界面出现前或切换瞬间。
SD已接回Mac，以只读挂载复制18份文件并校验；实卡KMD/UMD/ICD哈希仍与冻结组合一致。
数据与逐项结论保存于输出目录 `cold-boot-concurrent-20261113/round1/` 的
`collection.json`、`etl-summary.json`、`findings.json`。

四个session和全部provider的注册表Status均为0。四个ETL的EndTime、BuffersWritten
都未最终更新，不能按正常关闭文件读取，也不能把头部丢失计数0解释为没有丢事件。
按完整物理缓冲恢复（不改原始文件）：kernel61个缓冲/13428条记录、graphics5/98、
loader5/182、tgd3d1/2。后者两条仅为ETL元数据，没有已持久化的自定义UMD事件。
使用独立工具环境的dissect.etl3.14；适配其EventHeader GUID字节序、event-id查表及
cstruct4.7字段访问差异。kernel记录均可解码，graphics/CodeIntegrity仅按匹配版本和
精确payload长度解码，5条缺匹配schema的事件保留原始数据，没有猜测其含义。

恢复的kernel前缀包含17条进程事件、510条模块事件：`tegragpurender.sys`加载，随后
`csrss.exe`、`wininit.exe`、`winlogon.exe`创建；winlogon PID664的最后可见模块包括
user32、win32u、gdi32/gdi32full和imm32。没有已持久化的dwm/LogonUI进程记录或
tgd3d DLL加载记录。**这不证明DWM没有启动**，也不证明最后一条ETL就是崩溃时刻。
头部墙钟与文件及后续注册表时间存在差异，四路流以相同BootTime/QPC对齐，不能把
不同来源的墙钟直接拼成精确故障时间线。

两进程转储目录为空；Application/System/Winlogon/CodeIntegrity EVTX和旧DWM WER
与捕获前哈希相同，因此没有本轮新增的崩溃栈。已解出的图形错误含QueryInterface的
STATUS_NOT_SUPPORTED和两条早期IOCTL的STATUS_OBJECT_NAME_NOT_FOUND；前者对应
冻结KMD明确返回不支持的查询入口，后者没有DLL名称，均不能改称缺DLL或Fence超时。
线程切换数据不含具体Mutex名称；本轮也无已持久化的tgd3d等待序列可证明Fence死锁。

这轮没有替换驱动或改变启动方式。保留捕获设置至用户确认修复，继续对照实际冻结
代码审查首次设备/显示接管和资源提交，不用不完整的日志前缀排除tgd3d，也不将
未取得的LogonUI/DWM调用栈、GPU指令栈或锁超时写成已查明。

### 用这台系统的独立 SM4 程序检查 tgd3d → NVK/NAK

继续静态核对共享owner映射/释放及提交顺序，未发现把runtime句柄直接当物理地址
使用的代码。队列依赖的抽象顺序未形成简单环，但不据此宣称运行时Fence已验收。
VPS Claude按用户提供方式尝试只读复核，返回“Not logged in”，没有审计结论，
未更改其账号配置。Antigravity Carbon High原会话仍未交付report/diff；没有把
服务端stream超时当作完成或因此重开同一任务。

为检查实际DWM输入，给既有`tgd3d-contracts`增加`--export-dxbc`模式，复用生产
CreateVertexShader/CreatePixelShader及dxbc-spirv转换；只接受完整SM4.0 VS/PS、
ISGN/OSGN签名，检查容器/token/签名范围，成功后释放模拟shader。该模式不跑后续
合成失败注入，也不创建GPU设备。x64、ARM64构建通过，原CPU合约完整运行退出0。

从只读SD的`dwmcore.dll`（SHA256
`72e4a0545913eed25cc75d59894b6963a44ece1338c9d89658caaf5e433ae301`）与`dcomp.dll`
（`a1f9c1cd203036f95bb29f5025cfb30b14a9f04da0b5e38c7e04d0a7708178a0`）提取满足条件
的30个容器，按内容去重为28份：18个VS、10个PS。28份均通过生产DDI到SPIR-V
转换、Vulkan1.3静态校验和NVK/NAK SM53 CPU编译；最大输出1215条指令、64个GPR。
新x64编译工具对既有VS基准生成的机器码，与先前真实ARM64控制输出逐字节一致
（SHA256`aebccad99550c1849e8a46008dc09a28e2be2a6009f7e190c2a6bd97c6b04bea`）。

前端相对冻结9de42c82的差异只有最后的缓存调用；本次导出明确要求SPIR-V输入，
不接受缓存binary。NAK及`nvk_shader.c`相对实机ICD d3423d26无源码变化。
为本机编译控制补装匹配Rust1.90.0的x64 Windows标准库，复用现有ARM64构建的
bindgen设置方式；只构建控制工具，未生成或部署新的实机驱动。

这只证明这些独立程序的CPU编译接受性；运行时链接的库函数、实际崩溃时shader、
GPU执行和像素结果未覆盖，不能据此排除NAK/着色器问题，也不能称冷启动已修好。
Windows原始着色器及派生SPIR-V/机器码均仅保存在本机输出目录
`dwm-shader-audit/`，没有加入Git。输入偏移/哈希、逐shader结果与验证范围见
`inputs.json`、`frontend-results.json`、`spirv-validation.json`、`nak-results.json`
及`summary.json`。

## Deferred Init 对照构建（2026-11-13）

按用户要求增加独立诊断开关：设备创建时只留本地 completion timeline，桌面
就绪事件后在 DDI 线程切入双向桥接；保留原 timeline 和提交序号。shader 在创建
时保留校验/反射，NVK/NAK 后端与绘制布局延迟到首次使用。普通构建仍默认
启用双向桥接。完整语义、限制和参考见 [Deferred Init](TGD3D_DEFERRED_INIT.md)。

冻结组合上的候选 ARM64 UMD SHA256：
`9f195e43dbd5f5fa19e569c919cf39f67512f9b9b8cba4c432a5c3f3602bbae9`
（2932224字节）。全部 tgd3d 对象重新编译；KMD/ICD 不改，不带入磁盘缓存。
ARM64/x64 构建通过；冻结候选、HEAD 延迟构建、普通双向桥接构建的 x64 CPU
控制均通过。包括非阻塞调用、真实完成后切换、两方向导入失败清理、超时不能
退休资源、未使用 shader 不调用 NVK、反射和首次编译、布局部分失败后重试。
硬件冷启动与画面结果待本轮实机验证；没有据此宣布修复或稳定通过。
本地构建输入/输出/命令见 `deferred-init-candidate/build-manifest.json`。

写卡前补齐双输出完成条件：完成值取本地与 runtime 输出 Fence 的较小值，
CPU 等待同时覆盖二者。模拟本地先完成的控制证明尚未退休资源。第一版候选
只进入未提交的 WinPE COW 预演，未写实卡；最终候选及两种 HEAD 控制重新通过。

SD 部署已完成（UTC 2026-11-14T01:35:36.607483+00:00）：最终 a7a60d29 候选与登录辅助程序
逐文件哈希核对通过；347段、40435712字节写入均逐段读回匹配。SYSTEM 全量
键值对比无变化，SOFTWARE 仅增加 `Microsoft\Windows\CurrentVersion\Run`
内的 `SwitchWOA.Tgd3d.DeferredInit`。原生启动、KMD/ICD、平台设置及
冷启动采集配置保留。旧UMD、清单、限定范围的 `Restore-Offline.cmd` 已打包
到本机和 SD；扇区备份 ZIP SHA256：`c57527e9b94a1e7450e3d646c8148048daf48ed67731fd7293111b8c2b4e99b1`。
Windows 再启动后不得直接回写旧扇区，须用文件/注册表级还原流程。
本轮仅安装与控制验证完成，冷启动和实屏验收仍待用户插卡测试。

## Deferred Init 实机失败，按用户要求回退

用户反馈 a7a60d29 冷启动仍然自动重启，并明确要求回退本轮修改。没有取得
桌面30秒通过结果；本轮方案未解决问题。此反馈不证明某个具体调用已执行，
也不足以排除全部同步/着色器缺陷。

本分支生产代码、Meson选项和测试已精确恢复到实验前4c8691cb；只撤回5e029182、
a7a60d29引入的改动，保留此前修复及本轮记录。实卡还原目标是旧UMD
`ad5e3309f1d5e8b7e549d70e5a5741da1ee6cf7c4e4f49782e0067c7b858b3f7`
及旧清单，删除本轮桌面就绪程序和其专用Run值；KMD/bit14/ICD、原生启动和
既有ETW/WER不变。还原使用当前SD的新COW预演和文件/注册表操作，不回写
Windows启动前的旧扇区快照。实卡完成状态以新的还原回执为准。

用户随后将恢复目标改为“进入系统后5秒启用tgd3d”，因此覆盖上一条保留直接
原生启动的安排：撤销 AllowNativeBoot，保留相同 .158/bit14 SYS、ICD 和旧UMD，
复用 Default-Start.ps1 与 Tgd3dDefaultEnable 的 AtLogOn/PT5S 任务。离线仅放置
一次性登录引导，在当前已确认 EnableLUA=0 的配置下注册标准计划任务；不修改
UAC，也不构造私有 TaskCache。首次登录执行引导，后续登录使用持久任务。
是否成功触发、Code0和DWM加载仍需恢复后的真实Windows检查。

登录后启用恢复包已写实卡并核对（UTC 2026-11-14T01:58:52.486404+00:00）。UMD还原ad5e3309…；
SYSTEM仅删除AllowNativeBoot，SOFTWARE仅删除Deferred Init Run值并增加一次性
LogonSetup RunOnce。401段45088768字节读回一致；备份SHA256
`ded9e5a1c2d7b1cf2c6965d609d93f118b93e1d9062f20b93755cf9f14e217c8`。
SD安全弹出后，用户确认能进入桌面并持续操作约30秒；SSH尚未恢复，计划任务
实际注册/执行、Code0和DWM加载仍待核对。详见新交接报告
[TGD3D_COLD_BOOT_HANDOFF_20261113.md](TGD3D_COLD_BOOT_HANDOFF_20261113.md)。
