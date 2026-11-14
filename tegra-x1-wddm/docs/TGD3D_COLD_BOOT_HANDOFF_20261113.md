# tgd3d 冷启动重启：探索记录与后续交接

更新：2026-11-14（本机时区；交付回执使用 UTC）。包含原生桌面、冷启动探索、旋转
候选及后续审查；较早交接状态按时间保留，当前设备约束以本节为准。

## 当前设备授权（2026-11-14 UTC）

**最新：MPO有效掩码修正e28f5886已推送，并完成一次正常UMD横屏测试，仍失败。
14:49轮的source_visible/WIN_ENABLE仍为0，实际扫描面12点为0；逻辑图案5020点正确
不能替代上屏。该轮没有自动重启，14:49:50恢复竖屏；14:54:21发出计划恢复重启，
14:58:14已恢复原缓存组合1a86a02b…/0cd34833…/dee3d580…、Code0及原频率。
15:21:33交互只读确认当前/保存方向720×1280、DWM828加载原缓存UMD/ICD。
当前按用户新要求实现真正的GDI Rotate Blt及横屏shadow；只做本地构建，尚未安装。**

**14:21最新测试：强开WIN_ENABLE诊断0dd8c544已经安装并完成一次横屏红屏对照。
三个采样均source_visible0、WIN_ENABLE1，实际锁存地址`0xfbba1000`，12点红色一致；
用户确认“红色有但在闪烁”。只确认红色到达物理面板，稳定性失败，横屏尚未修复。
14:21:00恢复竖屏；14:22:44请求一次计划重启，以卸下诊断KMD。14:26:40恢复本轮
之前的1a86a02b…/0cd34833…/dee3d580…缓存组合，14:26:54读回Code0，DWM460加载
原缓存UMD，默认启用任务Ready、gpu-bridge。CPU1497600/GPU460800/RAM1600000kHz及
风扇13核对通过。没有追加重启，也没有回退`.59`。SSH最新地址为`ruje@192.168.8.6`。**

14:28:45交互会话只读核对：当前与保存方向均720×1280/orientation0，DWM的UMD和ICD
实际路径/哈希均为原缓存组合。14:31:17确认两轮红屏临时任务、测试/签名/安装目录及
上传工具已清理，ETW此前已停止；本地证据保留。用户随后明确要求删除强开代码，
KMD强开分支及构建/打包开关已移除；纯红UMD控制仍默认关闭，候选使用正常UMD。

本轮探针按用户要求：结束测试后清理实机临时任务、ETW会话和探针文件；确认修复后
移除诊断专用代码/构建开关，保留取回结果。Gemini4 Carbon High的SC7续作已通过
Antigravity CLI在独立mesa/windows-drivers/winx工作树启动，只做本地实现和构建，
不得操作实机；DVFS/托盘及SC7交付均未合入或部署。

**以下为J轮恢复后的历史状态：J轮缓存＋GPU预旋转候选已经安装并测试，横屏期间自动重启，记为失败。
12:02:50 UTC已恢复本轮更新前的缓存组合：KMD 1a86a02b…、UMD 0cd34833…、ICD
dee3d580…，读回`.158 / Code0`，DWM820实际加载恢复后的UMD。12:11:30确认当前及
保存方向均为720×1280竖屏。恢复过程没有追加系统重启，不回退`.59`。**
登录后5秒启用、双向GPU Fence、CPU1497600/GPU460800/RAM1600000kHz、风扇13及
512MiB GPU预算保持。完整哈希、失败范围和恢复记录见文末“J轮实机失败与恢复”。
当前SSH为`ruje@192.168.124.5`，不是Ubuntu用户；后续两次握手被远端关闭，未据此
重新启用、安装或重启。当前物理稳定性不能由恢复回执替代。

用户此前要求保持Ubuntu期间，没有自行切回Windows。Noble显示、DVFS、充电和SC7
配置已取回，APT源修复完成。Antigravity CLI Gemini4 Carbon High的独立任务已于
12:09:03 UTC退出0，交付DVFS、Windows电源/充电托盘和5秒鼠标隐藏的报告及diff；
尚未由本窗口验收、合入或部署，不能把代理报告中的测试声明当作实机通过。
用户澄清“Zink都不可用”指此前的硬件旋转架构，不能扩大为Zink整个渲染路径不可用。
旧架构没有横屏成功证据，不能作为本轮成功对照；源码差异只作线索，不能据此判定
tgd3d错误或照搬实现。当前优先审查屏障、Present完成及统一内存地址链，
红色主表面对照尚未实施；不重复安装未修改的J失败候选。

**选定基线：用户在组合B测试后明确要求“现在这个驱动直接作为基线”，后续在此基础上
修横屏上屏。** 组合为`.158 / Code0`，SYS
`69aa6036da81870efb439e8e09530ae662e810dccf887b603bdfeb6ba8a4a11a`，UMD
`34e35bcfbb17ea12eaf973502fbe96b95d6a9ab5cb6964e8002e338b826caeb6`，ICD
`dee3d5805af80696f87d094ec1c5f557d662f435336207992baf588e2dc1cee0`。安装目录
`C:\ProgramData\SwitchWOA\GpuUpdate\tgd3d-cffa4713-20261114-161143`；竖屏720×1280，
双向GPU Fence，登录后5秒触发启用。CPU1497600kHz/1120mV、GPU460800kHz/800000µV、
RAM1600000kHz、风扇驱动13及512MiB预算策略保持。后续候选恢复到本组合和竖屏，
不默认恢复旧59dd…组合，更不是`.59`。原59dd…仅保留历史应急备份。

基线命名是用户选定的继续开发起点，不代表横屏或完整稳定性已经通过。组合B包含
33e3621e和a5d80906，两项真实WDK控制、ARM64构建和Windows原生签名校验均通过；
08:12:50启用完成。08:17–08:19的G轮完成约30秒静止及60.54秒开窗（11次移动、
11次缩放），08:18:56切至逻辑1280×720/60Hz；5020个RGB像素容差0、差异0，但
**用户明确反馈没有上屏，横屏失败**。08:19:31恢复720×1280，用户确认已回竖屏。
该轮没有重启，DWM768/Explorer5260均保持；DWM等进程实际加载TegraGpuD3DVk.dll。
LUID `00000000000f27a0`仅属于该轮，不能作为未来重启后的固定值。
未完成五分钟横屏验收，也没有scanout backing原始像素读回；正确逻辑截图不能替代
物理上屏。结果、截图、组件及恢复状态在`rotation-timings-state/evidence/`和
`short-final-status.json`。08:22:25读回Code0，默认启用任务Ready，临时恢复任务已删除。
08:29:22移除本轮已结束的安装/测试任务，保留默认启用任务。打包器及恢复工具已允许
69aa…作为已审查的恢复基线；本地完整打包对照及Windows独立工具控制通过，旧基线
仍兼容、未知哈希仍拒绝。没有改写已安装包或重新安装驱动。下一轮生成候选时，
`baseline_signed_files`和`--kmd`均使用`rotation-timings-state/signed-package`；
完整当前组合快照保存在OUT的`tgd3d-current-baseline.json`。

**09:09最新实机：为定位交接边界，当前运行组合B加09c1046e只读快照的候选，SYS
`1a86a02b479b716b2b695ade0bd6860076d4d5f61a29d95b804cb655ce0bf164`，目录
`C:\ProgramData\SwitchWOA\GpuUpdate\tgd3d-cffa4713-20261114-165948`。** UMD/ICD、双向Fence
及频率设置保持；Code0、竖屏和默认启用任务Ready。用户选定的恢复基线仍是上述69aa…
组合B，未将读取候选另行宣称为横屏通过基线。H轮结果见文末。

**用户已在目录打包授权问题中明确回复“解除保持，打包后继续安装测试”。** 先读取
设备实际状态、完成CAT生成/签名校验后部署测试；不再受此前只读限制。用户随后要求
“两个一起更新”，因此本轮直接使用包含33e3621e目标激活门控及a5d80906方向状态修正
的B候选，不再分别安装A。源码提交仍各自独立可回退；不把本地构建或逻辑截图当作
实屏通过。

以下是解除保持前最后核对状态，须以本次重新读取结果为准。用户此前在source-vis-f
黑屏后要求“保持这个状态”，保持期间没有再部署或恢复。两项自动任务
`Tgd3dSourceVisibilitySequence`、`Tgd3dSourceVisibilitySequenceRestore` 已于06:29:53
核对为Disabled，未启动RecoveryF。06:31:10读回当前仍为本轮候选 `.158 / Code0`：
SYS `cb0aef8226171d0f6a392e4511c5472517a26264ebee034dd0762fca85c7e59f`，
安装目录 `C:\ProgramData\SwitchWOA\GpuUpdate\tgd3d-cffa4713-20261114-142334`，
UMD34e35bcf…、ICDdee3d580…、gpu-bridge、logon-default。没有恢复59dd…/14d…组合。
当前运行是06:28:33异常重启后的新启动，DWM PID776、Explorer PID5188；并非仍处于
先前黑屏时的硬件现场，不能用重启后寄存器推定故障前状态。完整本轮结果见文末。

用户要求结合Switchroot、微软文档和当前WDK复核，并让Gemini独立审查。Antigravity
CLI的Carbon High审查已在独立目录启动，只允许本地报告/diff，不允许操作实机。

用户随后明确指出：先前的四色横屏截图是正确的，实屏却黑屏。已重新查看保存的
`landscape90c-pattern-capture.png`：1280×720、四色/方向箭头/白边正确，对应控制记录
5020个RGB像素、容差0、差异0。截图由`Graphics.CopyFromScreen`取得逻辑桌面，并未
读取DC实际扫描的backing。该PNG属于较早的609ce9d9…/34e35bcf…组合，不能改标为
当前cb0aef82…的实屏证据。当前F轮另有逻辑像素匹配及用户黑屏反馈。
这将排查优先级收敛到最终primary交接、扫描地址/布局/启用及锁存、DC取数/输出路径；
尚不能区分“最终primary没有正确更新”和“DC没有扫描正确primary”，也不凭截图
宣判所有shader/Fence路径无误。仍不重复部署或旋转来代替定位。

## 冷启动交接时的结论与恢复状态（历史）

**GPU460.8MHz＋RAM1600MHz 的直接 tgd3d 冷启动仍自动重启，按用户反馈记为失败。**
该次失败仍使用 CPU 同步对照 UMD `ad5e3309…`，不能归到本轮随后准备的双向 Fence 版。
用户要求恢复“进入系统后再启用 tgd3d”，随后追加要求把双向 Fence 也写入，由用户测试。
SD已于 `2026-11-14T03:45:51.538472+00:00` 完成写入和实卡读回：74个文件哈希通过，
SYSTEM语义差异仅为删除AllowNativeBoot，SOFTWARE仅增加所需RunOnce。
后续登录引导、DWM实际加载和实屏稳定性仍待用户启动验证。

用户反复观察到：**接外部电源时，桌面天气模块和浏览器相对更稳定，但也会偶发重启；
电池供电时更容易复现。** 没有同负载、同电量、同观察时长的受控AC/DC比较，不能
给出改善百分比，也不能认定供电、PMIC、时钟或Fence中的某项已经是根因。
这条现象及低频冷启动失败一并交给后续审查，不能写成“插电已稳定”。

本轮已写入的配置与验收边界：

* 保留 `.158 + bit14 + GPU460.8MHz` KMD、`d3423d26` ICD、RAM1600MHz；
  CPU1497600kHz/1120mV、风扇13、睡眠设置与512MiB GPU预算保持。
* 删除持久 `AllowNativeBoot`，首次登录由 `!SwitchWOA.Tgd3d.LogonSetup` RunOnce
  调用已有 `Enable-LogonStart.ps1 -StartThisSession`，注册 SYSTEM 的
  `Tgd3dDefaultEnable` 登录任务，触发延迟 `PT5S`。首次RunOnce执行时间取决于
  Windows调度；5秒是触发延迟，不是GPU启用已经完成的承诺。
* UMD从冻结 `9de42c82` 加 XRGB `7f5f416b`、资源错误传播 `42ad5d1b` 出发，
  仅追加已审查的 `a537121f` Flush错误传播修正，去掉CPU同步控制宏，启用现有
  runtime→NVK与NVK→runtime的双向GPU Fence。没有恢复Deferred Init或混入shader缓存。
* ARM64、x64全部31个tgd3d对象分别重编；x64/Wine控制通过双向提交顺序、失败类别、
  DONOTWAIT、超时不伪造完成及在途资源不提前退休等检查。它们不执行真实GPU，
  新组合的物理桌面、五分钟稳定性、冷启动与双向Fence实机结果均未验收。
* 普通提交使用GPU依赖；两帧队列回压以及显式读回仍可能有CPU等待。不得称为
  “所有CPU等待均已消除”或“重启根因已修复”。软超时后的无界DeviceWaitIdle仍未解决。
* 保留原CPU同步UMD、匹配清单和控制程序的离线恢复包
  `D:\SwitchWOA\gpu460-logon-cpu-sync-backup.zip`；只恢复同步组合时保留登录后启用、
  GPU460.8和RAM1600。扇区备份对应更早的失败冷启动配置，不作为首选同步回退包。
* 未连接Switch SSH、未额外收集崩溃日志、未安排Claude/Gemini或新一轮实机自动测试。
  该交付时由用户测试并等待Claude安排；后续用户已授权继续硬件旋转和SSH自主测试，见文末。

频率的最后运行期读回为2026-11-14 03:16 UTC：GPU460800kHz/800000µV、
RAM1600000kHz（Ready=1）、CPU1497600kHz/1120mV；Code0且DWM加载tgd3d。
03:24:35 UTC曾按用户要求设为持久原生冷启动，随后用户报告仍自动重启。
这些旧读回不能代替本轮双向Fence组合启动后的实际状态。
电源/Speedo及频率记录见 [电源审查记录](TGD3D_POWER_SPEEDO_20261113.md)。

