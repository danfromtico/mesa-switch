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

### 桌面与 UMD 实测

Tsunami714! 在 HEG-001 上观察到可交互的 Windows 桌面。下表按测试轮次列出结果。依据为测试者提供的日志摘录、11 月 27 日 23:50 的交接记录和源码包 `switch-woa-full-driver-src-20261127.zip`（SHA-256 `456f05727208b2dd9bdf73e8b93468fab4d4df607bef76a67ac21f2fb4b2a17c`）。[OLED 移植与实机测试](https://github.com/Tsunami714a)

| 测试轮次 | 观察结果 |
| --- | --- |
| 移植版 PBUS 轮询修正（补丁 01） | 误报从 617 次降至 0 次。`4631`、`4632`、`4633` 返回 0，随后出现 `467`。标记 `4842` 表示进入 MPO3 地址回调。该轮未观察到异常重启。 |
| DXGI 枚举和首次建卡测试 | 启用 GPU 后，DXGI 枚举到 `Tegra X1 (GM20B)`。KMT 的 DX11 槽位查询当时返回 `0xC000000D`，指定 Tegra 适配器的 `D3D11CreateDevice` 返回 `0x887A0004`。 |
| 修正 UMD 注册后 | 测试者报告 ETW 记录了 `TegraGpuD3DVk.dll` 的映像加载，`umd.log` 出现 `dllmain:attach`、`open-native:enter` 和 `adapter-info`。KMD 记录 `8750:1 / 8751:1 / 8752:1`，对应 `PagingOnly=1`、身份已就绪、返回 flags 为 `0x1`。UMD 要求同时具备 `LUID_VALID` 和 `NATIVE_SUBMIT`（合计 `0x3`），因此在打开 Vulkan 之前返回 `DXGI_ERROR_UNSUPPORTED`。 |
| native `.201` 候选 | 使用 `--display --native-test-version 201` 构建。测试者在卡上核对了 KMD 的版本标记、大小（319,864 字节）和 SHA-256 前缀 `0E880BD9D08685E6`。这份 KMD 在 Tegra 上成功建卡及 DWM 使用硬件渲染的结果仍待实机确认。 |
| NVK 初始化（后续实测） | tgd3d 成功创建 Vulkan 实例，通过 LUID 匹配 Tegra 物理设备。下方列出日志和组件 SHA-256。 |

仓库 INF 的 `UserModeDriverName` 默认是四项 `REG_MULTI_SZ`，均指向随 KMD 提供的 `tegragpuno3d.dll`。桌面安装脚本按实际设备实例找到显示类键，再将 DX10 和 DX11 槽位（索引 1、2）指向 tgd3d 的完整路径。复测时可按本次枚举的 Tegra LUID 查询槽位，并核对进程实际加载的 DLL 路径及完整 SHA-256。测试者曾在该机替换 DriverStore 内的桩 DLL。[KMD 包的 INF](../windows/tegragpurender.inf) · [桌面安装脚本](../tools/tgd3d-default/Install-Default.ps1)

源码包的 KMD 已包含补丁 01，SwBatt 已包含补丁 02。实机有新桥的电压和时钟遥测，仍缺同一轮带令牌的 `CLAIM_RAIL`、`RELEASE_RAIL` 记录。补丁 03 随包提供，包内 `swhid.c` 匹配该补丁的修改前版本。

#### NVK 初始化

Tsunami714! 提供的 HEG-001 日志显示，KMD 上报 `8750:0 / 8751:1 / 8752:3`，对应 `PagingOnly=0`、适配器身份就绪、`LUID_VALID | NATIVE_SUBMIT`。该轮记录窗口内未报告蓝屏。[OLED 实测来源](https://github.com/Tsunami714a)

```text
adapter-flags 3 3
vk-load 1 0
vk-instance 0 1
vk-enum 1 1
vk-pdev 1 1
vk-open:end 0 0
create-device:enter 720911 23
create-device:enter 720911 7
```

`vkCreateInstance` 返回 `VK_SUCCESS`。NVK ICD 枚举到一个物理设备，并通过 LUID 匹配 Tegra。`VulkanAdapter::open` 返回 `S_OK`。随后两次进入 tgd3d 的 D3D `CreateDevice`，创建 Vulkan 逻辑设备的 `vkCreateDevice` 由后续建卡阶段调用。该轮 `step.log` 记录 `4900`（分配入口）54 次、`4910`（分页入口）293 次、`4920`（虚拟地址提交入口）237 次、`4842`（MPO3 地址回调入口）1 次。D3D 建卡的成功返回、GPU 完成 fence 和 DWM 硬件呈现仍待确认。[NVK 初始化及逻辑设备创建](../../src/nouveau/tgd3d/tgd3d_vk.cpp) · [D3D 建卡流程](../../src/nouveau/tgd3d/tgd3d_device.cpp)

第二次 `StartDevice` 尝试报告 `BootPermitStatus=0xC0000022` 和 `StartGateStatus=0xC00000A3`，与一次性许可拒绝一致。当前设备管理器 Code 0 仍待确认，以同一 Tegra 设备实例的 `DEVPKEY_Device_ProblemCode` 和 `DEVPKEY_Device_DevNodeStatus` 为准。[Windows 设备状态查询](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/retrieving-the-status-and-problem-code-for-a-device-instance)

下表列出测试者报告的本轮组件及 SHA-256。

| 组件 | 文件 | 字节 | SHA-256 |
| --- | --- | ---: | --- |
| KMD | `tegragpuw1.sys` | 319,864 | `211acdc935fe704c609c95e475e77a16bcba2e11d5eb33dd219c910590b173b0` |
| tgd3d UMD | `tegragpuno3d.dll` / `TegraGpuD3DVk.dll` | 1,746,944 | `381eca56cc9e5875e6d0a3d376280c88c8f54928f4b8ca1bc3d317084ef27c66` |
| NVK ICD | `libvulkan_nouveau.dll` | 20,441,088 | `45c9c5b0abe32a90fac86a137f515d3144bcae119c233dea00f9c7004b08cc66` |
| SwBatt | `swbatt.sys` | 66,936 | `503703a61c7a5d370185607fb06f09def3738930c78f0b0f674ddd1f44077458` |

#### OLED 的 0x10E 蓝屏

测试者报告同配置的另一次启动发生 `VIDEO_MEMORY_MANAGEMENT_INTERNAL`（`0x10E`），参数 1 为 `0x8`。微软将此子码定义为拆分后的 DMA 缓冲区含有无效引用。故障轮的 `0x10E` 转储、其余参数及对应 DMA 包仍待取得。蓝屏根因和是否属于竞态均待确认。[微软 0x10E 子码表](https://learn.microsoft.com/en-us/windows-hardware/drivers/debugger/bug-check-0x10e---video-memory-management-internal)

### OLED 移植版的一次性启动许可

这份 OLED 移植版在 native 许可之前增加了 `ArmStart` 检查。以下顺序来自源码包的 `01-kmd/windows/render_miniport.c` 和 `01-kmd/windows/native_start_test.c`。Windows 的服务配置 `Start=3` 表示按需启动，PnP 可以在开机枚举到设备时启动驱动。[Windows PnP 启动类型](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/inf-addservice-directive)

1. `StartDevice` 记录 `44` 后，检查 `Services\tegragpurender\Parameters` 下的 DWORD `ArmStart`。值为 `1` 时尝试清零并刷盘。检查失败会记录 `499` 并返回 `STATUS_DEVICE_NOT_READY`。
2. native `.201` 随后检查服务键。使用一次性许可时，`AllowNativeBoot` 子键应当不存在，`AllowDesktopStart` 子键在检查当时须可打开。驱动打开 `%SystemRoot%\System32\drivers\TgGpuBootTest201.bin`，要求文件恰好 4096 字节、首 DWORD 为 `1`，成功消费时将其写为 `0` 并刷盘。若能打开 `AllowNativeBoot`，native 检查会跳过许可文件。[共用 native 许可实现](../windows/native_start_test.c)

仓库的 Lite `.158` 桌面测试脚本会在当前会话创建易失的 `AllowDesktopStart` 子键，并使用 `TgGpuBootTest158.bin`。跨重启测试 OLED `.201` 时，要在 PnP 启动时核对相应子键和 `.201` 文件。可用 Windows 系统启动事件确定开机时间，再用 `devices.txt` 确认登录脚本是否写入记录。[Lite 桌面测试脚本](../tools/tgd3d-trial/Start-Desktop.ps1)

交接中另有一次 `42 → 44 → 481` 的摘录，许可文件首 DWORD 仍为 `1`。`481` 表示进入设备移除回调。定位退出位置需要同次启动的 KMD 加载路径、完整 SHA-256，以及 `Parameters` 中本次更新的 `BootPermitStatus`、`StartGateStatus`、`ResourceStatus` 原始值。

复测探针版时，可用同次启动的 `8750:0 / 8751:1 / 8752:3`、`adapter-flags 3/3` 和 `vk-open:end` 返回值定位进度。指定该次枚举的 Tegra LUID，`D3D11CreateDevice` 返回 `S_OK` 即可确认建卡。DWM 硬件渲染与呈现还需对应的进程证据。[构建参数](../tools/build.py) · [UMD 适配器校验](../../src/nouveau/tgd3d/tgd3d_entry.cpp)

### 首次走完 KMD 启动序列

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

首次黑屏这轮没有留下 `umd.log`，当时 DWM 是否加载 UMD 未能确认。后续测试取得了上文列出的映像加载和 UMD 入口记录。

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
