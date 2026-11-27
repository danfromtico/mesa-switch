# Switch Lite 与 OLED 的 WDDM 查询复审

Switch OLED（HEG-001 / Aula）的 Windows ARM64 适配、移植源码和四项问题反馈来自 [Tsunami714!](https://github.com/Tsunami714a)。

对方的测试环境为 Windows 11 ARM64 23H2（22631）。源码包 `switch-woa-gpu-src-20261127.zip` 的基点为 `68c4dd319f`，SHA-256 为 `6ffe6e7ce7bb959c2354d11ec144380e6cdc663fbe4bc0e7d3ea3f439ecf373e`。本仓库的共用候选基于 `29707703bfe4`。

| 项目 | OLED 源码复核 | Lite / OLED 共用候选 |
| --- | --- | --- |
| ① 子设备枚举 | 移植记录提到一次 28 字节请求。包内既有上报一个面板的路径，也有上报零个子设备的诊断路径。缺少同次启动的原始返回值。 | 一个面板使用 56 字节，前 28 字节描述面板，末尾 28 字节全零。显示路径收到单槽即返回 `STATUS_BUFFER_TOO_SMALL`，缓冲保持原样。纯计算路径上报零个子设备，一个全零槽即足够。 |
| ② 面板查询 | 去掉 `Panel.Dc.owned` 后可以查询已释放显示控制器的面板，但移植版同时去掉了子设备状态与描述符的 UID 校验。 | 记录成功启动时上报的面板及其 POST 标识。枚举、连接状态、描述符和静态模式查询独立于当前 DC 所有权。只向已上报的内屏返回已连接，拒绝未知 UID。 |
| ③ 早期能力查询 | `windows/query.c:88` 仍无条件描述表段，先于 `display=TRUE` 分支。两个宏配置在表段为空时均返回 `0xC000000D`。随包 `tools/build.py` 只为 miniport 相关编译单元传面板宏，实机出包命令未附。 | 显示和纯计算包分别注册静态能力入口。无表段时两者均能返回 `DRIVERCAPS`，`ComputeOnly` 依次为 0 和 1。段、页表等依赖资源的查询仍等待资源就绪。 |
| ④ 能力结构大小 | WDDM 2.6 模型中三个输出分别为 4、584、24 字节。各少一字节时，移植版对 `WDDMDEVICECAPS` 报错，对 `DRIVERCAPS` 和 `PHYSICALADAPTERCAPS` 写出部分结构并报成功。后者还会在检查输入长度前读取物理适配器索引。 | 接受较大的输出，仅写已知结构。过短的输出返回错误。物理适配器索引先验长再读取。固定的 WDDM 版本允许仅带输出的查询。 |

微软规定子设备描述符数组包含一个额外的全零终止项。微软也明确规定，系统会在 `AddDevice` 与 `StartDevice` 之间查询 `WDDMDEVICECAPS`。[子设备枚举契约](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/dispmprt/nc-dispmprt-dxgkddi_query_child_relations) · [WDDM 版本查询时序](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/ns-d3dkmddi-_dxgk_wddmdevicecaps)

## 验证与准入

ARM64 WDK 10.0.26100 构建已链接 WDDM 显示版和 MCDM 纯计算版。使用同批构建对象，在 Apple Silicon 上执行 `test-context`、`test-panel-scanout`、`test-panel-timings`，三个 WDK 用户态模型均返回 0。覆盖表段建立前的查询、单槽和双槽、过短输出的写入边界、DC 释放后的面板标识和错误 UID。

共用改动可进入实机候选。该候选在 Lite 和 OLED 上的驱动加载、冷启动及桌面显示验收均待完成。OLED 冻结的根因仍待确认。判定单槽和旧版能力布局是否需要另行适配，须将同次启动上报的 `NumberOfChildren`、查询类型、输入输出字节数、返回状态和实际加载的 KMD 对齐。

## OLED 实机启动记录

Tsunami714! 报告 HEG-001 的内核显示驱动（KMD）首次走完启动序列。进入系统后内屏一直黑屏，Windows 在整个观察期间持续运行。[OLED 移植与实机测试](https://github.com/Tsunami714a)

| 本轮阶段 | 测试者提供的记录 |
| --- | --- |
| 进入 `StartDevice` 并打开平台桥 | `44 → 460 → 4631:0 → 461`，`TgGpuOpenPlatform` 返回成功。 |
| PMIC 预检与固件加载 | `462 → 4632:0 → 463 → 464 → 4633:0 → 465`，预检与固件加载均返回成功。 |
| GPU 硬件启动 | `466 → 467` 记录硬件启动调用进入和返回。随后出现了 `IsSupportedVidPn` 的进入和返回标记（`4820/4821`）。 |

同轮出现 3 次 `vidpn-config`。下表按测试者提供的 `step.log` 标记计数。

| 标记 | 显示回调 | 本轮出现次数 |
| --- | --- | ---: |
| `4820` | `IsSupportedVidPn` | 15 |
| `4830` | `SetTimingsFromVidPn` | 1 |
| `4832` | `SetVidPnSourceVisibility` | 1 |
| `4834` | `UpdateActiveVidPnPresentPath` | 0 |
| `4840` | `SetVidPnSourceAddress` | 0 |

`dxgkrnl` 已请求设置时序和源可见性。`4832` 写在实际设置可见性的调用之前。`4840` 只覆盖常规地址回调，驱动另有多平面叠加（MPO3）的地址回调。该轮首帧是否提交到内屏及 GPU 的设备管理器状态仍待确认。

测试者报告 UMD 仍未加载，现场没有找到 `umd.log`。要确认 DWM 的加载结果以及 C++ 运行库是否与黑屏有关，还缺同次启动的模块或加载错误记录。

### 早期启动对照

移植者在 HEG-001 上测试了基于共用 WDDM 提交 `74638ab474d` 的带诊断标记版本。以下采用测试者最后逐项对照的 A、B 两次实验。编号来自移植测试包，目前有阶段摘要，尚缺同次启动的原始日志及实际加载 KMD 的完整 SHA-256。当前移植和实机测试由 [Tsunami714!](https://github.com/Tsunami714a) 提供。

| 场景 | 测试者报告的标记 | 结果 |
| --- | --- | --- |
| A，开机时允许 GPU 启动 | `42 → 44 → 40 → … → 460` | 系统挂起。最后记录的是调用 `TgGpuOpenPlatform` 前的标记。 |
| B，系统运行时允许 GPU 启动 | `42 → 44 → 40 → … → 460` | 系统也挂起。两份摘要都没有调用返回后的 `461`。 |
| 挂起后另一次开机，一次性许可已消耗 | `42 → 44 → 499 → 481` | `499` 表示启动许可检查拒绝，`481` 表示进入移除设备回调。Windows 正常启动。 |

这两次获准启动都停在平台预检、GPU 固件加载和面板接管之前。推迟到运行时启用仍复现同一最后标记。具体阻塞的调用、桥接设备当时的就绪状态仍待确认。OLED 图形启动及桌面显示验收保持开放。[平台启动顺序](https://github.com/ruje0504-2/mesa-switch-woa/blob/74638ab474db5eec9a7d5749a4adb28f5a4bff6b/tegra-x1-wddm/windows/platform_adapter.c#L13-L18)

### SwBatt 预检兼容候选

在较早一次测试中，带标记的 KMD 已越过桥接打开的挂点，到达预检返回标记 `463`，在固件加载标记 `464` 之前走清理路径，设备管理器报 Code 43。测试者后来报告当时的预检错误为 `0xC0000022`。

候选按已打开的设备识别参考工程的 `\Driver\swbatt` 控制设备。该桥随驱动装载和卸载管理生命周期，通过已引用的文件对象访问。SwitchFan 继续使用目标设备 PnP 通知。SwBatt 拒绝带所有权标记的地址查询时，KMD 在同一文件和原有超时内尝试旧版只读查询。确认旧协议后，可选的电源轨认领按旧格式发送。桥未实现认领时不会登记所有权，真正的权限错误仍会中止启动。

使用 ARM64 KMD 对象和 DanilKhmyrov 原版 SwBatt 请求处理函数做离线对照，I²C 和温度使用模型输入。原版 KMD 在首条查询返回 `0xC0000022`。候选 `976da83d44b` 在模拟 PMIC 地址 `0x31` 和 `0x33` 上均通过预检，未知温度和探测不到 PMIC 仍会中止。要将最新 OLED 实机结果与该候选对应，还需核对实机加载的 KMD 和 SwBatt 完整 SHA-256。[参考 SwBatt 请求处理](https://github.com/DanilKhmyrov/windows-switch/blob/ebff45d3038d1e321f3dc83dc7eea2dbe6cf79c1/drivers/swbatt/swbatt.c#L819-L885)

### 独立的 OLED GPU 桌面实测

[DanilKhmyrov](https://github.com/DanilKhmyrov) 的 `windows-switch` 项目记录了HEG-001 在 Windows 11 ARM64 23H2（22631）上的测试。该项目使用 NintendoSwitchPkg UEFI 和 SwBatt 提供的 PMIC 桥。较早的显示 KMD 在登录后切换到 GPU 时达到 Code 0，DWM 加载 tgd3d 和 NVK，内屏显示竖屏桌面。报告也记录了随后在桌面负载下挂起的情况。[显示接管与 Code 0](https://github.com/DanilKhmyrov/windows-switch/blob/ebff45d3038d1e321f3dc83dc7eea2dbe6cf79c1/README.md#L525-L540) · [测试版本和后续稳定性](https://github.com/DanilKhmyrov/windows-switch/blob/ebff45d3038d1e321f3dc83dc7eea2dbe6cf79c1/docs/notes-for-mesa-switch-woa.md#L3-L59)

两套 KMD 都打开 `\Device\SwitchBacklight`。早期实测所用的 `a5d80906` 在 `ZwCreateFile` 成功后保存资源并返回。`74638ab474d` 的共用实现还引用文件对象，并在相应条件下注册目标设备 PnP 通知。先前收到的移植包关闭了这项注册。对照当前实机时，需核实实际加载的 KMD 和桥接驱动，区分打开设备、取得对象及注册通知各自的返回状态。这三个调用的先后关系为定位 `460` 提供了检查点。[早期 KMD 的桥接路径](https://github.com/ruje0504-2/mesa-switch-woa/blob/a5d80906e64195606342e976df34c7f755576cd5/tegra-x1-wddm/windows/platform_io.c#L11-L27) · [共用 KMD 的桥接路径](https://github.com/ruje0504-2/mesa-switch-woa/blob/74638ab474db5eec9a7d5749a4adb28f5a4bff6b/tegra-x1-wddm/windows/platform_io.c#L628-L647) · [SwBatt 桥设备的创建](https://github.com/DanilKhmyrov/windows-switch/blob/ebff45d3038d1e321f3dc83dc7eea2dbe6cf79c1/drivers/swbatt/swbatt.c#L941-L956)