本次保留已有冷启动ETW/WER，按原约定在用户确认修复后清理。
既有 `EnableLUA=0`、计划任务私有TaskCache和显示配置不作改动。
注册语义参考微软 [LogonTrigger.Delay](https://learn.microsoft.com/en-us/windows/win32/taskschd/logontrigger-delay)
和 [Run/RunOnce](https://learn.microsoft.com/en-us/windows/win32/setupapi/run-and-runonce-registry-keys)，
查阅2026-11-13。离线阶段只放置引导，任务重新注册由下一次登录后的Windows完成。

此前Deferred Init撤回提交为`a019010d`，生产源码恢复到实验前`4c8691cb`；
登录引导工具为`f71cd0bd`。上一轮恢复后用户确认约30秒桌面正常，后续天气/浏览器
仍有重启。该轮02:11:31–02:12:06 UTC启用，SSH曾核对Registered/PT5S/Enabled，
DWM PID828和Explorer实际加载tgd3d；不能把该旧短时结果移用于本轮。没有合并main。

## 固定组件与源码边界

系统：Windows ARM64，23H2，build **22631.2861**；不能把构建SDK26100或维护WinPE
22621.5337当作实机系统版本。每次启动LUID重新读取，历史LUID不复用。

| 组件 | 源码来源 | 实际 SHA256 |
|---|---|---|
| 保留的729.6MHz恢复KMD | 冻结 `6c854e9f394c3d761fab85885bead4bbd4588581`，叠加 bit14 `253bc813e43a878359b9d6227e246cd7d1aa382f`、持久许可能力 `b916f53105f991294d6811718362e90f9af0ba01` | `c629328e7309381aa1b9923576ab57a4e664102a393d385ecf5643113641dd53` |
| 当前`.158` GPU460.8 KMD | 上述冻结基线、bit14和持久许可能力，加时钟对照 `68d9502b435c5563fa11f32949b045589111b171` | `59dd5ea77415a9ef474e8d8bc5a8cae86e74713d3c0ab534ae0ad683dacf6e81` |
| 本轮双向Fence tgd3d UMD | 冻结 `9de42c82` + `7f5f416b` + `42ad5d1b` + `a537121f6b9d8448a18a7bfe1ddc1d1c5332b493`；GPU-bridge构建 | `14d51a5526dccd00737e15bef0f3bd7a40b38535a47c9083184d62ad7d1eb6c4` |
| NVK ICD | `d3423d269f9e716eadc1237beadbfb6aa6049dbe` | `dee3d5805af80696f87d094ec1c5f557d662f435336207992baf588e2dc1cee0` |
| 保留的CPU同步对照UMD | 冻结 `9de42c82a03e84674954d8175237dcfd5bb4dc2e`，加 XRGB `7f5f416bc785e0ac9cc687c1ae6369e9355deed1`、资源错误传播 `42ad5d1bfa95c73466b7f76378f28792522742ec`，CPU-sync构建 | `ad5e3309f1d5e8b7e549d70e5a5741da1ee6cf7c4e4f49782e0067c7b858b3f7` |
| 已撤回 Deferred Init UMD | 上述冻结组合加 `5e029182`、`a7a60d29`；全部tgd3d对象重编 | `9f195e43dbd5f5fa19e569c919cf39f67512f9b9b8cba4c432a5c3f3602bbae9` |

**硬件包不是整个HEAD快照。** 当前开发源码中的 shader binary cache不在本轮冻结UMD内。
本轮显式带入`a537121f`并构建GPU桥，之前冷启动失败包仍是CPU同步对照。
仅完成编译和离线安装不能写成生产双向Fence实机通过。

固定源码导出分别位于输出目录的：
`kmd158-gpu460/source/tegra-x1-wddm`、
`gpu460-bidirectional-candidate/source/src/nouveau/tgd3d`、
`resource-status-candidate/source/src/nouveau/tgd3d`；
已撤回实验源码位于 `deferred-init-candidate/source/src/nouveau/tgd3d`。

## 已做过的主要轮次

| 轮次／修改 | 代码或控制结论 | 实机结果与范围 |
|---|---|---|
| 早期 `3c5d327d` | DWM实际使用tgd3d，保留251次成功Present | 用户确认短时桌面可用，打开CPU/风扇/温度程序后重启；不是稳定性通过 |
| 关闭跨队列GPU桥的同步对照 | 保留本地Vulkan timeline，Flush/Present CPU等待 | 有开窗成功，随后天气/网页关闭仍有失败；不是双向Fence已修好 |
| `fcc53bf1` UMD错误处理 | 修正设备丢失通知及失败对象生命周期 | 原 `.158`、旧ICD组合通过30秒、60秒和300.262秒交互，用户确认；随后Home导致Explorer重启，整体稳定性仍失败 |
| `d3423d26` ICD | NVK meta保存/恢复公共shader绑定标记；旧控制会丢VS，新控制保留VS | 从门控状态启动后Home正常；天气仍重启。该轮仍是原SYS，不能归因于后加的bit14 |
| `253bc813` bit14 | 在golden context保存前清除 `0x419e44` bit14并读回；保留其余异常位 | 用户指定作为固定比较基线，尚无同组合完整稳定性结论 |
| 持久原生启动 | 一次性许可不适合持续开机；`b916f531`增加显式持久许可 | 直接开机仍异常重启；许可改动不等于修正电源或绘制流程 |
| `9de42c82` | 支持标准primary/shadow OpenResource，不强制私有TGSR描述 | 冷启动失败；UMD SHA `6612e45e8059086e9fe8dfb7b1f9c1cd868bed78e19722da0aff9ce90d26fa9c` |
| `7f5f416b` | 补齐合法XRGB主表面，保留原格式与布局；真实DDI CPU控制旧失败/新通过 | 冷启动失败；UMD SHA `cfeab491db032ed6903a2d5f5bdb0c50dde89e9f7d17241564e52b908b52a6bc` |
| `42ad5d1b` | 保留OOM/移除类别，正确清理失败对象；资源/上传控制通过 | 冷启动仍失败；保留为本轮UMD的资源修正基础，不把它叫作已修复冷启动 |
| 并发冷启动捕获 | 配置DWM、LogonUI局部WER与4路ETW | 未得到可归因的进程Dump；旧流补全发现见下一节 |
| 实机系统shader的CPU编译复现 | 28份独立SM4程序均通过tgd3d→SPIR-V、Vulkan1.3校验及NVK/NAK SM53编译 | 无GPU执行/像素验收；未覆盖运行时链接库与实际故障shader |
| GPU460.8＋RAM1600，CPU同步UMD | 频率读回正确，保持CPU1497.6、bit14和修复ICD | 用户反馈直接冷启动仍自动重启，失败；接电相对更稳定但未通过稳定性验收 |
| 本轮GPU双向Fence＋登录后启用 | 冻结UMD加已审查Flush错误传播修正，完整重编及CPU控制通过 | 等待用户实机测试；不能复用旧组合通过结果 |
| Deferred Init | 布局/NAK后端延到首次使用；桌面事件后建立GPU桥；双输出完成检查及失败清理控制通过 | 用户反馈仍重启，已撤回。未取得新版实际加载证据，不能据此排除全部初始化/同步问题 |

五分钟通过的精确历史组合：原SYS
`46d1757674ff156028ce5b729ba53671981ffab09948fcaf125751c46bb10088`，
fcc53bf1 UMD `7c7f88ccf83ad1a89f663c3205e6ba2cb6fec587d946efd3622839c9deb2b089`，
旧ICD `17dc3110393365eef279a90104850d6f517c74659441916266afeae0ae2c8f73`。
它不能代替当前组合验收。详见 [完整轮次记录](TGD3D_NATIVE_20261113.md)。

## 新取回材料：旧冷启动流得到补全

这次取回的4个 `.etl.001` 都逐字节包含此前 round1 文件的完整前缀；这是旧流的
补全，不能仅按本次取卡时间将它标作 Deferred Init 的新日志。只对已知固定布局
解码tgd3d记录；原始文件没有改写。ETL头的 EndTime/BuffersWritten仍为0，尾部不完整。

* 22个物理缓冲内有989条有效自定义事件：DWM PID792为800条，LogonUI PID784为189条。
* 两进程各有成功CreateDevice，包含原有5个创建阶段；有32次shader后端完成记录。
* 成功Present记录为DWM 71次、LogonUI 5次；记录中的完成值分别达到80和7。
  这是调用/完成值证据，不代替物理画面与稳定性验收。
* 两条 `0x887b0001` 来自 staging Map 的等待分支，是本驱动的
  `DXGI_DDI_ERR_WASSTILLDRAWING`，不能把它当作已证明的Fence超时。
* 没有Deferred Init事件19；kernel Image_Load记录的UMD映像尺寸为2445312字节，
  与实验前尺寸相同，与撤回候选2449408字节不同。尺寸不是完整文件哈希，
  但足以说明这些记录不能证明 `a7a60d29` 已加载。
* 本次取卡时保存的Autologger FileCounter已到2，而上述已取回流为 `.001`；后续应优先
  核对Windows可见的 `.002`，避免把NTFS事务尚未回放时看到的目录当作完整记录。

旧流现在能证明DWM与LogonUI都进入了tgd3d并提交/呈现，修正此前“持久化前缀尚
没有DWM记录”的资料状态。仍无具体故障栈、超时返回或最后GPU指令，不能宣判
缺DLL、Mutex死锁、非法SM53指令、供电或时钟中的某项已定位。

证据：`deferred-init-rollback/cold-boot-artifacts.json`、
`analysis/umd-events.jsonl`、`analysis/loaded-umd-identities.json`、
`analysis/cold-stream-findings.json`。快速启动和休眠的取卡时注册表值均为0，
没有因上述旧流现象擅自修改电源配置。

## 已明确的修正与尚未证明的因果

1. **Home/Explorer的CPU访问违规已有具体缺陷。** 旧ICD异常位于
   `nvk_cmd_flush_gfx_shaders+0x358`，访问空的 `last_vtgm`。meta恢复遗漏
   `pipeline_shader_stages`，PS-only更新会丢VS；d3423d26修正该状态保存/恢复。
   这与整机冷启动重启应分别讨论。
2. **所谓46506字节问题不是直接改一条SM53指令。** NETB该偏移存放寄存器
   `0x419f78` 的地址字段，值在后8字节；冻结 `.158` 已在golden save前清bit3，
   `0x10beb→0x10be3`，并检查读回。bit14是另一个寄存器
   `0x419e44`，`0x00d3eff2→0x00d3aff2`。后续context restore后的实读仍未取得。
3. **风扇桥接在启动阶段已有路径。** KMD通过 `\Device\SwitchBacklight` 与
   SwitchFan13的内核IOCTL通道做PMIC/时钟等操作；不依赖桌面温度程序先运行。
   历史ETL中switchfan早于GPU驱动加载约4.6秒。尚无负载瞬态电压/转速测量。
4. **Fence仍有未解决责任。** 保留的CPU对照没有完整runtime→NVK ingress；本轮GPU桥
   必须单独验证。软超时后的无界 `DeviceWaitIdle` 仍未解决，不能直接删除等待并
   释放在途对象。旧全局quarantine方案的VkInstance/ICD/运行时allocation寿命不完整。
5. **着色器静态编译通过不保证硬件执行正确。** Windows独立SM4测试为18VS、10PS；
   最大1215条指令、64GPR。没有捕获实际失败shader及其完整绑定资源、描述符和上下文。

来源与适用条件见 [GR helper/bit14说明](GR_HELPER_LOADS.md)、
[微软DDI线程规则](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/changes-from-direct3d-10)、
[Vulkan同步规范](https://docs.vulkan.org/spec/latest/chapters/synchronization.html)。
比较 Windows 后端时使用 `nvkmd/windows`，不能把 `__SWITCH__` 分支当作已在Windows生效。

## 工具问题也必须保留在交接中

* `desktop-path.exe`退出4是缺少可选私有导出，已修正验收分类，不算GPU失败。
* 曾有测试窗口未真正显示；修正ShowWindow/交互会话调用后才取得可见窗口对照。
* `.59`上的另一轮桌面异常与本窗口取证进程有关：事件2004指向
  `sd-dump-state.ps1`所属PowerShell PID2408，提交量增长到5.26GB，系统提交耗尽。
  SSH超时后没有及时识别远端进程仍在运行，是本窗口流程错误；没有栈证据定位到
  具体CIM/序列化语句，不能据此认定tgd3d资源泄漏。后续改为限量原生命令采集。
* 强制复位曾留下零填充状态JSON和不匹配恢复副本，已引入WriteThrough、Flush与
  恢复集校验。恢复时使用当前SD的新维护副本，不覆盖Windows运行前的旧扇区快照。
* 当前Mac NTFS只读；实际写入复用Windows WinPE原生NTFS检查和COW预演，逐段备份、
  写回、读回。文件系统检查通过不解释GPU重启。

## 等待用户安排 Claude 的检查入口

用户另行安排后再提供本报告、冻结源码与交付清单；当前不派发审查，不并行操作设备。
请先从实际组合出发，而不是审整个HEAD后默认所有改动已部署：

1. 对比登录后启用与直接冷启动的DWM/LogonUI并发、NVK/runtime上下文、GPU VA及
   共享allocation生命周期。旧流证明两进程都曾提交，但尚无根因。
2. 分开审查默认双向桥和实际CPU对照。给出具体依赖链及失败后寿命，不能靠
   CPU伪造完成、提前退休或默认屏蔽同步来宣称修复。
3. 从真实DDI资源入口检查标准primary/shadow、pitch/offset、XRGB/ARGB及跨进程
   导入；注意已有9de/7f5/42ad修正，避免再次提交同一修正作为新根因。
4. Shader审查覆盖dxbc-spirv映射、NVK公共绑定状态、NAK SM53及实际资源布局。
   静态可接受和GPU语义正确分别报告；保留具体来源提交。
5. 首先核对 `.002` 的会话、模块尺寸/版本和启动身份。报告确定缺陷与因果假说，
   给可回退diff及最小必要控制；不要将旧ETL前缀、API成功或CPU控制包装为实屏通过。

Carbon曾返回修订审计，bit14建议经本窗口独立核对后采用；另一次冷启动审计会话
在503/EOF等错误后停止，没有交付所要求的report/diff，不能标作完成审计。
未将未验收的第三方diff合入当前硬件包。

## 硬件旋转接续点

用户后续已授权继续旋转并恢复SSH，要求尽量自主完成测试；本轮进度见文末。
冻结 `.158` 已含DC列扫描、H/V取址、几何锁存与VidPN/MPO映射，尚无该组合实屏
横向验收。tgd3d四分之一转主表面已有1280×720、5120字节pitch处理；
`tgd3d_dxgi.cpp::Blt` 的非identity旋转仍返回不支持，是需要核查的明确边界。

依据固定Switchroot `3c40adf1081096bc61a7c1280434c163e7e79b48` 的
[window.c](https://gitlab.com/switchroot/kernel/l4t-kernel-nvidia/-/blob/3c40adf1081096bc61a7c1280434c163e7e79b48/drivers/video/tegra/dc/window.c)
和微软 [旋转契约](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/supporting-rotation-in-a-display-miniport-driver)
核对列扫描、预缩放维度、末字节反向起点及不同接口的方向约定。
用四角异色、箭头、文字和一像素边框检查真实扫描地址、方向、裁剪与鼠标坐标，
通过再保存方向设置，不能只改Windows方向标志。

用户之前要求的shader缓存与实际60Hz/VBlank时序仍需完成对应组合验证；冻结包
尚未带入缓存，SyncInterval=1仅是请求。当前也没有已知像素、横屏五分钟或vkcube
通过结果。不得将旋转、缓存、同步及KMD多个变量一次性混装来代替逐项验证。

## 本机交付位置

根目录：`/Users/rujian/Documents/Codex/mesa-switch-woa-tgd3d-vulkan-out`。

| 内容 | 相对输出目录 |
|---|---|
| 本轮恢复计划、实卡回执、注册表差异和CPU同步备份 | `kmd158-gpu460/logon-restore/` |
| 本轮双向Fence源码、完整构建命令/哈希与控制结果 | `gpu460-bidirectional-candidate/` |
| 上一轮Deferred Init恢复及备份 | `deferred-init-rollback/deploy/` |
| 用户的延迟初始化失败反馈 | `deferred-init-candidate/physical-result.json` |
| 撤回实验的源码、命令、对象/二进制哈希及CPU控制 | `deferred-init-candidate/build-manifest.json`、`source/` |
| 旧冻结资源修正组合 | `resource-status-candidate/build-manifest.json`、`source/` |
| 冷启动捕获设置与初次记录 | `cold-boot-concurrent-20261113/` |
| 本次取回的旧流补全及固定结构解码 | `deferred-init-rollback/cold-boot-artifacts/`、`analysis/` |
| 真实Windows独立shader的编译控制 | `dwm-shader-audit/summary.json`及逐shader结果 |

Windows原始shader及派生代码留在本机，未加入Git；后续交接按实际需要传递材料。

## 双向Fence之后的横屏模式协商续查

用户随后要求推进硬件旋转，并明确允许连接SSH测试。2026-11-14 03:54:22 UTC
读回当前UMD `14d51a55…`、KMD `59dd5ea7…`，启动模式logon-default，状态Enabled，
HardwareStatus/PanelStatus均0、GPU460800kHz/800000µV；DWM PID840、Explorer PID1568
和系统UI进程均加载tgd3d。后续PnP查询为Code0。活动源、目标、原生KMT的LUID均为
`0000000000091038`；交互桌面Default/session1，720×1280、60Hz、identity。
03:57 UTC电源API为AC=1、电量99%。这些是当前运行状态，仍不构成长期稳定性验收。

现有包内display-state是旧工具，rotation-test首次退出2，只表示命令不支持，未改模式。
从当前源码重新编译工具后，90°严格CCD校验返回1610。随后只读对照确认：

| 请求角度 | 严格CCD | 允许调整模式细节 | 省略目标模式 | 源/目标均由OS选择 | GDI CDS_TEST |
|---|---:|---:|---:|---:|---:|
| 0° | 1610 | 0 | 1610 | 0 | -2 |
| 90° | 1610 | 1610 | 1610 | 1610 | -2 |
| 180° | 1610 | 0 | 1610 | 0 | -2 |
| 270° | 1610 | 1610 | 1610 | 1610 | -2 |

整个查询前后都保持原720×1280，没有强行应用失败配置或请求重启。
KMT真实模式列表仅两项：720×1280/XRGB/60Hz，orientation=1和3；
ValidatedAgainstMonitorCaps=1，ModePruningReason=7。**7在这里是驱动推荐、面板接受的
原因，不是模式被裁掉的错误码。** 推荐目标也为720×1280、78MHz、total1000×1300。
GDI普通模式列表为空，raw/rotated列表只有800×600占位模式，不能据此说面板只支持800×600。

本轮候选仅修正KMD模式枚举：保持未旋转源模式720×1280/2880 pitch，允许该模式
对应四个旋转角度，并保留已固定的旋转支持约束；移除将它与CCD横屏桌面尺寸混为
一谈、排除90°/270°的条件。实际DXGI主表面仍通过48字节描述明确提交自身
1280×720/5120 pitch等布局，allocation和扫描控制代码未变。

固定`.158+bit14+GPU460.8`源码上仅叠加panel_vidpn.c及匹配控制测试，候选SYS SHA256：
`609ce9d9e119aace6a29cb04b7ada290aa0cffacc4f9db898f433b5c24d7a4c4`。
主机构建和模型通过；同一新VidPN断言链接旧实现退出170、修正后退出0，扫描/布局
控制退出0。Inf2Cat无错误/警告，13个签名目录成员经Windows验证通过。
这些检查不执行真实GPU；后续安装及失败结果见下一节。
恢复包保留当前`59dd5ea7…`的460.8MHz KMD，不回到729.6MHz或`.59`。

本轮实际参考：微软[旋转契约](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/supporting-rotation-in-a-display-miniport-driver)、
[模式协商约束](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/enumerating-cofunctional-vidpn-source-and-target-modes)、
[SetDisplayConfig及ALLOW_CHANGES](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setdisplayconfig)、
[模式支持原因](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmthk/ne-d3dkmthk-_d3dkmdt_mode_pruning_reason)，
查阅2026-11-13；WDK/SDK为10.0.26100.0，实机仍22631.2861。
Windows-driver-samples/KMDOD的main文件于同日核对，仅参考模式接口；其CPU显示实现
不作为GPU实现移植来源。Switchroot固定`3c40adf1081096bc61a7c1280434c163e7e79b48`
的window.c用于核对列扫描、反向起点和VBlank更新规则，未复制Linux代码。
控制与原始查询保存在仓库外 `rotation-bidirectional-20261113/`。

## 临时横屏失败及恢复（2026-11-14 UTC）

04:23:33 候选 `609ce9d9…` 启用为 Code0，UMD仍 `14d51a55…`、ICD仍
`dee3d580…`，双向Fence开启。原生LUID `00000000000dc272`，Win11 22631.2861。
KMT由两个方向变为四个方向，`SDC_ALLOW_CHANGES`下0/90/180/270均能校验；
严格CCD仍返回1610。04:32:54起竖屏图案控制运行约30秒，逻辑截图四块色样及完整
白边共5020个RGB像素、容差0、差异0，程序退出0。没有物理画面验收。

04:34:54的第一次90°请求实际上已被Windows接受，但工具把CCD源尺寸与逻辑桌面
尺寸混为一谈，错误地返回13；该轮立即恢复竖屏成功。这是工具误判，不能算GPU失败。
工具改为分别核对native CCD源720×1280、横屏DEVMODE1280×720及原始DSI时序。

04:37:57 第二次临时90°应用返回0，读回DEVMODE1280×720、CCD源/目标720×1280、
path rotation=2，注册表仍portrait。用户随后报告自动重启；新启动时间为04:38:20。
最后落盘阶段为TemporaryLandscape，没有彩色图案完成记录，**本轮实机失败**。
不能据此确定DC已正确旋转、GPU已经完成，也不能确定根因是Fence、供电或扫描布局。
未保存永久横屏，未执行五分钟横屏或vkcube。

04:43:02请求一次恢复所需的重启，关闭测试启动许可；04:45:04核对Code43门控关闭。
恢复到原GPU460.8 KMD `59dd5ea77415a9ef474e8d8bc5a8cae86e74713d3c0ab534ae0ad683dacf6e81`，
保留双向Fence UMD `14d51a5526dccd00737e15bef0f3bd7a40b38535a47c9083184d62ad7d1eb6c4`
及ICD `dee3d5805af80696f87d094ec1c5f557d662f435336207992baf588e2dc1cee0`。
04:46:50恢复Code0和登录后5秒启用流程；DWM PID780及Explorer加载tgd3d，
GPU读回460800kHz/800000µV，HardwareStatus/PanelStatus均0。
没有回退`.59`、没有恢复高GPU频率、没有关闭双向Fence。
04:48:13–04:49:15恢复后的60秒竖屏图案控制退出0，5020个逻辑RGB像素全部匹配，
前后Code0。用户当时离开，实屏仍未验收。

继续静态核对固定Switchroot `3c40adf1` 的dc_config.c/window.c/ext/dev.c：
T210 Display A Window A声明列扫描及packed rotation支持；列扫描尺寸交换、末字节
反向偏移及VBlank几何更新与当前实现相符。这不能证明Windows上的实际寄存器锁存。
微软SetVidPnSourceAddress及旋转契约也已核对，未据此擅自改动供电、Fence或多项KMD路径。

后续发现UMD创建/打开路径不一致：创建主表面及KMD分配描述支持四方向，
无UMD资源payload的OpenResource却硬编码identity/720×1280/2880。
契约测试已在旧 `14d51a55…` 对应对象上复现失败；修正按分配描述验证旋转、尺寸、
pitch，保持系统标准shared primary/shadow的未旋转契约，不重新分配或旋转像素。
冻结源码只叠加该修正；x64/ARM64构建和x64 CPU契约通过。
这是一处确定的源码不一致，尚无证据证明失败的90°轮实际经过该拒绝分支；不写作已修复重启。


## 旋转主表面打开修正 cffa4713 的本机验证

修正提交 `cffa47137b01a52a9ea4ea8b562dbb210404fef6` 已推送 `tgd3d-vulkan`。
相对上一次失败组合只改UMD打开主表面的布局校验，仍是双向GPU Fence；
KMD仍 `609ce9d9…`、ICD仍 `dee3d580…`，未带入shader缓存、Deferred Init或其他主线改动。
ARM64 UMD SHA256为
`34e35bcfbb17ea12eaf973502fbe96b95d6a9ab5cb6964e8002e338b826caeb6`。
恢复包仍保留GPU460.8 KMD `59dd5ea7…`，完整旧双向Fence包 `14d51a55…` 也保留。

x64 CPU控制和Switch上的ARM64 CPU控制均退出0；覆盖四个方向的ARGB/XRGB主表面
创建/打开、确切extent/pitch、导入/绑定失败及释放。错误旋转、旧尺寸配新角度、
错误pitch及旋转标准shadow仍被拒绝。控制程序不会执行GPU，不作硬件通过依据。
Windows验证归档70个文件哈希通过；压缩包SHA256：
`984f1bc29f9a8f374b58b7c0aecdf6ac2b210c28188c446ffba63e0a225fb49e`。

04:53:57请求一次安装所需的门控重启；启动时间04:54:33。04:58:20安装/启用结束，
Code0，DWM PID776模块路径指向新目录
`C:\ProgramData\SwitchWOA\GpuUpdate\tgd3d-cffa4713-20261114-125722`，
UMD和ICD模块文件哈希均核对通过。GPU460800kHz/800000µV、CPU1497600kHz/1120mV、
RAM1600000kHz、风扇驱动13，仍登录后5秒启用。
04:59:01–04:59:34的竖屏图案控制退出0，5020个逻辑RGB像素、容差0、差异0；
用户离开，不能作为实屏或五分钟交互验收。

后续SSH连接反复超时；05:06:30读回仍是04:54:33这次启动、Code0，
尚无新一轮90°任务或状态文件，确认之前的请求未启动测试。改为上传脚本后短命令
执行，05:07:41确认 `Tgd3dRotationApply90C` 已启动；未重复安装驱动或请求重启。
新轮次标识为 `apply90c`，控制器结束时恢复原方向，不保存永久横屏。
输出位于 `rotation-primary-open-candidate/` 和 `rotation-bidirectional-20261113/`。


`apply90c` 结果：05:07:48临时切为CCD 90°，原生LUID `00000000000cc125`，
DEVMODE1280×720/60Hz、CCD源/目标720×1280。05:07:50–05:08:53彩色图案运行60秒，
四块16×16色样及完整一像素白边共5020个RGB像素，容差0、差异0，程序退出0。
05:08:54自动恢复720×1280/identity，恢复退出0，后备恢复任务已撤销。
05:09:17核对仍同一次04:54:33启动，Code0；DWM PID776（04:55:06启动）和Explorer
PID5184（04:55:35启动）均未在本轮重启，模块哈希仍为34e35bcf…/dee3d580…。
这说明本轮短时**逻辑横屏图案**检查完成，不能认定重启根因已证实或硬件旋转已验收。
没有物理方向、触摸位置、真实DC锁存寄存器或横屏五分钟通过记录。
当前保留新组合及双向Fence、logon-default；原方向已恢复，旧59dd…/14d…组合可还原。


## 同一组合二次旋转再次重启（apply90d）

用户回到现场后，05:11:57启动物理观察轮次。组件仍为609ce9d9…/34e35bcf…/dee3d580…，
LUID仍00000000000cc125。仅图案工具将下方文字移到底部，避免中央说明遮挡角标。
05:12:04第二次临时切90°返回0；最后状态仍TemporaryLandscape，没有图案完成记录。
用户确认重新出现启动/登录画面；05:15:06读回新启动时间 **05:12:29.7011980 UTC**。
因此 **apply90d实屏失败，组合稳定性和硬件旋转均未通过**，不得用前一轮60秒逻辑
图案检查抵销这次失败。停止扩大测试，恢复旋转前59dd…/14d…/dee3d580…及原方向。

两个序列均出现“首次临时横屏→成功恢复竖屏→再次切横屏后重启”：旧UMD的apply90A/B，
新UMD的apply90C/D。用户确认D轮从开始到重启一直插着外接电源。样本仍有限，不能据此直接定根因；下一步
优先查反复模式切换中的主表面/backing退休、GPU VA复用与缓存视图，不重复安装同一
失败组合来代替分析。cffa4713修复的OpenResource不一致仍是独立确定缺陷，不能写作
本次重启根因或稳定性修复。D轮没有永久保存横屏，也没有执行五分钟交互或vkcube。


05:16:21发出一次恢复所需的正常重启，05:17:10启动，05:18:38确认Code43且启动许可
关闭。05:20:52恢复59dd…/14d…/dee3d580…、Code0、双向Fence及logon-default。
05:22:02读回DWM PID832实际加载旧双向FenceUMD和原ICD，CPU1497600kHz/1120mV、
GPU460800kHz/800000µV、RAM1600000kHz、风扇驱动13。用户确认原方向恢复、桌面能操作。
D轮只取回既有轮次状态及恢复回执；图案ready/log/state文件不存在，不能因此断言
物理屏幕一定未显示过图案。没有新增转储或扫描历史日志。

用户随后要求常规测试自行完成，不再逐步索要确认。后续模式切换、窗口交互、逻辑
像素、进程/启动连续性和恢复由本窗口自动执行；无法自动验证的物理显示项目如实
保留未验收状态，不能将截图或Code0当作实屏通过。恢复后停止重复同一失败组合，
继续依据具体源码缺陷形成独立修改，再运行相应控制和实机测试。

## 模式切换锁存修正（构建/控制阶段）

审查 `panel_scanout.c` 发现ModeChange与普通异步翻转共用立即返回路径：回调返回
SUCCESS时，旧主表面仍可能在active寄存器组，新表面只在pending中。旧pending未完成
时又会直接返回DEVICE_BUSY。新的候选只调整这条模式切换路径：在PASSIVE调用中先等
前一笔pending翻转完成，再排队新主表面，并通过既有完整布局读回确认后才返回成功；
不把“已排队”当成“已锁存”。模式切换仍用VBlank，普通帧翻转和双向GPU Fence不变。
50ms内未确认则返回IO_TIMEOUT，保留pending状态，不能伪造完成。

实际参考微软[SetVidPnSourceAddress调用契约](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/nc-d3dkmddi-dxgkddi_setvidpnsourceaddress)
及[其参数中的模式切换/ContextCount说明](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/ns-d3dkmddi-_dxgkarg_setvidpnsourceaddress)，
结合当前代码没有独立模式切换锁存等待的事实。这里加强的是旧扫描面可安全退休的
实现条件，**不声称微软页面明确要求所有模式切换必须采用这一等待算法**，也尚未
证实它就是两次实机重启的根因。

固定609ce9d9…源码只替换panel_scanout.c及其控制测试；完整主机构建通过。
同一组新测试在Switch上链接旧实现退出226（成功返回后仍有pending），链接新实现
退出0。覆盖反复0/90/0/90/0、上一笔MPO仍pending、错误IRQL、无锁存超时和后续恢复；
没有执行GPU。第一轮新控制曾因测试读取未模拟的全局active镜像退出228，已改为读取
模型实际维护的REG_ACT_CONTROL寄存器；该工具问题与GPU失败分别记录。
源码/构建/控制位于 `rotation-modeset-latch-candidate/`；后续安装及实屏失败如下。

## 模式切换锁存候选的黑屏结果（modeset-e）

`7880abcd563b93a2d05699c7931761a7336f27ef` 已推送。签名SYS SHA256：
`7e9c1984072a6abd057841c37dd104bc745c84bd725c6332b1b401974bfd616f`；
UMD仍34e35bcf…、ICD仍dee3d580…，相对apply90d只改变KMD模式切换锁存路径。
归档SHA256为 `cae8950894f53a1ad5aa7b3ce8ad180dc6865f7296fa2ab0468573a909fa1321`，
Windows签名13项及包内70个文件校验通过。05:44:30请求一次安装所需重启，
05:44:57启动；05:49:46原生启用完成。05:51:13核对Code0，DWM PID828实际加载
34e35bcf…/dee3d580…；仍gpu-bridge、logon-default，CPU1497600kHz/1120mV、
GPU460800kHz/800000µV、RAM1600000kHz、风扇驱动13。

05:51:55临时切90°返回0，05:51:57–05:52:30的逻辑横屏图案完成，5020个RGB像素
容差0、差异0。但用户随后明确反馈：**没有横屏显示成功，屏幕是黑的，约30秒后恢复
竖屏显示。** 因此本轮为**实屏黑屏失败**，不得把逻辑截图、Code0或模式API成功
写成硬件旋转通过。05:53:04读回仍为05:44:57启动，DWM PID828和Explorer PID5188
及其启动时间均未改变；本轮没有系统或这两个桌面进程重启的证据。

同轮窗口控制另有独立工具错误：控制器从logon-default安装目录寻找未打包的
`Exercise-DesktopWindow.ps1`，PowerShell在运行脚本前退出-196608，未开始60秒开窗。
控制器05:52:32恢复原720×1280/identity，05:52:35结束，后备恢复任务已撤销。
已准备工具修正：从独立试验目录使用现有Exercise脚本，并在模式切换前检查脚本
存在及语法。保留实际执行版本和修正diff；**未因此重测已黑屏的组合**。
第二次横屏、五分钟交互和vkcube均未执行。

收到实屏失败后停止扩大测试，05:54:31发出一次关闭门控后的正常恢复重启；
05:55:08启动，05:56:33确认Code43、StartGateStatus=c00000a3、硬件尚未初始化。
恢复目标为59dd5ea7…/14d51a55…/dee3d580…及原方向，保留双向Fence和当前频率。
05:58:45恢复完成，Code0、gpu-bridge、logon-default。05:59:39核对实际SYS及
DWM PID788加载模块均为上述恢复组合，CPU/GPU/RAM/风扇设置与试验前一致。
06:00:38在交互Default桌面只读校验原快照通过，当前模式和注册表均为
720×1280/identity/60Hz；未再次应用模式。临时旋转及后备恢复任务已撤销，
只读校验任务完成后也已清理。本次恢复未索要人工确认，不另宣称实屏验收。
原始轮次状态位于 `rotation-bidirectional-20261113/modeset-sequence-status-03.json`，
用户实屏反馈及工具修正位于 `rotation-modeset-latch-candidate/`。

继续核对微软旋转、[DXGI Blt](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/dxgiddi/ns-dxgiddi-dxgi_ddi_arg_blt)
与[MPO attributes3](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/ns-d3dkmddi-_dxgk_multiplane_overlay_attributes3)
契约和固定Switchroot的window.c（查阅2026-11-14 UTC）：
现有源码仍缺旋转Blt回退，DC列扫描与pitch/offset需要对照实际绑定的primary确认。
这些是尚未排除的路径，不能将缺失Blt、Fence、供电或DC带宽直接写作本轮根因。
NVIDIA TRM下载入口本次要求登录，未取得正文，不声称已据TRM验证硬件限制。

## 源可见性与主表面绑定修正

继续静态审查发现一处可独立复现的显示契约错误：`SetVidPnSourceVisibility(FALSE)`
会将当前主表面替换成POST启动缓冲区；随后TRUE只显示POST，原地址和旋转布局已经
丢失。另一方面，legacy/MPO更新主表面都可能在源仍隐藏时重新开启Window A。
MPO禁用primary plane且不提供allocation时也会将地址替换为POST。

微软[源可见性DDI](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/nc-d3dkmddi-dxgkddi_setvidpnsourcevisibility)
及[参数契约](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/ns-d3dkmddi-_dxgkarg_setvidpnsourcevisibility)
要求开始/停止扫描该源的primary，隐藏期间仍生成VSync；
[MPO呈现契约](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/multiplane-overlay-vidpn-presentation)
要求源隐藏时关闭所有plane，重新显示时只开启primary plane。查阅2026-11-14 UTC，
控制使用WDK/SDK10.0.26100.0。

修正把源可见性独立保存于显示中断同步域：关闭Window A而保留地址/rotation/pitch；
源隐藏时，新的legacy/MPO绑定仍保持不扫描；重新显示恢复最近一次已绑定的primary。
MPO禁用只改enable状态，不凭空选择POST。停止设备或明确的无primary模式切换仍走
原POST恢复路径。仅保留硬件地址/布局状态，不增加allocation对象或驻留引用；
backing变化仍由VidMm通过地址DDI通知。普通翻转、双向GPU Fence及供电参数未改。

完整主机构建通过。Switch上的同一组ARM64 CPU控制链接旧实现退出107（隐藏后活动
地址/布局不是原primary），链接新实现退出0。覆盖隐藏/显示、隐藏期间换绑、MPO禁用、
隐藏时VSync、锁存超时不冒充完成、重复旋转和POST恢复。首次控制runner曾得到null
退出码，该条不作控制证据；改用直接持有Process句柄的runner后取得上述明确退出码。
这证明修正了源码契约错误，**尚不能证明它是实屏黑屏根因或硬件旋转已通过**。
候选位于 `rotation-source-visibility-candidate/`，此时尚未安装。

安装前继续核对完成通知：仅修改Window A可见性后，原读回代码会把自己写入的状态
当作外部状态，将已锁存的PresentId清零。补充修正把同一binding及原PresentId作为
可见性更新的pending状态，仍等硬件锁存后确认，不创建新的完成值。新增“隐藏和
显示后VSync仍报告原已锁存PresentId”控制：链接首版可见性修正退出214，补充后
退出0；完整主机构建通过。首版签名包未安装，后续使用
`rotation-source-visibility-v2/`，避免把首版的控制结果当成最终候选验证。

按用户配置尝试VPS Claude独立只读复核，返回“Not logged in”；未产生审计意见，
未修改远端登录或模型配置。上述结论来自本窗口的源码、微软契约和控制结果。

## source-vis-f：仍黑屏、二次旋转后重启，按用户要求保持现场

最终候选包含 `6461cb15713816d2b2a7bf32a8ea374f200ae6f5` 和
`dcd1c64e2b0efb4edd1179cc804407cd45d68b23`，均已推送。SYS为cb0aef82…，
相对modeset-e保持UMD34e35bcf…/ICDdee3d580…不变。包SHA256：
`f35244d1f90855d42d2902383528ca1e13a303afe50ce0d39f8a3d9d55f34f66`，
70个文件及13项Windows签名校验通过。RAM的本轮before/after均固定为1600MHz，
不会将此前2100MHz的旧备份作为本轮恢复目标。

06:20:15为安装请求一次正常门控重启；06:20:43启动。06:24:41启用结束，
06:25:27核对Code0和DWM实际模块哈希，CPU/GPU/RAM/风扇设置均保持。F轮原生LUID
`00000000000d7353`，Win11 22631.2861，开始时电源API为Online。

* 06:26:25第一次临时90°，06:26:26–06:27:00逻辑图案5020像素差异0。
* 06:27:07–06:28:08窗口自动控制运行60.46秒，11次移动、11次缩放、退出0。
* 06:28:09脚本恢复竖屏，06:28:12再次临时90°；最后落盘阶段ApplyingLandscape2。
* 06:28:33发生新的系统启动，用户同时报告横屏没有上屏及自动重启。**本轮实屏黑屏、
  稳定性失败**；逻辑图案和窗口控制不能抵销失败。五分钟交互未执行。

旋转控制脚本不含系统重启命令，但确实自动进入了第二次旋转。用户要求保持现场后，
第一条停止请求在SSH连接阶段超时，没有送达；重连后06:29:53已禁用测试和定时恢复。
没有请求回退或再次系统重启。原有登录启用任务在新启动中自行完成，06:31:10读回
上述候选Code0。任务调度器此时给旧测试任务显示LastResult=0，也不能视为测试通过：
系统启动时间已改变，原控制器没有完成记录，且用户实屏明确失败。
原始材料位于 `rotation-source-visibility-v2/` 与
`rotation-bidirectional-20261113/source-vis-hold-status-02.json`。

## Switchroot及微软版本对应检查（现场保持期间）

固定Switchroot `3c40adf1081096bc61a7c1280434c163e7e79b48` 的
[t21x带宽实现](https://gitlab.com/switchroot/kernel/l4t-kernel-nvidia/-/blob/3c40adf1081096bc61a7c1280434c163e7e79b48/drivers/video/tegra/dc/t21x/bandwidth.c)
会按布局、格式和旋转计算缓冲/延迟条件，并结合EMC与latency allowance；
dc.c处理Window A underflow计数。当前KMD只处理显示VBlank，尚没有对应underflow
观测与LA验证。它是明确实现差异，不是已证实黑屏根因；固定源中pitch布局也有独立
分支，不能据此断言pitch-linear硬件旋转不支持，更不能直接加频率或电压。

Hekate固定 `8e1392df4569ac501a115867056b13c361429e9e` 的
[di.inl](https://github.com/CTCaer/hekate/blob/8e1392df4569ac501a115867056b13c361429e9e/bdk/display/di.inl)
包含block-linear列扫描配置，而当前主表面是pitch-linear。其地址偏移、block height
不能直接移植。继续扫描该固定提交的C调用者，block helper只有定义/声明；
`nyx/nyx_gui/frontend/gui.c::_nyx_disp_init` 实际使用1280×720 X8R8G8B8、
VIC_ROTATION_270，等待VIC合成后调用pitch_vic配置；刷新路径也调用vic_compose。
因此该版本Nyx是VIC预旋转后竖屏pitch扫描，不能当作本驱动DC列扫描已验证的旁证，
也不证明Windows必须使用VIC。具体来源为
[固定Nyx调用路径](https://github.com/CTCaer/hekate/blob/8e1392df4569ac501a115867056b13c361429e9e/nyx/nyx_gui/frontend/gui.c)。
历史提交 [833f060c](https://github.com/CTCaer/hekate/commit/833f060c7b0860b8dc7832b071c4dc55188f833d)
（2022-10-11）说明改用VIC是为了改善T210B01的瀑布式缓慢绘制。其diff将CPU写入旋转
framebuffer改为中间横屏buffer加VIC合成，并不是从本驱动这条DC pitch列扫描路径迁出。
因此该记录可支持检查扫描效率，不能直接证明当前DC列扫描不受支持或黑屏根因已找到。
上述源码、完整归档调用者扫描结果及SHA256保存于
`rotation-black-static-review/references/`，未复制Linux实现进MIT驱动。

实机是22631.2861，不是编译SDK的26100。已核对本机WDK26100头文件及源码：
`build.py`选择WDDM2.6、`query.c`报告2.6，注册legacy source-address和MPO3回调。
[MPO3文档](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/nc-d3dkmddi-dxgkddi_setvidpnsourceaddresswithmultiplaneoverlay3)
的中断级/PrePresentNeeded要求仍适用于这条接口；新版页面提到的hardware flip queue
不能直接套用到本驱动。26100头文件的TargetFlipTime等成员受WDDM2.9条件保护，
本轮2.6构建不包含它们。这里只证明注册与ABI选择，没有尚未捕获的真实回调路径证据。

另有文档完整性差异：Learn的DescribeAllocation页面将Flags/Rotation称为reserved，
但本机26100头文件明确注释CheckDisplayMode用于显示模式查询、Rotation为显示模式
输出。不能只据网页把这些字段清零。还核对了winerror.h：
`DXGI_DDI_ERR_UNSUPPORTED=0x887B0002` 与 `DXGI_ERROR_UNSUPPORTED=0x887A0004`
不是别名；primary回退契约必须按实际返回码及NO_SCANOUT标志审核，不能仅凭名称相近
认定已经进入旋转Blt回退。本轮尚未取得真实失败调用参数，仍不将此项作为黑屏根因。

Gemini审计目录为 `delegation-rotation-carbon/`，目标为报告和统一diff，使用
`gemini-4-carbon-high`及本会话自动工具许可。CLI提示auto执行模式未开放，已回到默认
执行模式；自动工具许可仍明确生效，静默认证成功。当前审计在途，尚无可验收结论。

## 用户要求补查winx与Ubuntu Jammy/Noble（2026-11-14 UTC）

先前只检查了固定3c40adf1的通用DC代码，没有追到Ubuntu发行维护者的面板旋转适配。
该固定提交属于Switchroot的lineage-19.1分支背景，不能代替Jammy/Noble实现。
[Noble官方指南的Sources](https://wiki.switchroot.org/wiki/linux/l4t-ubuntu-noble-installation-guide)
链接到theofficialgman的内核构建脚本；固定脚本
`1fb0e92e10eaf452b71cbdf7fc7b06c27f6de121`克隆其`switch-l4t-kernel-nvidia/linux-dev`。
本次将该分支固定为`7d95822acda1f6dab3f3d1d099b43ff9e0d0e626`后核对，未将滚动分支
HEAD冒充所有已发布镜像的二进制来源。

* [官方问题说明](https://wiki.switchroot.org/wiki/common-issues)明确：Jammy/Noble通过
  Tegra DC接口的内核硬件旋转绕过NVIDIA Xorg用户态旋转的帧节拍问题。
* 维护者[ext/dev.c](https://github.com/theofficialgman/switch-l4t-kernel-nvidia/blob/7d95822acda1f6dab3f3d1d099b43ff9e0d0e626/drivers/video/tegra/dc/ext/dev.c)
  的`tegra_dc_rotation_to_win_flags`把90映射为SCAN_COLUMN+INVERT_H、270映射为
  SCAN_COLUMN+INVERT_V；set_windowattr按物理面板方向覆盖生产者的这些标志，必要时
  将输出宽高换回物理竖屏。fb.c同时转换逻辑模式宽高和blanking。主画面没有因此
  增加CPU整帧旋转或3D shader预旋转；cursor.c对小型cursor位图另做CPU旋转。
* 另查的l4t-community `linux-6.0.0`固定54e79a48采用组合生产者/面板变换，其行为与
  上述维护者分支的强制覆盖不同，不能混写成同一个实现。Windows显式MPO变换仍须按
  自身契约转换，不能把Linux固定面板角度再叠加一遍。
* winx本地固定`e3b30076a4c9a66e2ddb1e8deaffe0101d88ef06`中的KMDOD改编使用CPU
  BltBits/GetPitches/GetRowStart，把像素写进原生framebuffer；SWITCH_HEKATE配置为
  0xF5A00000、720×1280、pitch2880。SimpleFbDxe在启动OS loader前交回原生模式。
  该fork README记录其Lite自定义display-only安装曾撤回，不能仅据源码存在就认定
  用户此前正常的软件横屏一定由这份SampleDisplay实现；用户实际正常横屏作为对照
  保留，代码用于像素地址/可见性契约参考。

对应来源、SHA256及只读副本保存在`rotation-black-static-review/references/`，未复制
Linux GPL实现进本驱动。曾考虑新增DC查询接口，用户要求先对照成功实现后已停止该
方向，本轮没有新增诊断接口。

## 源可见性与目标激活分离：仅本地修正

winx/KMDOD在模式切换中保留SourceNotVisible，Present另检查目标/监视器状态。
当前KMD却让CommitVidPn和SetTimingsFromVidPn调用同一个SetVisibility，直接覆盖OS
最近一次SourceVisible请求。可复现的问题是：先隐藏源，再启用目标/提交模式，源被
意外显示；反过来，源启用也会覆盖目标停用状态。这是源码契约混用，不是已证明的
本轮实屏黑屏根因。

本次只增加TargetActive独立状态：源可见性DDI只改SourceVisible，模式/目标激活只改
TargetActive，所有新flip和显示恢复使用两者的有效组合。继续保留primary地址、布局、
PresentId和真实锁存等待；不改变旋转方向、Fence、UMD、ICD或功耗设置。

x64 real-WDK/Wine控制退出0，覆盖两种回调顺序、隐藏源中的模式切换、停用目标期间
的MPO更新及随后恢复。同一个新时序控制链接旧panel_vidpn实现退出63，对应第319行
“提交模式不能取消源隐藏”断言；新实现退出0。冻结cb0aef82源码只叠加本项6文件后，
完整host控制和ARM64 KMD/控制程序构建退出0。结果在`rotation-target-gate/`。
**没有上机、没有新实屏验收；设备仍为用户要求保持的cb0aef82候选。**

Gemini初次审计于07:11:29主动中断，以在原会话补入用户的“正确逻辑截图/物理黑屏”
及winx对照要求；不是因SSH或观察超时判定终止。07:11:51同一Carbon High会话接续，
自动工具许可仍为always-proceed。Noble源码已复制到审计参考目录，后续追加说明待
当前审计回合结束后发送；尚未把未收到的报告或diff记作验收完成。

## Jammy/Noble硬件配套对照（2026-11-14 UTC）

按用户要求继续检查旋转以外、与本轮上屏直接相关的硬件处理，仍固定维护者内核
`7d95822acda1f6dab3f3d1d099b43ff9e0d0e626`。新增只读参考文件
`dc/t21x/bandwidth.c` SHA256为
`e39db1ed567748e1f0196254f395be81c6e60c14486f91ddde4bea6ad5e988cd`，
`dc/dc.c`为`6ff3394760062fa9f7aaa647d27b9e90fb1fc38d71e9cc3243d1d2d3d08b2ac8`。
完整来源和文件副本保存在`rotation-black-static-review/references/noble-maintainer-dc/`。

| 环节 | 固定Ubuntu实现 | 当前Windows路径及采用判断 |
|---|---|---|
| 显示取数带宽/延迟 | `t21x/bandwidth.c::calc_disp_params`按pitch/block、列扫描、像素格式和缩放计算缓冲条件；`tegra_dc_handle_latency_allowance`同时设置LA和EMC下限；切换期间取新旧需求较大者 | 当前DC路径继承启动配置，没有对应按扫描布局更新LA的实现。RAM固定1600MHz不等于取数延迟已满足。这是待核对的硬件差异，不能直接推出当前黑屏是带宽不足，也不据此提高频率、电压或照搬Linux DVFS |
| 几何与锁存 | `window.c::update_is_hsync_safe`仅允许少量不改布局的更新走HBlank；旋转/布局变化强制VBlank，随后检查ACT_REQ清除 | 本驱动也限制immediate为相同旋转/pitch/offset/可见性，布局变更走VBlank；模式切换等待active地址/布局匹配。尚未发现该原则缺失。维护者HEAD撤回过“所有列扫描一律VBlank”的试验，不能把被撤回代码当作现行Ubuntu要求 |
| 取数FIFO | `window.c`在T21x提交窗口UPDATE之前写MEMFETCH_RESET，注释说明用于避免复位后FIFO取到旧数据 | 当前驱动接管正在运行的Hekate显示，不主动复位DC，也没有这项写入。需核对该寄存器对T210B01、继承状态与布局切换的实际适用条件，不能仅凭两份源码不同就新增复位 |
| 显示欠载 | `dc.c`分别统计各窗口underflow，暂时屏蔽中断后由worker清除并重新开放 | 当前只处理VBlank，缺少相同的underflow处理。固定源码中T21x handler实际是计数/清除；附近“连续四帧后reset”的旧注释没有对应复位实现，不能据注释声称Ubuntu会重启DC，也不据此新增整机重启/转储流程 |
| backing退休 | `ext/dev.c`更新窗口并同步后才解除上一批front buffer的DMA映射和引用 | Windows由VidMm管理驻留，KMD用真实锁存及VSync完成回执交还旧primary；复用生命周期原则，不移植Linux dma-buf/syncpoint私有ABI |

参考：[窗口编程](https://github.com/theofficialgman/switch-l4t-kernel-nvidia/blob/7d95822acda1f6dab3f3d1d099b43ff9e0d0e626/drivers/video/tegra/dc/window.c)、
[T21x带宽](https://github.com/theofficialgman/switch-l4t-kernel-nvidia/blob/7d95822acda1f6dab3f3d1d099b43ff9e0d0e626/drivers/video/tegra/dc/t21x/bandwidth.c)、
[显示中断](https://github.com/theofficialgman/switch-l4t-kernel-nvidia/blob/7d95822acda1f6dab3f3d1d099b43ff9e0d0e626/drivers/video/tegra/dc/dc.c)。
本轮没有移植GPL代码，也没有更改平台功耗设置。

同时检查冻结ICD的最终线性主表面：`nvk_rendering_linear`要求地址与row stride均为
128字节对齐，且没有depth/stencil或非线性色附件。竖屏pitch2880余64，必须走tiled
shadow；横屏pitch5120余0，在其他条件满足时可直接走线性render target。因此旋转
会同时改变DC列扫描和潜在的渲染存储路径，不能将CopyFromScreen截图正确直接等同于
最终primary内存正确，也不能反过来认定线性渲染有错。shadow路径在LOAD时复制进入、
STORE时复制回去，使用NON_PIPELINED copy和flush；tgd3d在结束rendering后记录host
读依赖再提交，KMD提交尾部仍有WFI与SET_REFERENCE。未发现“完全没有回拷/提交完成
标记”的缺口；本轮没有最终scanout backing的实机像素读回，缓存正确性仍未验收。

本轮仅源码比较和记录，不新增驱动改动或重复构建。已通过控制与ARM64构建的
SourceVisible/TargetActive修正仍只在本地候选中；实机保持原cb0aef82组合，横屏实屏
仍为失败。下一次需要区分最终primary写入与DC扫描，而不是把逻辑截图再次记作上屏。

## SetTimings遗漏已提交方向：仅本地修正（2026-11-14 UTC）

继续核对微软实际DDI发现：本驱动的CommitVidPn保存PathRotation，而注册的新路径
SetTimingsFromVidPn在验证VidPN时丢弃方向输出，成功后仍保留旧方向。随后对同一新
路径调用UpdateActiveVidPnPresentPath会被误判为不支持的方向变化。微软
[SetTimings契约](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/nc-d3dkmddi-dxgkddi_settimingsfromvidpn)
明确该DDI替代CommitVidPn；所查页面更新于2021-10-21，接口适用于本轮WDDM2.6，
不是借用新版hardware flip queue字段。此为可复现的软件状态错误，尚没有证据证明
黑屏轮次实际走到这条失败序列。

修正只在SetTimings成功且实际附有VidPN路径时保存接受的方向；失败、停用路径或
PreserveInherited且未提供VidPN时保留原记录。它不改变扫描地址、旋转寄存器或当前
primary布局，也没有开启smooth rotation。测试覆盖四个方向、失败后保留旧方向、
继承场景、模式切换后同路径更新，以及未提交的新方向仍被拒绝。

使用最终同一份真实WDK控制测试，链接修改前panel_vidpn对象退出235，精确对应
第235行“SetTimings后更新同一条路径应成功”；链接修正后对象退出0。完整host控制
及ARM64驱动/控制程序构建退出0，结果在`rotation-timings-state/`。构建来源为前一
TargetActive冻结候选再叠加本项源码/测试，两个修正保留独立提交。未签名SYS SHA256：
`058a07555b08a75ff80d4ec660240ee80ef14a58d0d09352297b06a3e3019ac0`。
**未安装、未启用、未重启；实机仍保持cb0aef82，物理横屏未通过。**

## 组合B之后：Jammy/Noble最终上屏链路复核（2026-11-14 UTC）

本节以顶部组合B（69aa6036…）为当前基线；上一节“未安装”是部署前的历史记录。
用户要求继续对照Jammy/Noble旋转后实际送到面板的路径。本轮只审查源码与已有G轮
结果，没有再次切换方向或安装驱动。G轮实屏仍为黑屏失败，竖屏已恢复。

参考来源仍为官方Noble指南链接的构建脚本`1fb0e92e…`和维护者内核
`7d95822acda1f6dab3f3d1d099b43ff9e0d0e626`。这是固定的公开源码参照，不是对每个
Jammy/Noble发布镜像内核二进制的哈希认证。官方[问题说明](https://wiki.switchroot.org/wiki/common-issues)
确认这两个发行版采用Tegra DC内核硬件旋转，绕过NVIDIA Xorg旋转的帧节拍问题。

### 正常提交链与当前路径的对应关系

| 环节 | Ubuntu固定源码 | 本基线及结论 |
|---|---|---|
| 确定实际生产者缓冲区 | `ext/dev.c::tegra_dc_ext_pin_windows`固定dma-buf映射；`tegra_dc_ext_set_windowattr`使用映射地址加surface offset，并保留生产者提供的stride、像素格式和布局标志 | Windows使用runtime allocation、VidMm驻留和`TgGpuResolveScanoutAddress`。不能把Linux DMA地址或fd直接当Windows物理地址；也不能用逻辑桌面截图确认最终primary内容 |
| 等渲染完成 | `tegra_dc_ext_set_windowattr`在提交窗口前等待pre-syncpoint/fence | 本基线仍使用runtime↔NVK双向Fence，Present回调后由调度器触发DC翻转。Linux等待函数本身的返回值没有在此处完整传播，不能机械复制成正确的错误处理 |
| 变换与尺寸 | 该维护者分支按panel rotation替换producer旋转标志；90/270度将横屏out_w/out_h归一为物理竖屏输出。`window.c`分别设置source尺寸、output尺寸、pitch、kind、H/V offset及DDA | 当前KMD的90/270度列扫描位、起始偏移、固定物理输出及单位DDA计算与该路径一致。Windows MPO已经给出显式顺时针变换，不能再强加Linux面板角度 |
| 显示取数保障 | `window.c`更新动态带宽；T21x `bandwidth.c`在切换前使用新旧需求较大值，在同步后再应用新状态，并根据布局计算LA（内存访问延迟预算） | 当前DC继承Hekate启动配置，没有对应的显示LA更新。RAM1600MHz不是这项契约的替代；目前没有证据证明G轮发生underflow，因此不能直接认定这是黑屏根因 |
| 提交和锁存 | `_tegra_dc_program_windows`写assembly状态，经UPDATE复制到armed，再发ACT_REQ；布局变化强制VBlank，`sync_windows`等待窗口状态完成 | 本基线也将布局/可见性变化限定为VBlank，并读active地址、pitch和偏移确认。Linux每次包含GENERAL更新，当前仅改变counter选择时附带GENERAL；这是实现差异，尚无依据断言单独WIN_A激活一定错误 |
| 旧缓冲区退休 | `tegra_dc_ext_flip_worker`调用`sync_windows`后才解除前一批front buffer映射和引用 | 当前由真实锁存后的VSync/PresentId回执交还VidMm。应保留该完成边界，不能把UPDATE写入或逻辑截图成功视为显示完成 |

对32位、无缩放、无裁剪的全屏表面，按上述`window.c`公式展开后：

| DC字段 | 竖屏identity | 横屏CCW90 | 横屏CCW270 |
|---|---|---|---|
| producer宽×高 | 720×1280 | 1280×720 | 1280×720 |
| pitch（字节，线性布局时） | 2880 | 5120 | 5120 |
| WIN_OPTIONS（已使能） | `0x40000000` | `0x40000011` | `0x40000014` |
| H_OFFSET（字节） | 0 | 5119 | 0 |
| V_OFFSET（行） | 0 | 0 | 719 |
| WIN_SIZE（物理输出） | 720×1280 | 720×1280 | 720×1280 |
| PRESCALED（水平字节×垂直像素） | 2880×1280 | 2880×1280 | 2880×1280 |
| H/V DDA | 1.0 / 1.0 | 1.0 / 1.0 | 1.0 / 1.0 |

所以，横屏时保留720×1280的物理扫描及PRESCALED值本身不是错误；不能将这些寄存器
一并盲改为1280×720。当前`tg_t214_dc_get_layout`与该计算相符。这是源码核对结果，
不是DC实机寄存器已按此锁存的证明，也不证明pitch-linear列扫描的全部硬件约束。
Ubuntu的`SURFACE_KIND`来自实际producer布局：普通pitch、旧式tiled和block-linear
分开设置。Vulkan的`OPTIMAL`枚举不能代替具体的DC kind/block-height/pitch契约。

另两项硬件差异需准确描述：T21x的`MEMFETCH_RESET`写入在每次窗口编程路径执行，
没有“刚发生DC复位”条件；注释中的用途不能改写成执行条件。当前基线未写该寄存器。
Ubuntu还处理窗口underflow中断与计数；当前只处理VBlank。两项都尚未证明是G轮根因，
不据此盲目复位DC或更改CPU/GPU/RAM、电压及Fence。NVIDIA在线X1 TRM下载本轮跳转
登录页，没有取得其具体约束；公开Jetson `nvimp_util`章节的TX2/Xavier参数也不能
代替T210B01显示配置。

源码：[窗口编程与锁存](https://github.com/theofficialgman/switch-l4t-kernel-nvidia/blob/7d95822acda1f6dab3f3d1d099b43ff9e0d0e626/drivers/video/tegra/dc/window.c)、
[buffer提交与退休](https://github.com/theofficialgman/switch-l4t-kernel-nvidia/blob/7d95822acda1f6dab3f3d1d099b43ff9e0d0e626/drivers/video/tegra/dc/ext/dev.c)、
[T21x带宽与LA](https://github.com/theofficialgman/switch-l4t-kernel-nvidia/blob/7d95822acda1f6dab3f3d1d099b43ff9e0d0e626/drivers/video/tegra/dc/t21x/bandwidth.c)。

### 独立审计的验收限制

Carbon High首版报告与diff已于08:38:41UTC完成；父任务复核后未采纳其diff，08:41:48
在同一Antigravity CLI会话发回修订要求。首版把逻辑截图的具体内部资源、shadow尺寸
及dxgkrnl可能合成的MPO参数推断成了事实。实际G轮CCD source与target均为720×1280，
逻辑桌面才是1280×720；[微软shadow契约](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmdt/ns-d3dkmdt-_d3dkmdt_shadowsurfacedata)
规定shadow与shared primary同尺寸、同格式，不能据此断言它必须是横屏shadow。

首版diff将primary拒绝改成`DXGI_DDI_ERR_UNSUPPORTED`，却没有实现该选择要求的
[旋转copy回退](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/dxgiddi/ns-dxgiddi-dxgi1_2_ddi_base_functions)。
不能将这样不完整的回退引入当前基线。旋转Blt未实现是功能缺口，但G轮是否进入该路径
仍未知；CW的MPO与CCW的VidPN使用不同映射也不构成已证明的冲突。

下一项修复应区分最终primary没有有效提交/写入，与DC已经收到正确primary但未能取数。
现有5020像素逻辑截图不能替代这一边界。当前没有新的、足以解释黑屏的已证实硬件
补丁，因此本轮没有重复构建或再次部署同一失败组合；基线继续保留组合B。

## 最终scanout边界：按需只读快照候选（2026-11-14 UTC）

08:46:17重新读取实机：仍为69aa6036…/Code0，启动时间08:06:32、DWM768及其启动时间
未变，CPU1497600、GPU460800、RAM1600000、风扇13、默认登录任务Ready。现有KMD没有
读取DC active bank和最终primary像素的接口，新增一次性`D3DKMT_ESCAPE_DRIVERPRIVATE`
快照；不新增ETW、dump或每帧日志，不修改旋转/提交/功耗策略。

该调用要求HardwareAccess第二级串行化与SeDebugPrivilege，只接受固定192字节协议。
在显示中断同步内读取固定的DC地址、布局、可见性、ACT_REQ、underflow及已有PresentId，
恢复原READ_MUX；不清中断、不推进Pending、不写UPDATE/ACT_REQ。仅当无待锁存状态、
布局已识别、整个表面位于驱动仍持有的scanout segment内时，读取四个固定四分区中心
像素。不接受用户提供的寄存器、物理地址或偏移，不映射任意内存。像素采样不是GPU
完成栅栏，不能据此单独标记实屏通过。

真实WDK/x64控制通过权限、协议长度、active/assembly区分、未锁存拒绝像素采样、
segment上下界及停止状态检查，并确认只临时更改READ_MUX、不消费中断或Present回执。
冻结组合B加本项的完整ARM64构建退出0；ARM64/x64读取工具构建通过。未签名SYS
`ac66d330fd0399b6249411b315484c3c5e7d71490e315bbfae148e13adb8519c`，签名SYS
`1a86a02b479b716b2b695ade0bd6860076d4d5f61a29d95b804cb655ce0bf164`。
产物在OUT的`rotation-scanout-boundary/`，截至本条仅准备CAT与原生控制，尚未安装。
后续候选以组合B为恢复输入；UMD/ICD和双向Fence保持。

## H轮：显示源关闭，尚未进入DC列扫描（2026-11-14 UTC）

只读候选及13个包文件的Windows原生签名校验通过，ARM64控制通过。旧B内核对新查询
返回STATUS_NOT_SUPPORTED（预期），证明客户端没有把不支持冒充有效读回。08:56:56
发出一次正常门控重启，随后安装读取版；09:00:48启用Code0。没有重复发出重启。

09:03:48逻辑切至1280×720、path_rotation=2；逻辑图案5020个RGB像素容差0、差异0，
用户反馈仍未上屏。09:03:54.532、55.954、57.313三次真实读回一致：

* SourceVisible=0、TargetActive=1、WIN_OPTIONS=0（窗口关闭，列扫描未启用）。
* pitch2880、identity、720×1280，active地址`0xfbef3000`，Pending为空，PresentId停在329。
* 四个固定像素仍为旧竖屏桌面`ffc0d6e1/ff2194fe/ff0a3fc9/ff112d98`，不是四色图案。
* ACT_REQ前后均0；underflow始终4，与切换前相同，没有观察到本次新增欠载。

09:04:25自动恢复竖屏。09:09读回SourceVisible=1、WIN_ENABLE=1、PresentId416，
Code0及DWM832/Explorer5196启动时间均保持；已删除结束的安装/测试任务。H轮为
**物理横屏失败**，软件退出0只表示控制流程完成。本轮电源状态记录为Offline。
结果在OUT的`rotation-scanout-boundary/evidence/`、`physical-result.json`和
`after-restore.json`。这里只证明三个采样时点未进入列扫描，不用历史欠载或逻辑截图
推出GPU/DC整体正确；下一步优先检查primary创建和模式切换完成到显示源重新启用。
不能强行绕过Windows的Visible=FALSE来掩盖这条未完成的路径。

用户明确要求直接横屏、不要重复竖屏开窗。收到跳过请求时H控制器已在LandscapePattern，
因此没有另起一轮或重复切换；后续横屏轮只做必要状态读回，不再机械重复竖屏静置/开窗。
该偏好已计入AGENTS.md。

另发现独立的UMD输出契约遗漏：成功创建可扫描primary后未填写DriverFlags。
预置NO_SCANOUT的真实DDI控制在旧冻结实现退出1，修正成功路径输出为0后退出0；覆盖
mandatory/optional、四方向、ARGB/XRGB，同时保留原有OOM/设备移除及资源清理检查。
实机候选按原9de42c82+7f5f416b+42ad5d1b+a537121f+cffa4713冻结配方重建，只改
resource.cpp和对应控制；没有带入shader-cache分支。ARM64候选UMD SHA256为
`cefc41cffaed8dd4005ad820c27369ad0f66dbce72bdf228a1e5c790d94f864c`，尚未部署。
该缺陷是否H轮根因仍待直接横屏验证，不主动启用未实现的DXGI旋转copy回退。

## 用户补充屏幕信息与本轮停止条件（2026-11-14 UTC）

用户确认这台Lite改装了第三方OLED；HOS、Linux、Android横屏正常（用户报告）。后续
应以这块实际改装屏在Jammy上的运行配置核对像素时钟、DSI与DC时序、porch及扫描布局，
不能把原装Lite面板的假设直接当作该屏验收依据。H轮三个采样仍显示源关闭、列扫描未启用，
现有证据尚不能将Windows黑屏归因于时序。

用户明确要求：完成54dc5597的本轮直接横屏测试后，若仍不上屏就停止，等待其安排启动
Ubuntu Jammy并提供SSH连接。该条件满足后，只完成必要恢复、记录和清理，不再追加
候选或测试；暂停持续目标，后续由用户恢复。记录本条时正在进行该单项UMD修正的安装准备，
不改变KMD/ICD、双向Fence或频率设置。

## I轮最终结果：DriverFlags修正未解决横屏黑屏（2026-11-14 UTC）

09:26:04安装完成，核对DWM788实际加载新目录中的UMD
`cefc41cffaed8dd4005ad820c27369ad0f66dbce72bdf228a1e5c790d94f864c`。
KMD保持H轮`1a86a02b479b716b2b695ade0bd6860076d4d5f61a29d95b804cb655ce0bf164`，
ICD保持`dee3d5805af80696f87d094ec1c5f557d662f435336207992baf588e2dc1cee0`。
安装目录为`C:\ProgramData\SwitchWOA\GpuUpdate\tgd3d-54dc5597-20261114-172511`。
本轮仅在09:20:11为清除旧UMD上下文执行过一次计划重启，随后没有重复重启或重装。

09:31:22直接切至1280×720横屏，没有重复竖屏静置/开窗。用户反馈“黑屏状态”，因此
**物理横屏失败**。三次09:31:28.556/29.916/31.291读回仍是SourceVisible=0、
WIN_OPTIONS=0、pitch2880、identity、720×1280，地址`0xfbb88000`、PresentId79不变，
Pending为空、ACT_REQ为0；四个采样像素全0。切换前/横屏/恢复后的underflow均为6。
逻辑图案5020像素差异0，只证明软件捕获流程，不改变失败结论。本轮电源Online，
原生LUID`00000000000ce280`，Windows22631.2861。

09:31:58恢复竖屏；09:32:50核对Code0、SourceVisible=1、WIN_ENABLE=1、PresentId98，
默认登录启用任务Ready，DWM788/Explorer5164及启动时间保持。安装/测试任务已移除，
恢复定时器已解除。未把I候选提升为选定基线，也未为移除读取代码再重启设备。
完整结果在OUT的`primary-scanout-flags-frozen/evidence/`、`physical-result.json`及
`after-restore.json`。软件结果退出0早于用户反馈，物理结果单独明确记为失败。

Carbon High在09:31:03结束H轮补充审查，报告v3、diff为空。其“从未有横屏flip”与
“严格只剩三个原因”的措辞超过离散采样证据；保留为候选方向，不将其视为完整因果证明。
没有继续委派、应用新diff或追加Windows候选；用户已暂停持续目标。

## Noble实际显示数据与第三方OLED对照（2026-11-14）

用户随后提供Ubuntu SSH并要求“获取数据看看是不是因为第三方oled”。本次仅使用
既有sysfs/debugfs读取接口、xrandr查询及现有启动信息；未改Linux方向、时序、驱动、
时钟、电源或面板命令，未重启，也未安装工具。数据保存在OUT的
`noble-oled-reference-20261114/`：`inventory.json`、`capture.json`、`link-capture.json`、
`comparison.json`及展开后的DC/DSI/xrandr/时钟文本。采集程序只保存在Mac，未上传执行文件。

实际系统是Ubuntu24.04.3、Linux4.9.140-l4t，构建标记
`#5 SMP PREEMPT Thu Sep 26 11:18:43 EDT 2024`；BSP标记R32.3.1。
它不是已核对的上游7d95822a源码版本的二进制认证。启动ID
`d6ee9e10-e244-4c71-9b0d-5df61a3ad8cf`在读取期间保持。设备树为Nintendo Switch Lite，
active-panel引用`panel-i-720p-5-5`（phandle219）。该节点是软件选择，不能据此识别
第三方OLED的真实控制板厂商、型号或保护阈值。

| 项目 | Noble实际读回 | Windows I轮或对应KMD契约 |
|---|---|---|
| 逻辑桌面 | 1280×720，Xrandr `left` | 1280×720，orientation1 |
| 面板有效/总尺寸 | DC active720×1280，总1000×1300 | 接管时严格检查同一active/porch，声明总1000×1300 |
| H同步/后沿/前沿 | 72 / 72 / 136 | 接管常量相同，旋转函数不改这些寄存器 |
| V同步/后沿/前沿 | 1 / 9 / 10 | 接管常量相同，旋转函数不改这些寄存器 |
| 显示窗口 | WIN_OPTIONS=`0x40010000`，WIN_ENABLE=1 | WIN_OPTIONS=0，SourceVisible=0 |
| DC旋转/步长 | SCAN_COLUMN=0、H/V反向位0，pitch2880 | 三次均SCAN_COLUMN=0、pitch2880 |
| 地址/布局 | WindowA `0xff400000`，输出720×1280，H/V offset0 | `0xfbb88000`，输出720×1280，H/V offset0 |
| DSI/时钟 | DSI视频通路启用；4 lanes；disp1/DSIA234MHz，DC分频3 | 本轮无完整DSI/PLL读回，不能宣称逐寄存器一致 |

**需要纠正此前从上游推断这台机器使用DC硬件旋转的说法。** 这份实际Noble虽有
`panel_rotation=270`，但两次active-bank读回的SCAN_COLUMN及反向位均为0；WindowA
已启用、B/C关闭。正常横屏图像在进入DC扫描前已经完成方向变换，不能把它当成
当前Windows拟采用的DC列扫描路径的实机通过样本。具体预旋转由GPU/VIC/其他上游
环节执行，本次寄存器读取没有单独区分；不称其为CPU旋转。

时钟须区分三个来源：设备树clock-frequency为78000000，Xrandr modeline为78.003MHz，
debugfs `mode.pclk`却为3333000。现有启动日志明确记录
`nominal-pclk:3333000 parent:234000000 div:3.0 pclk:78000000`；时钟树及
`DC_DISP_DISP_CLOCK_CONTROL=4`与此一致。因此按分频推导像素时钟为78MHz、配置刷新率
为60Hz，不能把3333000误读为正在以3.333MHz送屏。五次debugfs timestamp均返回
时间戳0、frame_no0，无可用VBlank节拍，故本次不声称完成物理60Hz时序测量。

DSI_CONTROL=`0x00103032`、STATUS=0、TO_TALLY=0、POWER_CONTROL=1；采样时没有置位
DSI FIFO溢出/欠载或超时计数。PHY_TIMING0/1/2为`06070603/040a0e05/00030172`，
BTA_TIMING=`00190a14`，SOL_DELAY=24，完整包序列、包长及原始寄存器已保存。
该T210B01的0x53–0x55是PAD_CONTROL5/6/7；旧debug输出把它们标作GANGED寄存器，
不能据`0x77777`误判双DSI拼接。参考头文件将B01 GANGED移到0x56–0x58。
Linux统计已启用，软件underflows=0，WindowA原始UFLOW在两次读取均为11；后者是
已有状态，不等于本次新发生11次故障。Flips completed由6625增至6667、skipped=0。
Noble读回EMC2500MHz，Windows配置1600MHz，二者不是相同内存频率的性能对照。

结论：**本次没有建立“第三方OLED自我保护导致Windows黑屏”的证据。** 相同行场
几何可在这块屏上工作，而Windows黑屏已有显示源关闭、最终primary交接未完成的直接
证据，应优先沿该边界定位。时钟源/DSI完整配置尚缺同轮Windows对照，也没有示波器
或逻辑分析仪测量，不能彻底排除电气时序差异，更不能认定微秒级阈值或面板保护机制。
本次数据采集结束；Windows补丁/测试仍按用户要求暂停。

本次实际参考[官方Noble指南](https://wiki.switchroot.org/wiki/linux/l4t-ubuntu-noble-installation-guide)
与维护者固定源码[DC读取接口](https://github.com/theofficialgman/switch-l4t-kernel-nvidia/blob/7d95822acda1f6dab3f3d1d099b43ff9e0d0e626/drivers/video/tegra/dc/dc.c)、
[DSI寄存器](https://github.com/theofficialgman/switch-l4t-kernel-nvidia/blob/7d95822acda1f6dab3f3d1d099b43ff9e0d0e626/drivers/video/tegra/dc/dsi_regs.h)、
[像素分频](https://github.com/theofficialgman/switch-l4t-kernel-nvidia/blob/7d95822acda1f6dab3f3d1d099b43ff9e0d0e626/drivers/video/tegra/dc/mode.c)。
源码仅用于接口/位域解释，实际状态取自本机读回；不将固定上游提交冒充运行内核哈希。


## J候选：tgd3d GPU预旋转（2026-11-14，未部署）

用户根据Noble对照明确取消DC列扫描路线，要求把逻辑1280×720转换成物理720×1280
再交给显示控制器。本地支线及main的`d3d10umd/DxgiFns.cpp::_Blt`均只是
`LOG_UNSUPPORTED_ENTRYPOINT()`后返回S_OK，没有可以原封不动移植的GPU旋转实现。
因此在tgd3d实现了实际Vulkan绘制拷贝；没有导入main整批代码。

实现：

- 删除KMD的列扫描、反向扫描角度和横屏stride/offset计算。DC仅接收720×1280、
  pitch2880、identity allocation，MPO非identity变换明确拒绝；仍保留Windows逻辑
  VidPN旋转枚举。`DriverRotation=1`表示驱动在图形引擎执行预旋转。
- 按微软DXGI契约，对需要旋转而未标记NONPREROTATED的primary创建返回
  `DXGI_DDI_ERR_UNSUPPORTED`，请求runtime使用rotated Blt进入固定竖屏primary。
  已预旋转的像素保持identity存储，不再二次旋转。
- 新增`src/nouveau/tgd3d/tgd3d_blit.cpp`及MIT原创fullscreen-triangle shaders，
  支持90/180/270度逆时针拷贝，GPU shader object按设备/角度缓存，图像view随backing
  退休并随资源identity轮换。比例不变用nearest，缩放用linear；非零目标矩形和
  mip/array子资源都按真实参数处理。BGRA/RGBA、XRGB alpha-one、sRGB编码值保留；
  float/display隐式gamma互转目前明确拒绝，没有伪称实现。
- 拷贝记录到既有图形队列，与既有双向GPU Fence、完成值和在途对象退休相衔接。
  没有新增Present前CPU等待，没有CPU整屏旋转，也没有强行打开SourceVisible。

Noble的DC行场/PLL目标与既有Windows路径相符，本轮不修改DSI或PLL时序。
Noble `WIN_OPTIONS=0x40010000`中的0x10000是CP_ENABLE，属于其颜色处理配置，
不是旋转开关；不连同Linux LUT状态一并验证就不能只抄这一位。Linux的IOVA也不能
作为Windows主表面地址。真正采用的是其固定竖屏fetch几何、无column/decrement，
地址仍取Windows allocation并等待有效锁存。

参考：Microsoft Learn `DXGI1_2_DDI_BASE_FUNCTIONS`/`DXGI_DDI_PRIMARY_DESC`、
`D3DKMDT_VIDPN_HW_CAPABILITY`，以及Khronos `vkCmdDraw` shader-object动态状态契约；
2026-11-14查阅，Windows头文件10.0.26100.0。本路径仅改变tgd3d旋转，未把Noble
桌面的预旋转执行器未经分析就断言为GPU或VIC。

构建刻意保留已部署组件的源码谱系：UMD以冻结9de42c82及其已应用的XRGB、错误传播、
flush、standard primary、DriverFlags补丁为基础，再加本轮旋转；没有夹带HEAD中的
shader binary disk-cache改动。KMD以H轮冻结源码加本轮显示补丁，仍`.158 + bit14 +
GPU460.8`；ICD未改。初次冻结构建误带HEAD版context测试的新诊断字段，编译失败；
改为对冻结文件应用本轮窄diff后，最终全量ARM64构建通过，没有为通过编译去更改
内核诊断ABI。

已完成：shader glslang/spirv-val；HEAD和冻结UMD x64 CPU contracts；ARM64 UMD和KMD
交叉构建；KMD host models；真实WDK头文件编译的x64 scanout/timing controls在Wine
通过。测试覆盖GPU命令/状态恢复、角度specialization、view缓存、OOM传播、未完成
GPU对象不提前退休、rotated primary fallback、fixed portrait allocation、MPO变换
拒绝及VBlank锁存。它们不执行Switch GPU，不构成像素、物理横屏或稳定性验收。

候选输出：OUT `gpu-prerotation/`，包括冻结源码、差异、构建命令和控制结果。
ARM64 UMD SHA256 `75df9fcb4a23a0fb326d63463a264354b54162a30fc58de04c7e5787d5bf580c`；
SYS签名后SHA256 `49d21c47cb0937d4b57b68af689709a1f896ef5a05fd74ca371ea547f6d37718`。
SYS已有本地签名，CAT尚需Windows原生Inf2Cat生成并验证；不能称为已安装的完整驱动包。
打包工具要求此portrait-only KMD与同一补丁的GPU预旋转UMD成对，恢复仍指向组合B。

下一步在Windows环境补齐CAT和原生工具检查后，直接验证逻辑横屏、物理竖屏backing
的方向/边框/像素与显示锁存，保留约60Hz请求及实际节拍的区分。设备仍在Ubuntu，
未重启、未写SD、未改变频率/充电或Windows启动方式；Gemini的DVFS/托盘候选不会
混入本轮旋转组件验收。

### J后续：Flip旋转契约及SM53编译检查

实际调用链复核发现`TgGpuDdiPresent`仍承认`Flip | Rotate`，注释也仍声称panel执行
旋转。DC变换已移除，这会把没有执行的工作报为成功。本次从no-DMA Flip允许标志中
去掉Rotate；普通预旋转后的Flip/NoWait保持支持，需额外旋转的请求返回
`STATUS_NOT_SUPPORTED`。没有重启、强开source、补CPU拷贝或关闭双向Fence。
该遗漏属于新预旋转候选，不据此倒推旧版本黑屏/重启根因。

完整冻结KMD的真实WDK context fixture已在x64/Wine执行，通过primary/shared/GDI
allocation、句柄、512MiB、队列、资源退休及Flip标志检查；GDI旧横屏descriptor用例
改为验证拒绝及零allocation泄漏，竖屏GDI拷贝和XRGB/ARGB转换用例继续执行。
首次执行指出测试仍期待旧Rotate成功，更新对应预期后全部通过；没有将失败解释为
GPU错误。ARM64 SYS及更新后的context测试程序已编译，尚未在Windows实机运行。

复用`nvk-tgd3d-shader-check`，增加可选VS specialization constant0参数，直接读取
候选内嵌SPIR-V原字节，走生产NVK→NIR→NAK SM53编译。90/180/270度VS分别产生
192/256/192字节（17/22/17条指令），PS产生192字节（16条指令），均24 GPR，全部
退出0。核对冻结ICD来源d3423d269f9e716eadc1237beadbfb6aa6049dbe与当前NVK shader、
NAK、NIR、SPIR-V和descriptor-layout实现无差异。结果与机器码/汇编哈希在
`gpu-prerotation/nak/`；编译通过不能证明硬件执行或已知像素正确。

当前J候选SYS签名后SHA256更新为
`346528eeb5e456069b8503eec3fb82ad4f9faaeb5620f0318365c1632cd16ef4`。
原49d21…候选保存在`gpu-prerotation/superseded-01e5/`，不再用于测试。
UMD75df9…与ICDdee3…不变；CAT仍待原生Windows生成及验证。用户指定保持Ubuntu，
本轮不触发Windows安装/启动/显示测试；主目标继续active，本地代码与Gemini交付仍推进。


### 后续静态审查（与缓存更新分开）

用户要求审查其他tgd3d代码，并取消新增离屏检查；未创建或运行新的离屏像素探针。
继续沿描述符、slice/view退休、设备销毁、Query/Fence调用链读源码。设备销毁目前
先等待真实提交Fence，必要时DeviceWaitIdle，再强制退休对象；没有找到新证据支持
“新增预旋转对象必定提前释放”。不因审查未发现就宣称该路径已实机通过。

Query错误回调有两处明确契约差异待单独修复：`CreateQuery`对未实现类型或缺少
capability调用SetErrorCb(E_NOTIMPL)，微软仅允许E_OUTOFMEMORY、NONEXCLUSIVE、
DEVICEREMOVED；`QueryGetData`会将pool创建失败后记录的q->error=E_OUTOFMEMORY直接
转交SetErrorCb，而该DDI只允许WASSTILLDRAWING/DEVICEREMOVED。对应源码为
`src/nouveau/tgd3d/tgd3d_query.cpp`的CreateQuery、query_pass_begin、QueryBegin/End
和QueryGetData。问题可以触发runtime认定critical driver error，尚无证据证明它就是
历史硬件自动重启的根因。不得把不支持的查询静默写成成功或CPU伪造结果。
依据2026-11-14核查的Microsoft CreateQuery及QueryGetData文档（更新时间分别见官方页），
先保留独立修复项，不混入本轮已冻结的缓存UMD。

## 缓存接入与Windows安装（2026-11-14 11:11 UTC）

用户自行切回Windows后明确要求“先把缓存接上”。此前源码已有
`35696545d482ff7bbe68c1dcf5da496d357ba0f8`的shader-object缓存，但冻结UMD没有带入，
其旧构建也未启用`ENABLE_SHADER_CACHE`；不能把源码存在当作实机已使用。本次从
I轮冻结UMD源码单独移植该提交，使用含Win32缓存后端
`16a0c400a93255a846e3add6b427c897f9b4a3b9`的库，所有tgd3d对象重编，强制核对缓存
宏且不带CPU同步控制宏。没有夹带GPU预旋转、Query修复或Gemini的DVFS改动。

| 组件 | 本轮实际SHA256 |
|---|---|
| 保持的`.158 + bit14 + 460.8MHz` KMD | `1a86a02b479b716b2b695ade0bd6860076d4d5f61a29d95b804cb655ce0bf164` |
| 新ARM64缓存UMD | `0cd34833bbeff8dff4b6a7f6db5f1b681ca7f5df38a44190b03085a229c7b2e8` |
| 保持的d3423d26 ICD | `dee3d5805af80696f87d094ec1c5f557d662f435336207992baf588e2dc1cee0` |
| 保留的更新前I轮UMD | `cefc41cffaed8dd4005ad820c27369ad0f66dbce72bdf228a1e5c790d94f864c` |

冻结基础为`9de42c82a03e84674954d8175237dcfd5bb4dc2e`，此前已包含XRGB、资源错误
传播、Flush、standard primary及DriverFlags修正；完整提交链、源码和链接输入哈希
见OUT `cache-only-live/update.json`及`build-manifest.json`。x64/ARM64构建均通过，
x64/Wine和Switch原生ARM64 CPU控制均通过，覆盖冷缓存写入、新进程二进制命中、键和
driver/stage失效、不兼容回退及OOM/device-loss错误传播。同步和资源退休控制继续通过。
这些控制不执行真实GPU，不是用户已取消的新增离屏像素检查。

11:01:06 UTC更新器完成暂存，并发出本轮唯一一次计划重启；11:01:41为新启动时间。
第一次读回仍为Code43且登录任务Running，未据此重复启用或追加重启；原任务随后
完成。11:05:26及11:11:32均为Code0/HardwareStatus0/PanelStatus0，默认任务Ready、
LastTaskResult0。DWM816实际加载上述新UMD及原ICD，路径位于新包目录；DWM启动时间
及系统启动时间连续，Explorer、StartMenu、TextInput和ShellExperience也加载tgd3d。
Windows为23H2/22631.2861，仍为登录触发延迟PT5S，AllowNativeBoot不存在。
CPU1497600kHz/1120mV、GPU启动实测460800kHz/PMIC设定800000µV、RAM1600000kHz、
风扇13保持；动态512MiB预算策略未改。

实际桌面运行在用户LocalAppData下生成13个缓存二进制，共25758字节，写入时间在
本次原生启用后，目录后缀为
`685de1ecb01eb6342da277bfcdeedba5e25592019c6d58fcdd7b90ece37981bc`。
这些文件不是CPU控制在TEMP内创建的样本。此机器现有SYSTEM/服务账户Shell Folder
注册同样指向该LocalAppData；本轮未改用户配置或扩展缓存ACL。目录存在与文件写入
不能单独确定每个写入进程，也不等于已测得真实GPU热缓存命中率或提速幅度。
缓存上限128MiB是磁盘预算；DXBC→SPIR-V转换仍执行，持久缓存主要复用NVK/NAK编译
结果。未把本轮进程/启动连续性写成五分钟物理交互验收或横屏通过。

当前包：
`C:\ProgramData\SwitchWOA\GpuUpdate\tgd3d-cache-0cd34833-20261114-190102`。
更新前完整目录保留：
`C:\ProgramData\SwitchWOA\GpuUpdate\tgd3d-54dc5597-20261114-172511`。
新包内`cache-update-backup.json`、`cache-prior-task.xml`及`Restore-ShaderCache.ps1`
保留原注册和任务。恢复命令为管理员PowerShell执行：

```powershell
& 'C:\ProgramData\SwitchWOA\GpuUpdate\tgd3d-cache-0cd34833-20261114-190102\Restore-ShaderCache.ps1' -Restart
```

它仅恢复更新前UMD注册/包路径及登录任务，重启后加载旧UMD，不更换本轮KMD/ICD，
不回退`.59`。两脚本已通过Windows原生PowerShell解析，旧UMD哈希和恢复文件已读回
核对；未额外执行恢复演练。UMD/控制程序载荷ZIP的SHA256为
`3e7e08f5091dced20a0716504cd4fbc161b2ef0c4d7b7415b7e544c5954217d3`。
更新/恢复脚本另行传入暂存目录；完整交付归档为
`cache-only-live/tgd3d-cache-0cd34833-delivery.zip`，含载荷、脚本、构建清单和本轮回执。
安装、启动及缓存元数据在OUT `cache-only-live/`的`native-controls.json`、
`install-request.json`、`installed-verified.json`和`cache-files-01.json`。

下一步仍按单问题推进：真实GPU热缓存收益尚未测量；Query错误回调单独修复；J候选
需与缓存版重新组合构建后再进行横屏测试，不能安装旧75df9…旋转UMD而无意撤掉缓存。

## Query契约修正（2026-11-14，源码修正，尚未部署UMD）

本轮针对上述Query审查结果修改生产实现，仍保持Switch当前0cd34833…缓存UMD。
查阅微软[CreateQuery](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/nc-d3d10umddi-pfnd3d10ddi_createquery)、
[QueryBegin](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/nc-d3d10umddi-pfnd3d10ddi_querybegin)、
[QueryEnd](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/nc-d3d10umddi-pfnd3d10ddi_queryend)
（均更新于2024-02-22）和
[QueryGetData](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/nc-d3d10umddi-pfnd3d10ddi_querygetdata)
（2022-08-23）。WDK头文件为10.0.26100.0；这些DDI契约适用于本分支当前D3D10接口，
文档日期较早不表示可以把DXGI API错误码直接当作DDI回调错误码。

- CreateQuery保留合法的E_OUTOFMEMORY返回，失败对象在SetErrorCb前析构，因为runtime
  不会再调用DestroyQuery。未实现的查询或缺失后端能力仍失败，记录原始原因并进入
  device-removed状态；不把它们伪装成内存不足或有效查询。本轮未增加stream-output
  查询功能，也未提高Feature Level或能力声明。
- Begin/End、跨render-pass查询池扩展失败只通知D3DDDIERR_DEVICEREMOVED，停止后续
  录制和提交；旧pool在新pool创建成功前保持所有权，仍按真实完成值退休。
  fullPools/activeQueries扩容失败同样不让C++异常越过该查询调用边界。
- QueryGetData轮询Fence发现移除后立即返回，不再被WASSTILLDRAWING覆盖；失败时输出
  缓冲区不变。固定上限的查询结果暂存改用栈数组，轮询不再为结果临时分配vector。
- End未先Begin时按微软契约开启并立即结束新的空区间，不复用上次结果。
  timestamp End经过的命令缓冲分配失败也改用合法的移除通知；原始Vulkan分配错误
  留在已有trace事件中。这个分配路径也供Draw/Flush使用，二者同样只允许移除错误。

x64/ARM64构建均通过且启用缓存和GPU bridge；x64/Wine及11:27:10 UTC的Switch
ARM64 CPU控制均通过：初次创建OOM可恢复、未知失败/未实现类型/缺失能力明确失败、
Begin重用/End时间戳/第129个pass扩展OOM、GetQueryPoolResults未就绪/错误、Fence
读取失败、命令缓冲分配失败、失败后不再录制/提交及不提前退休。既有129-pass求和、
DONOTWAIT、双向Fence和缓存控制继续通过。这些测试替换Vulkan/runtime调用，不执行
GPU，不构成开窗、横屏、实屏稳定性或历史重启根因的验收。
结果及可独立审查的构建产物在OUT `query-contract-fix/`。

## J候选加入缓存及完成原生目录签名

以刚部署的缓存冻结源码为起点，仅追加01e5aa3e的GPU预旋转，保留shader cache和
双向Fence。该候选**未包含上述Query修正**，以便分别验证。冻结控制文件的历史上下文
不同，原旋转diff的一个测试hunk不能直接应用；生产hunk全部直接应用，测试仅取回
相同的presentation_rotation_contract并保留缓存控制入口，没有复制整个HEAD。

新ARM64 UMD为`cef5daa8c189f6cc0ceecb660d73c0b830a380993b638c8926030704a67737ef`，
代替旧75df9…旋转UMD。x64/ARM64构建、x64/Wine和11:27:10 UTC的ARM64 CPU控制
通过。保留J候选SYS
`346528eeb5e456069b8503eec3fb82ad4f9faaeb5620f0318365c1632cd16ef4`，原生Inf2Cat
成功且无错误/警告，签名CAT为
`d5e31a90c95f8ee3d19c78d122ef561f8b604da11698c5959ccce074a8470d43`；Windows
SignTool验证全部13个包成员通过。源码、构建和签名回执分别在OUT
`gpu-prerotation-cache/`和`gpu-prerotation/windows-signature-verification.json`。

打包器和恢复工具现在接受已核对的1a86a02b…恢复KMD，后续旋转候选恢复到当前I轮
KMD，不再默认退回较早69aa…组合。打包清单同时保留UMD缓存配置及冻结提交链。
现有Switch缓存包及其旧UMD备份继续保留，下一步按当前组合准备安装与恢复流程。
截至本节，未安装旋转KMD/UMD、未改变方向或追加重启；物理横屏和GPU已知像素仍未验收。

## J轮实机失败与恢复（2026-11-14 11:40–12:14 UTC）

本节取代上一节“未安装”的历史状态。实际安装组合为上述J轮SYS `346528ee…`、
缓存UMD `cef5daa8…`、原ICD `dee3d580…`；未包含Query提交b6b17521或DVFS改动。
完整包`tgd3d-gpu-prerotation-cache-logon.zip`为7302391字节、69个文件，SHA256
`8937bedd218d1e8d5c755df861d9dd81ba949c305f3977b3f4d3dd73e2507159`。
11:36:31的目录校验和ARM64 context控制通过，但该控制使用替身，不执行GPU。

- 11:43:03发出安装所需的一次门控重启，启动时间11:43:25.970；确认Code43后安装。
  首次SSH安装请求握手失败，检查任务和安装记录均不存在后才提交实际安装任务。
  11:50:38读回Code0，DWM840加载cef5daa8…，测试LUID `00000000000e9ae9`。
- 11:52:38启动直接横屏测试；约11:52:52切至DEVMODE 1280×720/orientation1。
  CCD source与物理target均为720×1280，path rotation2。严格SDC验证1610，工具使用
  SDC_ALLOW_CHANGES完成切换；这只证明系统接受调整后的配置，不能证明原请求完全匹配。
- 11:52:49切换前：source_visible1、target_active1、DC options `0x40000000`，
  address/displayed_address `0xfbf09000`，displayed_present79，pending0。
  11:52:58、11:52:59、11:53:01三个切换后快照均为source_visible0、target_active1、
  options0；地址仍是`0xfbf09000`，displayed_present80，pending0。
  pitch2880、720×1280、offset0及四个旧桌面像素均未变。不能把这些旧像素当作新
  旋转帧，也不能宣称GPU已经完成旋转、仅漏发一次Flip完成通知。
- 用户报告自动重启，新启动时间11:53:35.7470770。彩色工具虽保存ready/geometry，
  没有完整截图或物理上屏确认；本轮记为横屏失败，停止扩大测试，没有重试同一候选。
- 12:02:50恢复缓存接入节中的原组合：SYS `1a86a02b…`、UMD `0cd34833…`、ICD
  `dee3d580…`，Code0，DWM820实际加载恢复UMD。恢复没有请求系统重启。
  12:11:30核对当前及保存方向均为720×1280/orientation0；工具changed=false，
  没有再切一次显示模式。默认登录任务和原频率设置恢复，未使用`.59`。

记录位于OUT `gpu-prerotation-cache/`的安装、失败、恢复回执及`evidence/`。
已取回3346行UMD实时trace及3145728字节ETL；可解析窗口约11:52:51–11:53:04，
最后的TraceState报告丢失计数0，但硬重启前尾段缺失，没有TraceEnd，不是完整故障轨迹。
窗口内77次DWM SetDisplayMode返回S_OK，Blt返回事件0；Blt事件位于函数返回处，
内部旋转shader也没有ShaderPhase事件，因此“没有返回事件”不能证明未进入旋转或
shader编译。早期Fence完成值有推进，同样不能证明最后一笔工作已完成。
文件传输成功；此前文件长度快照中一个pattern.log为0，取回时已写成112字节。
后续远端SHA256复核两次SSH握手被关闭，全部文件的远端哈希复核仍待完成，未将其
写成已校验。已结束的安装/恢复临时任务最后为Ready/0，删除确认同样待连接恢复。

## 屏障、完成通知与UMA地址审查（2026-11-14，源码审查）

按用户“先查屏障与Present后的Fence”执行，没有安装新候选或实施红色主表面对照。
逐文件比对确认：本节所查runtime/resource、KMD allocation/paging/scanout/缓存代码
与J轮冻结输入相同；NVK内存、屏障代码与实际ICD来源d3423d26相同。提交worker采用
冻结KMD版本审查，未把HEAD后来增加的诊断字段误作实机已有行为。

| 环节 | 实际实现与结论 |
|---|---|
| CPU、GPU及DC地址 | `tgd3d_runtime.cpp::owner_map_gpu`用runtime allocation handle及4KiB页偏移映射GPU VA，核对范围并等待paging fence。`runtime_bind_image`绑定同一owner；没有把CPU指针或用户态句柄直接传给DC。KMD扫描主表面来自16MiB物理连续扫描段，限制在4GiB以下；`TgGpuResolveScanoutAddress`检查地址及帧长度落在段内。PTE解析另按SegmentId和页偏移取得backing，不能混用两种接口的地址单位。 |
| 图像布局 | runtime图像使用LINEAR，明确给ICD row pitch，随后核对GetImageSubresourceLayout的pitch和offset0；扫描面固定720×1280/pitch2880/identity。拒绝的描述符不能伪装成创建成功。 |
| 缓存属性 | 私有扫描段/页表通过NON_CACHED分配；GPU页表使用VOL/uncached，未宣称CacheCoherent；普通共享CPU可见section使用SEC_WRITECOMBINE。Windows NVK不暴露HOST_CACHED内存类型。这些是源码策略，不能代替对故障时实际PTE及CPU映射属性的核实。 |
| 旋转屏障 | 90/180/270度由采样源图＋颜色附件绘制完成，不是vkCmdBlitImage。cmd_begin已有ALL_COMMANDS全局读写屏障，首次image_use作PREINITIALIZED/UNDEFINED→GENERAL转换。GENERAL布局允许这条绘制路径；不能机械改为只适用于传输的TRANSFER布局。 |
| 写回与提交完成 | flush末尾有GPU写→HOST_READ依赖。冻结KMD在观察实际GPU完成标记后执行FB/L2 dirty writeback及clean invalidate，再quiesce并核对CPU可见完成字，满足条件才通知DMA_COMPLETED。不存在“CPU看到提交成功就直接报GPU完成”的正常路径。仍不能凭函数存在保证失败那一笔已执行到这里。 |
| 显示完成 | `panel_scanout.c`在真实VBlank读取实际锁存地址，更新Displayed；按启用类型发送经典CRTC_VSYNC地址或MPO2 PresentId，并QueueDpc。渲染Fence和扫描锁存是两条完成链，已有通知实现不能证明某帧确实进入、锁存并通知。 |

审查结论：目前没有定位到“完全没有缓存处理”“直接把GPU VA当物理扫描地址”或
“根本没实现Flip通知”。统一DRAM不保证跨CPU/GPU/DC的自动一致性，仍需匹配布局、
地址、可见性和执行顺序。现有失败快照首先指向显示源关闭及主表面交接没有继续，
不是已证明的缓存一致性根因；后续应核对该交接处的callback输入、实际backing和
锁存，避免盲加屏障、强制开启SourceVisible或伪造完成。

另有源码差异：d3d10umd的SetDisplayMode调用RuntimeResourceFinish；tgd3d调用
flush(true)，后者只保证提交/队列门控/有限回压，不是最后一笔的完整CPU等待。
这是待核实的模式切换同步差异，尚无契约或实机证据证明必须照搬前者。用户明确旧
硬件旋转架构也不可用，因此不得把该旧路径当作已通过的横屏对照。

本次实际参考：Microsoft [GPU虚拟内存](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/gpu-virtual-memory-in-wddm-2-0)、
[GPU segments](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/gpu-segments)、
[allocation usage/Lock2](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/allocation-usage-tracking)，适用于本分支GpuMmu路径，不采用IoMmu的CPU/GPU同指针假设；
[SetVidPnSourceAddress](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/nc-d3dkmddi-dxgkddi_setvidpnsourceaddress)
（2022-11-04）、[primary描述](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/dxgiddi/ns-dxgiddi-dxgi_ddi_primary_desc)
（2024-02-22）、[旋转契约](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/supporting-rotation-in-a-display-miniport-driver)；
Khronos [同步规范](https://docs.vulkan.org/spec/latest/chapters/synchronization.html)及
[Synchronization2示例](https://docs.vulkan.org/guide/latest/synchronization_examples.html)
（2026-11-14访问的latest页面）。同布局全局memory barrier可以覆盖图像；布局改变或
ownership transfer另有要求。微软display-only样例不能直接替代本分支完整GPU驱动的
资源驻留、调度和呈现实现。

## 红屏最小对照与强开探针（2026-11-14）

用户要求按此前方案绕过旋转，先把720×1280主表面清红。源码befb7959增加默认关闭的
`TGD3D_RED_PRIMARY_CONTROL`：使用runtime创建的原生主表面，不复制句柄或伪造地址；
替换旋转Blt，并在SetDisplayMode前记录纯红clear、提交及真实Fence等待。未改变正常版
GPU桥接策略；诊断等待不能写成生产模式已修改。测试UMD SHA256
`71d548335b9e3c3a953ed8d969f739c9f3278af452ed0f5234bd67d7bd781338`；保持J轮SYS
`346528ee…`和原ICD `dee3d580…`。x64/ARM64构建、正常版及诊断版CPU控制、Windows
原生ARM64控制通过，未执行新增离屏像素探针。

13:30:06请求一次安装门控重启，13:30:47新启动；安装请求第一次SSH握手超时，核对
任务不存在且没有install记录后才实际提交。13:38:19已Code0，DWM780实际加载上述
诊断UMD。测试任务13:39:06开始，13:39:34结束，启动时间持续一致，没有本轮异常重启。
本轮LUID `00000000000e5c96`；Windows仍22631，外接电源Online。

- 切换前13:39:16：source_visible1、target_active1、WIN_ENABLE1，地址`0xfb067000`，
  displayed_present56，四点为旧桌面像素。
- 13:39:20、13:39:23、13:39:25：逻辑横屏，source_visible0、target_active1、
  WIN_ENABLE0，地址仍`0xfb067000`，displayed_present57，pending0。三次共12点均为
  `0xffff0000`，容差0、差异0；pitch2880、720×1280、offset0，underflow6未增加。
- 同一DWM在切换后有59组clear录制、真实Fence等待成功及随后SetDisplayMode成功。
  当前DC地址对应的backing从桌面像素变红；这排除了这块缓冲区“清屏没有执行/写入
  完全不可见”的解释，但不证明所有新primary地址交接正确，也不证明物理OLED亮红。
  本轮显示启用检查失败；下一步查SourceVisibility调用顺序及主表面交接。
- 工具13:39:32恢复720×1280/orientation0。22个文件共8324300字节已取回并逐个核对
  远端SHA256/长度；实时trace有TraceEnd，invalid及丢失计数0。结果在OUT
  `red-primary-control/evidence/`及`returned-files.json`。SFTP第一次误用相对盘符路径
  未传回文件，修正为`/C:/...`后传输和哈希校验通过；属于工具错误，不是GPU失败。

13:45:34发出计划恢复重启，13:46:14启动。恢复任务退出1、未产生恢复state：检查发现
控制器目录有正确ACL，而Restore-PreviousCache.ps1文件DACL为空，SYSTEM也不能读。
这是本轮`icacls /inheritance:r /grant ... /T`对文件的错误处理。13:56:24恢复文件继承，
读回SYSTEM/Administrators Full、Users Read/Execute及正确脚本哈希；本地控制器也已
改为只保护目录并启用文件继承。没有把这一恢复工具故障归因于GPU。

用户随后明确授权寄存器强开测试。设备停在Code43门控，原恢复任务Disabled，先不
启用旧组合来重复桌面测试。0dd8c544增加默认关闭的`--force-red-scanout` KMD诊断：
保留OS的SourceVisible记录，对已验证位于自有扫描段的主表面使用真实本轮地址和
既有DC寄存器提交/锁存流程强开窗口；不硬编码上轮地址，不伪造Flip完成。目标关闭、
禁用平面、停止和POST交接仍按原路径处理。正常/强开两个真实WDK模型和原生ARM64
模型通过，包含“先隐藏、后换地址”、范围拒绝、目标断电和停止恢复。签名SYS为
`a5755760fb75669c1ea067c18f5fc0af62e2d8b7c7adee801647720398fdb4e9`；Windows
Inf2Cat和13个成员签名验证通过，实机强开结果仍待测试，不能当作正式修复。

已核对[SourceVisibility契约](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/ns-d3dkmddi-_dxgkarg_setvidpnsourcevisibility)
和[MPO呈现](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/multiplane-overlay-vidpn-presentation)：
Visible=FALSE本应停止扫描，仍保留VSync。当前WDK 10.0.26100及接口表没有
`DxgkCbNotifyFlipCompleted`入口；经典MMIO flip通过实际VBlank地址通知，MPO通过
锁存的PresentId通知。强开只回答红色backing能否通过DC输出，不把忽略Visible=FALSE
作为生产修复，也不能由一次红屏宣布所有时序、同步及资源生命周期均正确。

### 强开实机结果：红色上屏但闪烁（14:20–14:21 UTC）

14:14:05在既有Code43门控中恢复I核及原缓存注册，仅用于准备候选，没有启动GPU或
追加重启。14:15:21提交强开版安装任务，14:16:29原生启用完成，DWM780加载红屏UMD；
安装任务退出0。SYS为上述a5755760…，UMD仍71d54833…，ICD仍dee3d580…。
目录`C:\ProgramData\SwitchWOA\GpuUpdate\tgd3d-befb7959-20261114-221530`，
LUID `000000000014cd30`，Windows10.0.22631.2861，外接电源Online。

14:20:34开始一次强开横屏测试，14:21:03结束；期间启动时间保持13:46:14，未发生
测试中的自动重启。14:20:43切换前source_visible1、WIN_ENABLE1、PresentId42，
地址`0xfbba1000`、四点为原桌面颜色。14:20:48.172、50.594、52.891三个采样均为：

- path_rotation2，source_visible0，target_active1，options0x40000000；强开确实生效。
- DC地址与Displayed地址均为`0xfbba1000`，PresentId43，pending0；固定720×1280、
  pitch2880、offset0，underflow4在采样中未增加。
- 每次四点均`0xffff0000`，共12点、容差0、差异0；工具的清屏录制、真实Fence等待、
  随后SetDisplayMode检查通过。最终完整trace有80次清屏/等待、79次SetDisplayMode
  成功返回，仅1次Present返回，Blt返回0；其中有回收trace时继续发生的调用，不能与
  测试脚本较早读取时的64次SetDisplayMode配对数混用。

用户本轮物理观察原话为“红色有但在闪烁”。本轮总体记录为**稳定性失败**，不能用
软件工具退出0掩盖闪烁。与不强开的最小红屏对照相比，强开使红色扫描面到达物理
面板；这削弱“这条固定720×1280时序完全无法驱动OLED”的解释。尚不能断言闪烁是
OLED、GPU写扫描面、地址切换还是可见性切换所致，三个间隔采样无法排除间隙变化。
反复创建主表面和SetDisplayMode而未恢复持续Present，是接下来审查模式交接的依据。

14:21:00工具恢复当前及保存方向720×1280/orientation0，ETW及短时恢复任务已停止。
39个本轮文件共2559061字节取回，逐个SHA256/长度匹配；实时trace7019行，有TraceEnd，
invalid及events/log/realtime lost均0。结果在OUT `force-red-scanout/evidence/`，
物理反馈在`physical-observation.json`，总体结论在`test-summary.json`。
14:22:44发出一次计划恢复重启，用于卸下诊断KMD；恢复目标是原缓存组合而非`.59`。

14:23:35为计划重启后的新启动；SSH恢复前仅重连/读取，没有重复启动或追加重启。
恢复任务14:25:49开始、14:26:40结束，退出0；14:26:54核对SYS
`1a86a02b479b716b2b695ade0bd6860076d4d5f61a29d95b804cb655ce0bf164`、Code0，
DWM460加载`0cd34833bbeff8dff4b6a7f6db5f1b681ca7f5df38a44190b03085a229c7b2e8`。
恢复ICD仍为`dee3d5805af80696f87d094ec1c5f557d662f435336207992baf588e2dc1cee0`，
默认启用Ready、gpu-bridge、CPU1497600/GPU460800/RAM1600000kHz、风扇13。恢复成功
仅表示上述组件与设置复原，不代表本轮闪烁或完整桌面稳定性已经修好。

14:28:45从交互会话只读查询显示配置及模块：当前和注册方向均720×1280/60Hz/
orientation0，DWM实际加载原缓存目录下的UMD及ICD，完整哈希匹配。此前SSH会话0
直接运行显示工具退出1/error87，不支持查询交互桌面；改用一次只读交互任务完成，
没有再次旋转或测试画面。14份恢复回执均已取回并校验。
清理只针对已结束的两轮红屏探针：6个临时任务、7个目录及17个上传包/脚本，另移除
清理脚本自身；不删除当前缓存包、不改变频率和启动设置。清理命令因Windows命令行
长度限制第一次未执行，改为上传脚本后执行；SSH等待50秒超时，14:31:17只读检查确认
上述任务与路径均不存在，原清理进程已退出，没有重复执行删除。原缓存注册仍有效。
源码诊断开关保留到正式修复验收后移除，交付报告和冻结构建保留在本地OUT。

源码核对：`source_visibility`确实转交Visible值，`set_visibility`分别保留source与
target门控；普通`program_flip`在新地址到来时继续服从已有SourceVisible。因此
“先FALSE、后地址”会使新绑定保持关闭，这是现有契约行为，不能靠永久忽略FALSE
解决。尚未证明实际缺了某次TRUE回调，或哪个上游失败阻止它到达。当前WDK没有用户
提出的NotifyFlipCompleted入口，已有VBlank/MPO通知不能替代进一步核对这轮的模式
和主表面生命周期。没有在这轮诊断结果上继续安装未经审查的第二个行为补丁。

### 正规中断通知与NO_SCANOUT复核：MPO mask候选

按用户要求直接检查`render_miniport.c`以及最后注册的显示DDI：文件中的空interrupt
是无显示后端入口；显示构建随后通过`TgGpuRegisterPanelLifecycle`和
`TgGpuRegisterPanelScanout`替换为`panel_interrupt`，不能仅看到前者就认定没通知。
后者在真实VBlank读取锁存状态，发送经典CRTC_VSYNC或MPO2 PresentId，再QueueDpc；
`context.c`的DPC调用NotifyDpc。GPU渲染完成仍由已验证的DMA完成路径报告。

查到明确遗漏：MPO2通知填写`PhysicalAdapterMask=1`却没有置
`Flags.ValidPhysicalAdapterMask`，与微软字段契约不符；本轮补上该有效位，并在真实
WDK模型中核对mask=1、Flags仅含有效位，仍只报告已锁存PresentId。未把这个契约
修正提前宣布为横屏黑屏根因。冻结候选相对J轮KMD只多这3行通知修正及2行控制断言。
KMD强开源代码、测试分支、CLI和打包开关同时移除，正常source/target显隐行为恢复。

实际参考WDK10.0.26100.0的shared/d3dkmddi.h，构建DDI为WDDM2.6，以及微软
[中断数据](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/ns-d3dkmddi-_dxgkargcb_notify_interrupt_data)
和[平面输出标志](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/ns-d3dkmddi-_dxgk_plane_specific_output_flags)
（2026-11-14访问）。`HsyncFlipCompletion`不是所有Flip都必须设置的完成暗号；即时
翻转可以使用HSync完成，普通VSync通知也能完成异步即时翻转。当前同步即时路径在
PASSIVE级等实际锁存才返回，常规路径由真实VBlank通知，不能统一伪标成HSync事件。

`NO_SCANOUT`复核：`tgd3d_resource.cpp`成功创建primary后明确将DriverFlags清零，
已有runtime控制覆盖复用旧NO_SCANOUT描述符后的清零。只有不能直接扫描的旋转
primary会设置NO_SCANOUT并返回DXGI_DDI_ERR_UNSUPPORTED，请求runtime代理面/Blt。
这符合[DXGI资源创建契约](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/passing-dxgi-information-at-resource-creation-time)，
该标志控制直接Flip资格，不是完全禁止拷贝后上屏。本轮没有找到已接受primary误留
NO_SCANOUT的证据，未作无依据的UMD改动。下一轮用正常GPU预旋转UMD cef5daa8…及
原ICD dee3d580…，保持频率/双向Fence，只测这次MPO通知修正。

### MPO有效掩码修正实测失败及恢复（14:49–15:21 UTC）

e28f5886939042032f0a1698e8929088a482a54e已通过单次HTTP/1.1 Git传输推送；此前
两次HTTPS超时没有修改Git配置。签名SYS
`08ce8cf54a9176936185cdcd236ff75b1b38454e0ed3b832012eb28ddefe4543`，正常UMD
`cef5daa8c189f6cc0ceecb660d73c0b830a380993b638c8926030704a67737ef`、ICD仍dee3d580…。
ARM64构建/控制、13个目录成员签名及69文件包核对通过，均不算GPU或上屏通过。

14:43:10请求一次安装门控重启，启动时间14:43:47；SSH恢复后核对原缓存Code43，
14:46:43启动安装任务，14:47:52原生启用完成。DWM496加载正常UMD，LUID
`00000000000c73ee`。14:49:02–14:49:53只测一轮横屏；没有新增离屏GPU探针。

- 14:49:11切换前地址`0xfae61000`、PresentId39、source_visible1、WIN_ENABLE1，
  四点为桌面颜色。
- 14:49:19.642、21.080、22.595均source_visible0、target_active1、WIN_ENABLE0，
  地址/Displayed地址仍`0xfae61000`、PresentId40、pending0，12点全0；underflow6
  未增加，扫描布局仍720×1280/pitch2880/offset0。
- 逻辑图案1280×720、5020点容差0差异0；它不等于实际扫描面的图案。工具明确
  退出1：`Portrait scanout activation/layout did not match GPU prerotation`。
- 最终trace12212行，有TraceEnd，invalid和各丢失计数0。按首个扫描采样的UTC与
  trace起始QPC时钟对应，之后DWM又有186次SetDisplayMode成功返回，没有Present或
  Blt完成记录；这是返回事件统计，不能排除曾进入某个尚未返回的调用。
- 14:49:50恢复当前/保存竖屏，ETW停止、短时恢复任务解除。52文件3293252字节
  全部取回并逐个SHA256/长度匹配，位于OUT `mpo-vblank-mask/evidence/`。

14:54:21请求一次计划恢复重启，14:55:12启动；恢复任务14:58:14完成，退出0，
15:01:23读回Code0、默认启用Ready、DWM828加载原缓存UMD。15:21:33交互只读核对
当前和保存方向均720×1280/orientation0、UMD和ICD路径/完整哈希正确。CPU1497600、
GPU460800、RAM1600000kHz、风扇13及双向Fence保持，不恢复`.59`。

结论：有效掩码是应修的通知契约遗漏，但本轮证明仅补它不足以恢复横屏。强开KMD
代码已删除，成功primary的NO_SCANOUT清零也与冻结生产UMD源码相同；不再重复这版
候选。下一步按用户授权补齐真实Rotate Blt和横屏shadow，仍以实际GPU完成、扫描锁存
及物理画面验收为准，不用HsyncFlipCompletion伪造已完成的工作。

### 四方向 GDI Rotate Blt 与标准 shadow（构建及控制完成，实机待测）

保留 Windows 设置的0/90/180/270四方向能力。补上标准1280×720、pitch5120的
非primary shadow，并在tgd3d导入时使用真实宽高/pitch；物理primary仍严格保持
720×1280、pitch2880。`DXGK_PRESENTFLAGS.Rotate`只在Blt路径按已提交的VidPN角度
执行，普通Flip不能把未旋转的地址直接报为完成。

- 0°沿用CE矩形复制；90/270°用GM20B B0B5正stride列/行复制，支持裁剪、偏移、
  XRGB转不透明ARGB和有限DMA缓冲区的MultipassOffset续传，不使用负CE pitch。
- 180°用902D二维引擎双轴翻转，单个完整命令块；32.32导数均-1，采样起点为
  末像素中心。参考deko3d固定373bfd43d8b2d805fa2eb7d406dbb823032df958的
  Blit2DEngine及FlipX/FlipY约定，许可证/来源/改写说明保留在NOTICE和Zlib.txt。
  没有新增着色器、CPU旋转或整屏scratch分配。
- 902D准入只接受完整固定状态序列，核验矩形、对齐、源/目标完整跨度、保留VA
  排除和实际映射读写权限；不开放任意2D命令。源目标重叠仍不支持。
- 完成顺序沿用GPU全引擎WFI、真实DMA完成和显示控制器地址锁存后的VBlank通知，
  不新增虚假的HsyncFlipCompletion，不强制WIN_ENABLE。

实际参考WDK10.0.26100.0及微软
[Present标志](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/ns-d3dkmddi-_dxgk_presentflags)
（2026-11-14访问）；`Rotate`是图像搬运责任，不是放行未处理Flip的理由。

本地独立六像素向量、非对齐矩形、padding、alpha、损坏包拒绝及映射权限模型通过；
ARM64 KMD构建通过，Switch上test-context/test-panel-timings退出0（包括180°、
四方向能力、multipass、实际WDK结构的Present编码和snapshot捕获）。早先同UMD
的ARM64 runtime controls已通过并复用。这些均为CPU控制，不是GPU或物理验收。
设备15:41:33UTC仍原缓存I基线Code0，启动时间14:55:12，DWM828；本候选尚未安装。

### 四方向候选798595d0：90°扫描检查失败，180/270°停止扩大测试

798595d0a73fa84196a6ef48b1fcd6afad77f622已推送tgd3d-vulkan（两次HTTP/1.1
超时后，默认传输成功；没有改Git配置）。签名SYS
`d9be1b6cc0cd2493efbbbc58e61588f15fe111917cf83e1df4eb0153bd32f7c6`，UMD
`d28d5669ca284f1b2dcd7541bccc05c2ca5d409f6cf6283884dadade110999c4`，ICD
`dee3d5805af80696f87d094ec1c5f557d662f435336207992baf588e2dc1cee0`。
13个CAT成员和71个可还原包文件校验通过；包variant沿用受检的gpu460-vidpn枚举，
四方向补丁另记gdi_rotation_patch_commit，未放宽恢复基线校验。

15:45:41UTC请求一次安装门控重启，15:46:15启动；SSH恢复后确认旧I内核Code43、
默认启用Disabled，未因连接超时重复启动/重启。15:49:04启动一次安装任务；
15:50:14新候选Code0，DWM780加载本轮UMD，gpu-bridge开启，频率保持。

15:50:55–15:51:44只完成了90°这一轮，LUID `00000000000d6fcf`：

- Windows逻辑1280×720图案5020个像素比较无差异；不能代表物理输出。
- 15:51:12.833、14.489、15.958三次扫描快照均source_visible0、target_active1、
  WIN_ENABLE0；地址和displayed_address仍为`0xfbf27000`，PresentId38，pending0，
  720×1280/pitch2880/offset0。12点仍是旧桌面颜色，underflow3保持。
- 工具退出1：`Portrait scanout activation/layout did not match rotation 90`。
  因本方向失败，没有继续测180/270°，不能把其CPU控制结果写成实机通过。
- 15:51:42当前/保存配置恢复720×1280/orientation0/60Hz，trace停止，短时恢复
  guard解除。51文件13648768字节取回并全部长度/SHA256匹配。
- trace完整结束，matched12446，invalid/events_lost/log_buffers_lost/realtime
  _buffers_lost均0。按QPC转换，15:51:10以后DWM有201次SetDisplayMode成功返回，
  没有Blt、Present或OpenResource返回记录；重复创建资源主要是720×1280。
  这限定了下一步的模式建立/primary协商排查范围；仅缺少返回事件不能证明函数
  从未进入，也不能证明新增GPU复制已执行或其完成通知就是根因。

15:53:05请求一次计划恢复重启；保留四方向源码实现，撤下失败的实机候选。
取证位置OUT `gdi-rotation/evidence/`；摘要 `gdi-rotation/test-summary.json`。

恢复任务15:56:53UTC完成，Code0；KMD1a86a02b…、缓存UMD0cd34833…、ICDdee3d580…
均恢复，DWM784实际加载对应模块。CPU1497600、GPU460800、RAM1600000kHz及风扇13
核对通过，默认启用Ready。15:58:13交互只读查询当前/保存均720×1280、方向0、60Hz。
13个恢复文件取回并校验；16:00:03清理完成，删除3个任务、20个本轮目录/文件，
不删除当前缓存基线和共用签名工具。

用户问“VidPN死循环”：目前应表述为模式建立反复重试，不能宣布已证实VidPN内部
死锁。继续核对primary描述、旋转元数据和MPO准入的组合。微软
[primary描述](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/dxgiddi/ns-dxgiddi-dxgi_ddi_primary_desc)
明确NONPREROTATED表示由应用处理方向、primary代表identity；
[miniport旋转](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/supporting-rotation-in-a-display-miniport-driver)
也明确共享primary用未旋转尺寸。不能仅看到CCD source仍720×1280便认定工具或
VidPN错误，更不能再以宽高直接互换或强制source_visible掩盖协商失败。
