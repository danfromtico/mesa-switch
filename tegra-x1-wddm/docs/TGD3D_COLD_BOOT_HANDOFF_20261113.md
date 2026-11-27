# GM20B 图形与 H.264 硬解基线

2026-11-19 选定 `video-pool-900b8d5d1b2c`，GPU Code 0，已授予永久开机许可。测试播放器加载官方 x64 wiliwili 1.6.0 内置的 mpv，使用 D3D11VA 在 GM20B 上解码两段 720p H.264。两段共 480 帧，逐帧校验和 NVDEC 提交均匹配，窗口显示并正常退出。ARM64 原有解码与 FL11_1 绘图回归通过。[源码与组件哈希、实机验收记录](dxvk-games-20261118/mpv-h264-output-pool-baseline.json)

## 此前的 1080p 硬解与横屏验收


**1080p H.264 六帧慢速窗口实机验收通过。** NVDEC 经 D3D11VA 解出 1920×1080 画面，NV12 纹理池为 1920×1088。六帧均与独立参考逐字节相符。实屏窗口显示彩色图案，六次 Present 成功，末帧 230,400 个像素各通道最大差值为 1。客户端计时 16.093 秒，含末帧停留 8 秒，随后正常回到桌面。[1080p 验收证据与完整 SHA256](NVDEC_1080_D3D11VA_20261117.json)

该轮 KMD／UMD／ICD 为 `9ef86d7e`／`c8a8eb9a`／`e563a81d`。90° 逐行横屏和 Handheld 电源配置已复核，720p 可用组合保留为恢复目标。2K、4K 已有独立实现，暂缓实机测试。连续播放和高负载重启另案跟进。[版本表](NVDEC_INTEGRATION_20261116.json)

## 上一轮 720p 实机验收


**720p H.264 硬解及窗口实屏功能验收通过。** 该轮基线为 de8bb90b KMD、c7ef263c UMD 和 e563a81d ICD。D3D11VA／NVDEC 解码的六帧 NV12 全部匹配独立参考，GPU 转色后的图案显示在 640×360 的 DWM 窗口中。慢速观察的六次 Present 均返回成功，末帧停留 8 秒，解码会话、GPU 事件、DWM 刷新和窗口关闭均有完成回执。客户端计时 15.109 秒。[组件哈希、逐帧结果与实屏证据](NVDEC_720_COMPOSITOR_OPEN_20261117.json)

DWM 通过旧版 DDI `0xA0009` 成功打开两块视频缓冲，pitch 均为 2560。当前和已保存桌面均为 90°、1280×720，面板信号为 720×1280。两次无人工间隔的运行发生意外重启，轻量复测的最后一条记录停在第五次 Present。常规连续播放稳定性保留为后续任务。[重启记录](LOAD_STABILITY_FOLLOWUP_20261116.json)

### 前一轮 864ff770 UMD 的结果

此前工作组合为 de8bb90b KMD、864ff770 UMD 和 e563a81d ICD。UMD 在五补丁源码上单独移入 358e0954 的运行时提交修正。六帧 720p H.264 经 D3D11VA／NVDEC 硬解，NV12 逐帧匹配，两张 640×360 GPU 读回的 RGB 通道与独立参考最多相差 1。两次完整运行均有六次重定向翻转、解码会话关闭和两个 GPU 上下文释放。实屏重播出现黑窗，随后整屏变黑，窗口退出后桌面恢复。720p 视频实屏验收失败。[当前组件、像素、关闭和实屏记录](NVDEC_720_RUNTIME_WINDOW_20261117.json)

独立跟踪捕获到 DWM 的旧版 DDI `0xA0009` 六次打开资源返回 `0x887A0004`，驱动随即释放各次 allocation，均未观察到该 allocation 的驻留请求。追踪轮在末帧停留期间失去完整记录，系统意外重新启动。重启根因未定。重播客户端记录 9.297 秒，包含末帧停留 2 秒。DWM 保持同一进程，两次完整运行的 GPU 渲染序号在设备销毁时都是已提交 10、已完成 10。当前与已保存模式均为 90°、1280×720，面板信号为 720×1280。Handheld、DFLL、EMC、风扇和 85% 充电限制已核对。1080p、2K、4K 后续集中验收。

## 五补丁组合的早期窗口记录

2026-11-17，当前 de8bb90b KMD、45683c21 UMD 和 e563a81d ICD 已通过 720p H.264 六帧 D3D11VA 硬解及 NV12 读回。同步 libavcodec 使用 17 个表面，六个 NVDEC 同步点逐一到达目标。六帧可见像素合计 8,294,400 字节，逐帧转换为相同的平面顺序后，哈希均与独立软件解码一致。解码循环实测 860 毫秒，会话已正常关闭。[组件、样片和逐帧证据](NVDEC_720_WINDOW_BOUNDARY_20261117.json)

在同一驱动组合上，交互窗口首帧经过 D3D11 像素着色器转换、GPU 复制和阻塞式 staging Map。640×360 共 230,400 个像素，所有 RGB 分量与独立 BT.601 有限范围参考最多相差 1 个 8 位色阶。应用随后进入第一次 `Present(1, 0)`，没有留下返回记录，系统意外重启。此前普通路径和加入首帧读回的诊断路径均停在此调用。重启后 DWM 加载相同的 UMD／ICD，Handheld、DFLL、EMC、风扇及 85% 充电限制已核对。解码窗口合成、实屏和本轮完整关闭仍待验收。[首帧像素、日志和清理](NVDEC_720_FIRST_SUBMIT_20261117.json) · [当前组件与五个补丁](TGD3D_PRESENT_BASELINE_20261117.json)

使用解码设备实际所属的 DXGI 工厂创建交换链后，实机确认该工厂和原枚举工厂是两个不同的 COM 对象。校验后的首帧哈希与上一轮完全相同，首次 Present 仍没有返回记录。系统事件 41 的 BugcheckCode 为 0，未发现本次转储。已取回故障归档，重启后 DWM 加载相同的五补丁组合。[客户端提交、组件与故障记录](NVDEC_720_DEVICE_FACTORY_20261117.json)

双缓冲 DWM 合成窗口在创建交换链时失败。实机 D3D11.1 DDI `0xB000F` 的第一个 640×360 表面获得 921,600 字节 allocation，随后驱动在驻留前释放了它并返回 `0x887A0004`。公开的 `CreateSwapChainForHwnd` 返回 `0x887A0005`，解码和 Present 均未开始。完整 ETL 与已安装源码将拒绝点定位于 displayable 资源的提交 device 归属检查；输出中没有这次 kernel resource handle 的数值。进程退出时 DWM 仍为 1216。取回归档后另有一次意外启动，DWM 1384 重新加载同一组合，原因未定。[逐调用记录、匹配源码与系统事件](NVDEC_720_COMPOSED_FLIP_20261117.json)

## XP 项目只读源码索引

1080p 的可见区域、编码表面和 NV12 平面边界见配套的 KMD、UMD 改动。原有 GM20B、NVK／NAK、扫描输出与 Windows 宿主索引继续适用。[1080p 源码提交](https://github.com/ruje0504-2/mesa-switch-woa/commit/fddf6c6b06de5cbf3aa6389b528a8b85dc6c42b9) · [实机证据](NVDEC_1080_D3D11VA_20261117.json)

下表的链接固定到 Windows ARM64 实际构建所对应的版本。XP 项目可只读查阅硬件格式、执行顺序和验证样例。XP 专用驱动、WOW64 和 D3D9 调用桥由 XP 侧独立实现。两侧的运行结果分别验收。

| 范围 | 源码入口与依赖 | 已有证据 |
| --- | --- | --- |
| GM20B 硬件核心 | 页表项和地址映射在 `core/gm20b_mmu.c`，GPFIFO 项编码在 `core/gpfifo.c`。T214 侧的通道、MMU 和 GR 上下文分别在 `platform/t214_frontend.c`、`t214_mmu.c`、`t214_gr_context.c`。寄存器 I/O、物理内存分配、缓存一致性、供电和固件来源须由宿主提供。[核心](https://github.com/ruje0504-2/mesa-switch-woa/tree/4dd80c7920991d51462fc5ce1f120b4e2a23206a/tegra-x1-wddm/core) · [平台](https://github.com/ruje0504-2/mesa-switch-woa/tree/4dd80c7920991d51462fc5ce1f120b4e2a23206a/tegra-x1-wddm/platform) | 当前 de8 KMD 上的首帧 Draw、复制和真实 GPU 读回覆盖这条组合路径。各核心文件没有逐函数实机覆盖声明。[图像与组件](NVDEC_720_FIRST_SUBMIT_20261117.json) |
| NVK、NAK、NIL | NVK 的 `nvk_cmd_draw.c` 记录绘制，`nvk_shader.c::nvk_compile_nir` 调用 NAK。`compiler/nak/sm50.rs` 实现 Maxwell 指令编码，包括 SM53。`nil/image.rs`、`nil/tiling.rs` 描述图像布局。设备信息、内存和队列由 NVKMD 后端提供。[NVK](https://github.com/ruje0504-2/mesa-switch-woa/tree/7823d6419de624bb7ba555fa6a5867b6f79607a4/src/nouveau/vulkan) · [NAK](https://github.com/ruje0504-2/mesa-switch-woa/blob/7823d6419de624bb7ba555fa6a5867b6f79607a4/src/nouveau/compiler/nak/sm50.rs) · [NIL](https://github.com/ruje0504-2/mesa-switch-woa/tree/7823d6419de624bb7ba555fa6a5867b6f79607a4/src/nouveau/nil) | DXBC → SPIR-V → SM53 的顶点／像素着色器各有离线产物哈希。实机另有同源着色器和已安装 ICD 的首帧像素对照；GPU 机器码未在运行中转储。[编译产物](NVDEC_720_WINDOW_BOUNDARY_20261117.json) · [实机像素](NVDEC_720_FIRST_SUBMIT_20261117.json) |
| 扫描输出 | 在已验收的逐行版本中，`platform/t214_dc.c::tg_t214_dc_latched_surface` 读活动寄存器组，`windows/panel_scanout.c` 接 VidPN、主表面、VBlank 和翻转通知。保留本机面板／DSI 时序。预旋转入口在 UMD 的 DXGI Blt 路径。[DC](https://github.com/ruje0504-2/mesa-switch-woa/blob/c0c6647cec87fcc8d40951d8040ced5d1bbd5f2f/tegra-x1-wddm/platform/t214_dc.c) · [WDDM 扫描输出](https://github.com/ruje0504-2/mesa-switch-woa/blob/c0c6647cec87fcc8d40951d8040ced5d1bbd5f2f/tegra-x1-wddm/windows/panel_scanout.c) · [UMD](https://github.com/ruje0504-2/mesa-switch-woa/blob/c1f25986eee51c0ee01088c3ecb3c9508dea2479/src/nouveau/tgd3d/tgd3d_dxgi.cpp) | 固定 90° 实屏验证对应 e952／5b66／95b9 组合，逻辑 1280×720、物理 720×1280、pitch 2880。原生 Win32 vkcube 经 NVK 绘制和 GDI 交给 DWM，运行 300.289 秒。DC 列扫描和四方向稳定性仍待验证。[原组合与逐行记录](ROW_SCAN_BASELINE_20261116.json) · [窗口与时长](VKCUBE_ACCEPTANCE_20261116.md) |
| WDDM／KMT 宿主 | `nvkmd/windows/kmt_native.c` 按 LUID 打开适配器并调用 D3DKMT，`nvkmd_windows_queue.c` 管提交和 fence；tgd3d 的 `tgd3d_runtime_queue.cpp` 和 `tgd3d_dxgi.cpp` 使用 D3D runtime 回调。KMD 的 `windows/render_miniport.c`、`paging.c`、`submit_ddi.c` 接 Dxgkrnl／VidMm／VidSch。allocation、GPU VA、上下文、监控 fence、DXGI 呈现令牌和回调生命周期属于 Windows 宿主。[KMT](https://github.com/ruje0504-2/mesa-switch-woa/tree/7823d6419de624bb7ba555fa6a5867b6f79607a4/src/nouveau/vulkan/nvkmd/windows) · [UMD](https://github.com/ruje0504-2/mesa-switch-woa/tree/7823d6419de624bb7ba555fa6a5867b6f79607a4/src/nouveau/tgd3d) · [KMD](https://github.com/ruje0504-2/mesa-switch-woa/tree/4dd80c7920991d51462fc5ce1f120b4e2a23206a/tegra-x1-wddm/windows) | 五项修复分别记录提交、回调控制和实际运行范围。兼容性控制中的模拟设备移除没有在 GPU 上注入。在 864ff770 UMD 上，11.1 生产端通过真实 runtime callback 提交了六个含 WrittenPrimaries 的命令。c7ef263c UMD 的 DWM 接收端已打开两块视频缓冲，跟踪中有 13 次携带一个 WrittenPrimaries 的 DWM 提交成功返回。[五补丁](TGD3D_PRESENT_BASELINE_20261117.json) · [11.1 回调修正](https://github.com/ruje0504-2/mesa-switch-woa/commit/358e0954ce38cca6052e97054e5ecfe2d54a6a0d) · [0xA0009 接收端修正](https://github.com/ruje0504-2/mesa-switch-woa/commit/8549e9e398f6cdb4e064fc30bd7c06a5243be893) · [原故障](NVDEC_720_RUNTIME_WINDOW_20261117.json) · [修正后的实机跟踪](NVDEC_720_COMPOSITOR_OPEN_20261117.json) |
| NVDEC／Host1x | `src/nouveau/nvdec/` 包含 H.264 参数、固件接口和 NV12 布局。`platform/t214_nvdec.c` 管保留通道和同步点。`windows/nvdec_backend.c`、`nvdec_memory.c` 对接 Windows 资源和 DMA；`tgd3d_video.cpp` 接 D3D11 视频 DDI。[解码核心](https://github.com/ruje0504-2/mesa-switch-woa/tree/4dd80c7920991d51462fc5ce1f120b4e2a23206a/src/nouveau/nvdec) · [Host1x](https://github.com/ruje0504-2/mesa-switch-woa/blob/4dd80c7920991d51462fc5ce1f120b4e2a23206a/tegra-x1-wddm/platform/t214_nvdec.c) · [视频 DDI](https://github.com/ruje0504-2/mesa-switch-woa/blob/7823d6419de624bb7ba555fa6a5867b6f79607a4/src/nouveau/tgd3d/tgd3d_video.cpp) | 720p H.264 的六个 Host1x 目标全部到达，NV12 与独立软件解码逐帧一致，会话关闭。更高分辨率待集中验收。[后端](NVDEC_720_NATIVE_BACKEND_20261117.json) · [D3D11VA](NVDEC_720_WINDOW_BOUNDARY_20261117.json) · [720p 实屏与正常关闭](NVDEC_720_COMPOSITOR_OPEN_20261117.json) |

当前 KMD 在 720p 已验证的 `4dd80c7920991d51462fc5ce1f120b4e2a23206a` 和冻结平台补丁上加入 `fddf6c6b06de5cbf3aa6389b528a8b85dc6c42b9` 的 1080p 改动。ICD 与 UMD 底本为 `7823d6419de624bb7ba555fa6a5867b6f79607a4`，UMD 在已验证的 `358e0954ce38cca6052e97054e5ecfe2d54a6a0d`、`8549e9e398f6cdb4e064fc30bd7c06a5243be893` 两项修改上移植同批解码改动。历史逐行 KMD 为 c0c6647c 加已记录的 EMC 修正。XP 侧可按提交和相对路径只读查阅，并按文件核对 SPDX、NOTICE 及上游来源。T214 电源部分含 GPL-2.0-only，NVIDIA 固件有单独条款。[冻结输入与组件哈希](NVDEC_1080_D3D11VA_20261117.json)

以下为此前轮次的记录。

当前使用包含五个呈现与资源管理补丁的基线。2026-11-17 20:32 UTC，已在计划启动后读到 DWM 加载的 45683c21 UMD 和 e563a81d ICD，KMD 为 de8bb90b，显示设备状态为 0。后续实机观察保留这套组合及故障现场。[五项基线及验证范围](TGD3D_PRESENT_BASELINE_20261117.json)

普通窗口任务分发后发生意外重启，缺少窗口就绪、正常退出及对应驱动跟踪事件。本轮已停止，720p 预览没有运行。20:38 UTC 核对了 11 项安装文件，重启后的 DWM 仍加载所选组合，原始记录及取证归档保留在设备上。[故障范围、部分跟踪和当前组件](TGD3D_PRESENT_BASELINE_20261117.json)

以下为该组合启用前的观测。

2026-11-17，提交时的资源归属、两项描述符分配优化、Fence 移除识别和释放失败处理已接入解码分支。候选 NVK 保留了已安装驱动区分 tgd3d 和原生 Vulkan 的活动标记。两套 ARM64 控制在 Switch 上通过。独立 Win32 `vkcube` 核对候选驱动 UUID 和 GM20B 的 LUID 后，在 320×240 窗口完成 120 次成功呈现，进程运行 8.19 秒。首次观察因加载已安装 ICD 提前结束。此次未新增实屏目视结果。[源码提交、组件完整哈希及实机记录](TGD3D_PRESENT_OWNERSHIP_20261117.json)

实机 DWM 继续使用 3a8ed9ff UMD 和 95b97df4 ICD，内核为 de8bb90b。测试前后的启动时间、DWM PID 和三个组件身份一致，两项临时任务及上传文件已清理。候选 UMD 的普通 DWM 呈现留待下一轮 720p 窗口验证。旧路径在等待 GPU 空闲失败后的资源退休仍待处理。[版本表](NVDEC_INTEGRATION_20261116.json)

2026-11-17，使用 Windows D3D11VA 的同步 libavcodec 客户端完成了 6 帧 720p H.264 硬解。17 个表面的帧池由公开接口给出。6 次 NVDEC 提交的同步点均到达目标，会话正常关闭。按显示顺序读回的 8,294,400 字节 NV12 与独立软件参考完全一致。[组件、源码、逐帧哈希与实机结果](NVDEC_D3D11VA_720_20261117.json)

Media Foundation 已完成视频设备查询、解码器创建和缓冲分配，仍在首帧 EndFrame 返回错误。独立窗口测试期间系统非正常重启。恢复出的跟踪记录了两帧硬件完成和预览资源创建，未取得 GPU 色彩转换输出或首次 Present 的结果。事件 41 的 BugcheckCode 为 0，未找到本次崩溃转储。窗口与实屏验收待完成。[故障范围与现场记录](NVDEC_D3D11VA_720_20261117.json)

重连后实机加载了 3a8ed9ff UMD、de8 KMD 和 95b ICD。当前及保存的桌面均为 90°、1280×720，面板信号为 720×1280。已核对 11 项文件身份、Handheld、DFLL、EMC、风扇及 85% 充电限制。测试任务、观察程序和临时结果已清理，恢复文件可用。视频接口仍按进程启用。1080p、2K、4K 后续集中验收。[版本表](NVDEC_INTEGRATION_20261116.json)

以下为历史轮次。[早期原生 KMT 后端验收](NVDEC_720_NATIVE_BACKEND_20261117.json)

2026-11-17：当前保留修正版374b KMD（SHA256 `2b4fa062…`）、045时钟提供者、9eb资源表及原5b／95b用户态组合。增量替换已实际完成：相同表不重写、提供者不重装、原UMD／ICD注册路径保留；本次修正版只做一次计划启动。DWM、11项完整身份、90°当前／保存配置和Handheld／DFLL／动态GPU与EMC／风扇／85%充电限制已核对，未重复实屏或vkcube验收。

此前838cab候选的真实请求仍在通道11所有权检查处失败，GET=`00121010`、PUT=`0`、START=`108e00a8`、END=`a800a800`；未创建后端、启动固件或DMA，输出0帧。非零边界不足以证明通道可接管。另发现58dc依据旧源码把DMAEND误改为绝对地址，NVIDIA后续提交0e43b8da明确要求相对偏移；374b已纠正，并通过旧实现失败／新实现通过的边界控制和ARM64构建。错误提交路径从未在本机执行，不能归为该所有权拒绝的原因。本次未重跑未改变的失败检查。

[当前修正及组件记录](NVDEC_RELATIVE_DMAEND_NATIVE_20261117.json)、[通道实测与错误假设纠正](NVDEC_INCREMENTAL_POINTER_NATIVE_20261117.json)、[前一保留组合](NVDEC_RETAINED_CHANNEL_NATIVE_20261117.json)。取证归档已核对并清理对应临时观察文件；活动目录、用户态目录及恢复输入仍在使用。720p／1080p硬解未验收，仅主窗口复核，无外部审核。下文是各历史轮次的状态，不能替代上述当前身份。

2026-11-17 13:20 UTC：候选切表启动实际加载4568字节旧DSDT，磁盘上的4993字节候选表未生效，
GPU不存在，原生启动仍关闭。新KMD与时钟提供者只暂存，均未选择或执行，未运行解码。
已恢复原表及启动配置，直接复用原驱动；11项组件、DWM、90°、Handheld／EMC／风扇／
85%充电限制核对完成，候选包、任务及临时文件已清理。本轮只做主窗口复核。
匹配实机加载器的调用方忽略可选表加载函数的错误返回；实际失败分支及启动时读取结果
仍未捕获，不能归因于新解码代码。通过必要验证的模块沿用已验证组合继续升级，
发生实际枚举、显示、电源或生命周期异常时回退。720p／1080p仍未验收。
[启动表失配与恢复记录](NVDEC_ACPI_OVERRIDE_RECURRENCE_20261117.json)。以下轮次为历史证据。

每个模块修复完成、必要验证通过且没有明显回归后，并入源码基线并同步组件哈希与回退记录；未经硬件验证的候选不提升为安装基线。
电池供电、CPU-Z同时运行期间关闭vkpeak出现卡死／重启，列为
[后期待修](LOAD_STABILITY_FOLLOWUP_20261116.json)，目前根因未确认；未重放负载、未改驱动或电源策略。
打开左下角天气、打开浏览器时的重启一并归入该组，分别保留触发条件；共同根因仍是待验证假设。
继续720p解码目标，不把该后期任务重新作为当前阶段前置条件。

先完成720p真实NVDEC／D3D11VA／NV12及窗口呈现链，再推进1080p；
2K／4K仅保留已有独立实现，未检测、是否整合待定，不能标成当前基线支持。

最新修正（2026-11-17 UTC）：已确认旧SMMU旁路判定错误要求客户端translation-enable全部清零。
实际安装的BL31为49755字节，SHA256 `c2d048aaba276b2bc8d4b3f565f89d765a9851a522ced0985e551558c7c9e104`，
与固定ATF源码构建记录一致；该文件反汇编在偏移0x7524处明确把五组enable写为`ffffffff`。
Noble驱动通过SWGROUP ASID_ENABLE单独关闭转换，上游Tegra210说明也确认这种旁路语义。
因此修正为检查HC／NVDEC的ASID_ENABLE，不再把全1客户端使能误判为读失败；
启用的ASID、无效ASID及MC读取仍被拒绝，SMMU寄存器不作写入。
全局CONFIG为何读回全1仍未查明，原始值、REGS_INVALID及BOOT_REGS_VALID未置位的诊断保持。
详见[旁路判定及启动固件证据](NVDEC_SMMU_BYPASS_20261117.json)。
ASan／UBSan平台控制、WDK资源／失败清理控制和三个ARM64对象编译通过；未构建或安装新整包，未执行硬件DMA或解码。

04:14:51 UTC重新核对原11项文件全部匹配，Code0，DWM1384加载原5b／95b，默认启动任务正常。
但本次04:06开机后的电源运行状态为固定回退mode7，DFLL未启用／错误`0xc0000483`，GPU固定460800kHz；
EMC实际2500000kHz、flags327、transition11。充电中心85及自动风扇保持。
这不是健康Handheld运行状态，也不是本轮未安装代码造成的回归；原因未归属。
本轮没有请求重启、改配置、清除错误锁存或变更频率，未上传探针或创建实机任务。
后续硬件候选前需确认电源运行条件，同时在本地接通真实Host1x时钟归属和共存传输。

前轮实机进展（2026-11-17 UTC）：已到达ed09候选KMD的真实客户端查询。
先导入包再切表的启动再次出现设备问题码为空；这次取回了实际加载的4568字节DSDT，
SHA256为`74070d7c6455f3f421610c4369ba483c84e39006755d03c7fcaed8a4f131b5bc`，其中没有GPU0／SWOA0F01，
FAN0片段也没有ACTMON资源；磁盘文件仍为827b。该轮尚未选择候选KMD，不能归因于ed09启用。
完整present列表未取回，不补写该项观察。暂存日志全零、安装日志有NUL区；
一个耗时异常的只读取证进程已按PID及创建时间核对后停止，后改用有界原始字节读取。
新41／6008记录的早期时间与请求时间尚未对应清楚，不声称计划请求代表正常关机。

已把包暂存与切表重启分成两个动作：StagePackage核对Driver Store中13项文件及SetupAPI登记，
匹配时直接复用；StageCandidate只复核既有暂存结果并切表，不再导入包。
恢复前也先核对实际加载的原DSDT。源码提交`1e2ecbc2c478593c416c9ac653d087a79aff4f9b`，
15项编排替身控制和实际SetupAPI往返核查通过，详见[暂存复用修正](NVDEC_STAGING_REUSE_20261117.json)。
新轮确认没有发出导入请求，实际加载827b、GPU存在／Code43、ACTMON=0，然后才选择ed09。
这一结果不能证明此前覆盖表未生效的根因已经修复；旧失败仍保留。

02:59:04 UTC真实KMT查询返回：LUID`00000000000a9805`，11组可选资源全部映射，mask=0x7ff。
全局SMMU配置和客户端使能0／1／3均为`ffffffff`，HC／NVDEC／NVJPG ASID均为0，
`resource_status=4`、`client_flags=0`；尚不能据此确认DMA路由，未启动固件、DMA或解码。
启用后的EMC flags367／全部错误0，DWM1388使用原5b／95b，未观察到先前9资源轮的ACTMON-3。
完整原始值、组件与恢复记录见[真实客户端查询](NVDEC_CLIENT_QUERY_20261117.json)。
这只是启动快照，不是实时DMA归属、GPU完成或720p解码验收。

03:11:35 UTC完成恢复，03:15:18确认原E95／5b／95b、0502实际DSDT、Handheld／85%／自动风扇正常，
DWM1420加载原模块，EMC错误清零。03:13:57只读确认当前与持久化1280×720／90°、物理720×1280；
没有新的实屏人工验收。03:17:50确认原11项文件、五秒任务及Code0，候选oem0包、两轮目录、上传包和观察任务均已清理。
本批四次明确计划重启请求：失败轮切表／恢复，复用轮切表／恢复；没有因SSH观察超时追加重启。
候选未提升为安装基线，720p／1080p仍未完成硬解验收。

前轮留下的SWGROUP旁路判定已由上述固件及源码证据修正；全局配置读权限仍未确认。
下一步闭合Host1x时钟归属和720p真实解码，重新读取实时路由并保留资源至真实完成。
不得把全1读数解释为Mariko没有SMMU，不写SMMU绕过归属，也不按CPU替身扩建后续链路。
现代UMD扩展、方向／负载故障重放继续冻结；新增代码待Gemini4 Carbon High于2026-11-20T23:53:18Z额度恢复。
先前EMC阶段修正与ACPI-only正向对照仍见[NVDEC_ACPI_HANDOFF](NVDEC_ACPI_HANDOFF_20261117.json)
和[ACPI枚举对照](NVDEC_ACPI_ENUMERATION_NATIVE_20261117.json)，不扩大各自证据范围。

上一轮本地进展：按上游NVK的逐行H.264布局修正scratch容量：当前17个表面索引在720p需要
4003840字节共址数据，旧512KiB固定区域不足；history为61440字节、MB history为8448字节。
保留会话工作区，不随码流缓冲变动；改变coded尺寸必须先安全关闭／重置，取消失败仍保留旧资源。
容量／尺寸变更控制修正前7项失败，修正后ASan／UBSan通过，六个依赖ARM64对象通过。
见[参考帧工作区容量](NVDEC_SCRATCH_CAPACITY_20261116.json)，包含固定上游提交、MIT来源、完整哈希和验收边界。
这是本地契约修正，尚无原生解码结果；不将该缺陷归因于用户之前的负载或方向重启。
固件来源已进一步核实：交付中的128000字节镜像与linux-firmware固定提交的Tegra210镜像逐字节相同，
WHENCE标为L4T-r32.7.6，完整SHA256仍为7b5ac5ad66dad47e77f2a056991e422dd2630fa17e0a57a5874af492efa0f909。
见[固件来源及加载条件](NVDEC_FIRMWARE_PROVENANCE_20261116.json)。已区分单镜像与安全引导/WPR两条加载路径，
实际同机Noble配置和B01运行结果仍未取得。该外部blob未提交；其再分发条款包含开源操作系统条件，Windows公开包的固件再分发权限未确认。
实机恢复连接后先完成客户端／PMC路由查询及选择前EMC核查，再连接真实时钟归属并启动固件、进行720p解码。

历史连接阻塞（本轮已解除）：此前客户端查询、继承电源、DMA／工作区三轮未能取得新的认证设备连接。
可独立处理的上述修正已完成必要本地验证；后续依赖真实客户端／PMC状态及EMC，不再按CPU替身扩大引擎或传输实现。
恢复条件是连接可用、已知SSH主机身份匹配、主聊天独占并重新读取安装哈希及电源状态。
720p／1080p原生解码仍未验收；Gemini独立审核仍待2026-11-20T23:53:18Z额度恢复。

上一模块：NVDEC分配已改用现有Dxgkrnl私有内存回调，保留真实分配句柄，
释放失败转入退休列表重试，不因上层清空描述符而丢失所有权。加入HC／NVDEC客户端路由复核，
只接受已明确未翻译的路由，不写SMMU配置。失败清理控制、ASan／UBSan路由控制及ARM64对象构建通过；
见[跟踪DMA修正](NVDEC_TRACKED_DMA_20261116.json)。真实时钟提供者、DMA路由和共存传输仍未接通，未安装或提升硬解基线。

连接证据修正：20:02 UTC确认Mac的物理IPv4仅有Wi-Fi en0的192.168.124.6/24；旧192.168.8.6默认走代理隧道。
绑定物理接口直连旧地址超时，一次有界当前网段SSH发现没有候选。因此下方旧“对端关闭”记录只能证明连接失败，
不能证明Switch的SSH状态、重启或当前画面。本轮没有实际执行设备命令，最后完整恢复证据仍为18:12回执。
连接恢复后先按已知SSH主机身份及组件哈希核对设备，再做客户端／PMC查询和候选选择前EMC快照。
继续本地720p workspace／DPB契约审查，不重复压力负载或方向测试。

上一模块：已修正B01继承电源路径，检查NVDEC／NVJPG上电、钳位、复位及门控状态，
不再将“已上电”本身视作其他驱动占用；不关闭继承分区，也不执行T210专用MBIST。
Host1x改为要求实际时钟所有者的取得／释放接口，通道与同步点由父层明确保留；
退出前处理复位／MC排空、通道DMA指针退休和失败保留，超时后可重新取得已清理的通道。
ASan／UBSan控制及三个依赖ARM64对象构建通过，见[继承电源修正](NVDEC_INHERITED_POWER_20261116.json)。
实际FAN0时钟提供者、DMA分配和共存传输尚未接通，不能以CPU替身作为这些接口可用的证明。
本轮只读SSH仍在命令执行前被对端关闭，没有安装或重启；下面的客户端查询候选也未部署。
继续本地DMA及720p scratch／DPB契约核对；连接恢复后先取得客户端／PMC及选择前EMC实值。

客户端路由候选（上一轮）：用户已授权休息期间自主继续。客户端SMMU路由、PMC钳位／电源命令、MC状态
已补入同一次启动快照，旧48／112字节查询保持兼容，新152字节查询仅返回实际读取的状态。
11组资源候选已完成ARM64构建、SYS本地签名、ACPI保持性检查及资源／查询控制。
见[客户端路由候选与契约](NVDEC_CLIENT_ROUTE_20261116.json)，其中保留完整哈希、失败边界与PDB身份差异。
这轮三次只读SSH尝试均在握手／认证前被对端关闭，未上传、安装、启用或发出重启命令；
没有新的实机快照，当前可引用的最后完整恢复证据仍是下面的18:12回执。
候选尚缺Windows目录签名验证及实机路由读取，不提升安装基线，也没有完成NVDEC固件启动或解码。
继续本地处理继承电源、Host1x时钟归属及共存传输；连接恢复后用新候选补齐路由与选择前EMC证据。
保持既有720p→1080p顺序，新增代码仍待Gemini4 Carbon High额度恢复，不唤醒用户重复索要常规许可。

上一轮原生核查已实际加载E95冻结源码加资源核查补丁的配套KMD／ACPI，26项候选／回退包文件通过Windows签名验证。
真实KMT查询确认全部9组PnP资源已映射，但全局SMMU配置读回`ffffffff`，按设计判定快照无效；
Host1x／NVDEC／NVJPG的ASID读回0，不能单独据此声称DMA路由可用。
NVDEC及NVJPG电源分区在启动时已上电，原交付默认未上电的前提需改为明确的继承状态与所有权处理。
没有启动NVDEC固件、DMA或解码，也没有提升候选基线。

候选期间ACTMON返回`-3`（`SwActmonReadback`），恢复后返回0、动态EMC重新工作；
这项差异原因未明，不能写成原有行为或已修复。下一版控制器已增加选择候选之前的电源／EMC快照，
用于区分更早的启动失败与候选影响，不能补称本轮已有该证据。

18:08 UTC确认原11项文件哈希、原五秒启用任务、DWM1456加载5b／95b及Handheld／85%／风扇策略一致；
18:09的只读显示查询确认当前及持久化1280×720／90°、物理信号720×1280，没有切向。
18:12清理回执确认候选oem141包、上传包、临时目录和任务均已移除，Code0；实屏画面本轮未另行人工确认。
该轮两次均为明确请求的计划重启，未观察到异常重启。详细结果见
[原生资源核查与恢复](NVDEC_RESOURCE_PREFLIGHT_20261116.json)。

资源失败清理和旧／新查询控制保留。下一步核实按客户端的DMA路由、继承电源状态及Host1x时钟归属，
同时解决上述ACTMON差异，再继续720p NVDEC执行、NV12／视频DDI和GPU CSC；不重放同一失败组合。

完整组件身份、冻结补丁和控制结果见OUT `nvdec-integration-20261116/resource-preflight/`
及`work-state.json`；版本表`decode_integration`与已安装`baseline_files`分开。
新增代码待Gemini4 Carbon High额度恢复审核；该轮已恢复原E95／5b／95b及原ACPI，继续本地整合。
本批源码、外部来源、完整哈希及未完成项集中于
[解码整合记录](NVDEC_INTEGRATION_20261116.json)。原始交付保存在
[`nvdec-deepseek-delivery-20261116`](https://github.com/ruje0504-2/mesa-switch-woa/tree/nvdec-deepseek-delivery-20261116)
归档分支；其中高分辨率提交及旧独占探针不能作为当前基线已支持硬解的证据。

# 已完成：固定90°逐行DWM及原生vkcube本轮验收

用户随后明确交回Switch继续vkcube验收，最终确认五分钟运行、移动／放大正常，
并在单独15秒橙色遮挡补测后确认恢复正常。完整结果见
[VKCUBE_ACCEPTANCE_20261116.md](VKCUBE_ACCEPTANCE_20261116.md)及
[版本表](ROW_SCAN_BASELINE_20261116.json)的`win32_vulkan_acceptance`。
本轮保持E95 KMD／5b UMD／95b ICD；原生LUID0000000000079305，
实际计时300.289秒，区间平均19.77FPS，包含用户窗口操作；不是固定大小性能基准。
全轮7769次Present与30次交换链创建／重建返回0，DWM1184及启动保持，程序退出0。
初次5秒遮挡用户未看见，仅补测该项；没有重复五分钟或恢复已取消的方向排查。

清理收据和24项已校验本地证据见OUT `vkcube-acceptance-20261116/`。
原桌面vkcube包保留，不提升／替换驱动；180°→90°异常依旧保留，列扫描仍后置。
源码新增计时／观察工具待Gemini额度2026-11-20T23:53:18Z恢复，不声称独立审核通过。
本轮实机工作结束，继续已授权的本地720p NVDEC／D3D11VA审查整合；
该解码候选尚未提交／安装或硬件验收，资源归属及生产接口仍有具体缺口。

# 历史交接：用户手动打开vkcube；停止Switch连接

最新指令：“你补进去我来打开就行，连着ssh似乎容易重启”。启动参数已于15:09:18 UTC
补入桌面快捷方式，主聊天随后停止全部Switch SSH/SFTP、查询、启动、清理和恢复操作，
直到新的用户授权。一次后续启动检查命令超时，无完成收据，不重发；不能据此认定SSH导致重启。
当前设备使用权及待清理项见OUT `device-lease.json`，历史设备归还不覆盖本次接管。

桌面目录为`C:\Users\ruje\Desktop\vkcube-native-arm64-20261116-142245`，
包含未修改的Vulkan-Tools／Loader v1.4.354 ARM64构建、已有NVK身份检查工具、许可证及完整版本表。
`vkcube-320x240.lnk`现通过`Run-vkcube.ps1`设置进程级GM20B NVK opt-in，再使用320×240／FIFO参数。
15:06 UTC的枚举对照确认缺少该参数时失败，加上后同一原生LUID枚举成功；没有安装VC运行库。
该对照未创建Vulkan设备、提交或呈现，WSI验收仍未完成。脚本已补回本地构建工具，
后续包附带`Run-vkcube.cmd`入口及启动文件哈希；新CMD未上传。
来源、完整SHA256及后续验收边界见[本地构建与桌面暂存](ROW_SCAN_BASELINE_20261116.md#原生vkcube本地构建与桌面暂存)。
GPU／电源／90°工作基线保持下述组合，180°→90°异常继续依用户要求停止排查、不重放。

用户另外授权“审查并入一下解码／你做完整吧”：继续本地720p H.264 NVDEC／D3D11VA集成。
外部交付已接收，候选仍未提交、安装或完成硬件验收；不能把旧“本轮未整合”当作拒绝该新指令的依据。
生产资源归属、与DWM共存的内核通路、NV12输出及视频DDI仍须闭合，不安装会替换当前DWM驱动的探针。
Gemini4 Carbon High待2026-11-20T23:53:18Z额度恢复，新增代码仍待独立审查。

# 已发布基线：固定90°逐行；180°返回异常未解决

用户已指定当前E95 KMD／5b UMD／95b ICD、电源配置和90°横屏为开发／回退基线，
并要求保持这个默认方向。保存设置及一次用户要求的Windows重启已核实：
逻辑1280×720，物理720×1280逐行扫描；11项文件哈希和电源策略保持。

用户随后明确追加270°、180°测试，并取消隐藏方向选项的想法。90°、270°、180°图案正常上屏，
270°→90°正常；**最后180°→90°返回轮发生异常Windows重启，原因未定位**。
用户最终要求“不用查了……做个记录更新GitHub就行”，因此停止排查和后续测试，不重放该轮。
旧目标中的基线提升条件由用户本次明确指定覆盖，但不能据此把未验收项标成通过。

权威结果、完整组件哈希、电源策略和失败边界见
[逐行基线与本轮结果](ROW_SCAN_BASELINE_20261116.md)及
[机读版本表](ROW_SCAN_BASELINE_20261116.json)。窗口交互、vkcube、长期稳定性、列扫描均未完成。
190项本轮原生文件已归档校验，14:04 UTC清理回执确认临时目录和剩余任务清理完成、Code0、默认任务Ready。
下方交接按时间保留，不能覆盖顶部最新指令。FEX侧聊于13:22 UTC归还是历史交接；
当前用户正在测试Win Box64，主聊天仅继续本地工作。

# 历史设备交接 — 2026-11-16 12:22 UTC

用户已在 FEX／CPU-Z 侧聊授权协调实机使用权；主聊天完成最后只读核对后，现交由 `01a1e43d-d617-7d50-8720-bf2fca4270a1` 独占进行临时 FEX ARM64EC／CPU-Z x64 测试与原样还原。**该侧聊明确归还前，本聊天不再进行 Switch SSH/SFTP、安装、切向、测试或恢复；可继续独立本地工作。** 详情见 OUT `device-lease.json`。

最后核对：Code0、DWM1424、默认启用任务Ready、E95/5b/95b完整哈希一致；本聊天临时任务和录制会话均为空。新逐行切向尝试因主机确认握手失败，被保护条件拦在任务启动之前，`test-state.json` 不存在，没有切方向，也没有新的重启。实时录制工具与未执行输入暂留 `C:\ProgramData\SwitchWOA\GpuUpdate\row-switch-evidence-20261116`；不要启动它们。确认方式已在本地改为同一SSH连接的独立控制通道写入确认文件，尚未部署或验证。归还后先读取实际状态和组件，不盲目重放旧输入。

# 当前阶段 — 固定手持横屏：GPU 预旋转＋DC 逐行扫描，再验收原生 vkcube

**实施原则：复用已有短时正常上屏的逐行横屏路径，优先修复切向／恢复重启及已证实的稳定性问题。新增预旋转实现、重做呈现链和扩建现代 UMD 接口均不是默认前置工作。**

**用户已替换阶段范围。** 当前目标是普通 DWM 的固定逻辑桌面 1280×720，经 GPU 预旋转送入 720×1280 的逐行扫描主表面。固定横屏实屏、进入／必要恢复及窗口交互通过后，验收同一原生 NVK LUID 的 Win32 `vkcube`，包括至少五分钟运行。DC 列扫描和完整四方向均后置，不再作为本阶段前置或结项条件。

权威目标文件已变更为 `/Users/rujian/.codex/attachments/a754274e-adfb-4ec9-879c-9ddce4145fed/goal-objective.md`，继续工作前先读该文件。旧 `ffae5deb-...` 目标及下方历史轮次中的列扫描门槛不再适用。现代接口扩展冻结；既有列扫描源码／研究完整保留，当前不继续扩建。不能靠始终不切向掩盖重启故障，也不能把历史短时横屏组合直接恢复为稳定基线。

下一轮围绕“逻辑横屏请求／GPU 预旋转 → 实际逐行主表面分配 → VidPN／MPO → DC 锁存”补齐调用进入／返回及实际布局、同步、生命周期证据，日志须覆盖故障时刻并实时保存到 Mac。先验证取证通路，再进行所选方向的一次普通切换；不扩大现代接口或四方向。当前仍使用下述已恢复组合，尚无新横屏或 vkcube 验收。

已有路径复用依据、完整组件哈希、历史恢复故障、实时取证控制结果和未执行的进入尝试，集中见
[逐行横屏与切向证据](ROW_SCAN_ROTATION_EVIDENCE_20261116.md)。新逐行测试按实际720×1280／pitch2880布局核对，
不再把列扫描位、1280×720扫描存储或pitch5120作为启动图案的条件；进入和恢复分别观察。

## 最近列扫描试验 — 2026-11-16 10:54 UTC：横屏应用异常重启，已恢复

该轮要求接齐一次普通方向切换的请求、分配、VidPN／MPO 和锁存，捕获中断的具体调用；现按上方逐行路线继续取证。Claude 不可用（用户确认额度不可用），不重试；Gemini Carbon High 仍待 2026-11-20T23:53:18Z 额度恢复，不把其他复核当作 Gemini 通过。

本轮只加载已有列扫描 KMD `102218b0d69af15e1259a9f4047dec049eb8a61228b10eaf3551898cea8172b7`，保留 5b UMD／95b ICD。DWM1412 实际加载该 UMD／ICD，随后一次普通 90° 切换在 `Applying` 阶段异常自动重启，用户确认“自动重启了”。未取得切换后的主表面或 DC 锁存，未运行图案、四方向或 vkcube。该次异常不是主聊天发出的重启，也不同于前轮现代 UMD 在切方向前失败。

10:52:48 UTC 恢复任务完成；10:54:33 UTC 核对 11 项文件、Code0、DWM1424 的实际模块、Handheld、85% 充电及风扇／EMC 策略。当前仍是 E95 KMD／5b UMD／95b ICD，活动目录 `C:\ProgramData\SwitchWOA\GpuUpdate\tgd3d-copy-lifetime-5b66bbec-20261116-122524`；恢复后实屏未另行确认。此轮有两次计划重启（加载、恢复）和一次横屏应用中的异常重启。未提升基线。

现有 ETL 解出 44 条 UMD、506 条 Dxg 记录，其中 4 组 MPO（8 事件）仍是 720×1280。最后 Dxg 事件为 10:43:49.5862455Z，`Applying` 状态文件持久化于 10:43:50.1346963Z，缺少真正切换后的尾部。ETL `end_time=0/buffers_written=0`，不能用零 loss 计数证明完整；部分 PowerShell 重定向的 JSON 已成为全零内容。已有 KMD `c27c1f9b-62e8-4e21-b29b-6bb2d585c6d0` 诊断提供程序未加入本次会话。须先补齐取证并在 Mac 实时持久化，不能据缺失记录定位根因或重复同一失败测试。

原 UMD 源码确有 1280×720／pitch5120 主表面分支，原 ICD 支持显式线性 pitch；三个组件的 `allocation_abi.h` 完全一致（SHA256 `860bbf9c80475c7a853d182cea53ef70a03ef9a5e662aa06c04cf9001aa31ecd`）。这只证明已有实现和布局契约，不能证明本次 runtime 实际走通该分支。完整源码、组件、取证与恢复收据在 OUT `legacy-column-handoff-20261116/checkpoint.json` 及 `native-results/`。11:19:19 UTC 已清理三个临时任务、失败安装目录、输入／采集工具和上传文件；17 项恢复备份已在 Mac 逐项校验，实机只保留备份与恢复收据。DWM1424、Code0、默认任务Ready保持。该列扫描试验已结束，不重放。

## 历史状态 — 2026-11-16 10:05 UTC：新接口试验撤回，原组合已核对并清理

**已恢复5b UMD/E95 KMD/95b ICD，回到普通DWM横向primary/DC列扫描。** 10:00:04 UTC
核对11文件、DWM1352实际模块、Code0、默认任务Ready及原电源策略；10:05:40清理本轮
临时trace任务、输入/失败候选目录和上传包，DWM/Code0不变，恢复备份保留。
没有切方向、没有横屏/四向/vkcube新验收；恢复后物理画面未另行确认。

本轮新接口候选真实创建runtime context并接受7508字节NVK初始化提交，但随后DWM完成查询
报设备移除并出现黑屏反馈，不能记为呈现成功。现代协商已再次关闭，停止扩大该方案。
保留同device提交/Fence生命周期、无NT导出的owner v3及失败清理修正；下一步直接沿原可用
DWM路径核对横向主表面请求/选择和DC锁存，现代接口不作为横屏或DXVK前置条件。
完整组件/源码哈希、真实验证边界及审查状态见[本轮记录](RUNTIME_SUBMIT_OWNERSHIP_20261116.md)。

此前09:33的离线由用户确认是没电中断、候选未执行；外接电源确认后才实际加载。
本轮有证据的计划重启共两次：候选加载一次、恢复一次；没有因SSH超时重复安装/启用/重启。
本节取代下方09:49/09:39的进行中状态。Gemini仍待2026-11-20T23:53:18Z额度恢复。

# 当前补充 — 2026-11-16 09:49 UTC：确认没电中断，候选此前未执行

用户确认“没有执行过，刚才没电重启了”。09:46:43 UTC重新核对原5b UMD/E95 KMD/95b ICD
及11项文件，DWM1368实际加载原组件、Code0。此前未找到stage/install收据、候选安装副本或
临时任务，不能将断电记为新接口失败。09:49:22 UTC已读取外接电源Online、Charging。
现在继续同一已准备候选的最小实机确认，采用直接执行且保存收据的Stage；详情以OUT
`runtime-submit-owner-20261116/checkpoint.json`及最新`native-stage-direct.*`为准。
不要重复控制、安装或重启。候选仍未验收，成功后即回到普通DWM横向primary/DC列扫描。

# 当前进行中 — 2026-11-16 09:39 UTC：配套候选已发出加载，实机离线待核实

**不要重复安装、启用或重启。** 09:33:23 UTC仅发出一次 `Tgd3dRuntimeSubmitStage`；
其后SSH读取超时，重连为连接超时/无路由。尚未读回本轮`stage-state.json`/`installed.json`，
不能声称候选已经加载、已启动、驱动失败或已经恢复。没有切方向、图案测试或vkcube。
本机没有USB/串口恢复连接；已集中询问用户当前实屏状态，等待恢复所需信息。

09:32:32 UTC安装前核对11文件、DWM1176/07:18:11、启动07:17:34、Code0、Handheld、
充电85、fan1、EMC204/1331.2/1600/2500。最后确认加载5b UMD/95b ICD/E95 KMD。
09:32:39 UTC三项原生ARM64 CPU控制均退出0；这些没有打开GPU，不能当作真实runtime确认。
候选为 UMD `f8ed1d8938f8d490fac84762234e3e0a0671d79ba018fa245190ad60939f9016`、
ICD `a488ff8f6f3b5bb22c86715d8c2ca1d0265fab48542956104ba46c2d406a5ed6`，KMD计划保留
`e95287cbeb1d2c325baacb9685affd1d24783b22ecadc1801ba1dffcfe7cc25e`。
完整候选源码、原继承改动、构建、哈希、控制、SSH记录和待执行恢复脚本在OUT
`runtime-submit-owner-20261116/`（先读`checkpoint.json`和`IN_PROGRESS.md`）。

当前补丁把实际NVK命令和WrittenPrimaries移到runtime拥有的上下文；同device Fence引用保留，
提交保持原DDI线程；displayable owner v3不依赖普通allocation的NT导出。GPU等待/清理失败时，
必须成功销毁这些提交上下文才能释放在途GPU对象；失败保留对象，不写CPU完成值。
UMD/ICD为配套私有ABI，ARM64/x64构建与本地控制通过；现代入口已为这一次候选协商。
尚未取得原生runtime创建/提交/呈现成功证据，**不能宣布新增回归已修复**。

用户最新边界：旧UMD已可经runtime到KMD MPO3；现代接口不是DXVK前提，也未证明是列扫描
必需。停止扩大现代接口工作；本次最小实机确认后直接回到普通DWM横向primary/DC列扫描。
异常即保存直接证据，恢复确切5b/E95/95b；不要重做已跑通的呈现链路。
Gemini的DXVK游戏适配及SC7、DeepSeek的NVDEC/D3D11VA均为外部未合入工作，勿重复开发。
Gemini Carbon High审查仍待2026-11-20T23:53:18Z额度恢复；未重试未声称通过。
横屏、四向、实屏与vkcube均未新增验收，候选没有提升为基线。

# 当前进度 — 2026-11-16 08:48 UTC 写入跟踪连接完成，真实导出范围未闭合

在f01448d0后端上接通owner v2、显示表面属性、实际写入列表和NVK命令提交；完整操作前按16项
分批，异步队列复制列表，禁止通用合并丢失列表。清理失败保留引用；显示引用不误报Trim回收。
四项原生ARM64 CPU控制和相应x64控制通过。详见[呈现资源契约](DISPLAYABLE_RESOURCE_CONTRACT_20261116.md)。

**未开放现代接口，未安装新候选。** 新核对发现ShareObjects要求NtSecuritySharing，匹配runtime
只在特定共享描述下设置该位；AllocateCB的kernel-resource输出可用性也未闭合。不能把CPU替身的
成功当作DWM资源可导出的证明。下一步审查runtime管理的第二个提交context与真实NVK命令路径，
保持当前双向Fence和列扫描目标，不通过空命令／伪完成绕过WrittenPrimaries。

08:43:33已清理CPU控制目录，启动仍07:17:34.328844Z、DWM1176、Code0；加载5b66bbec UMD／
95b97df4 ICD、e95287cb KMD及平台设置保留。无安装、切方向或重启；没有新增实屏验收。
08:47:55只补测后续批次失败清理的新队列控制，退出0，DWM1176不变，临时目录已删除。
新ARM64 UMD/ICD及完整源码、哈希、控制结果在OUT `displayable-bridge-20261116/manifest.json`。
Gemini独立审计待2026-11-20T23:53:18Z额度恢复，未声称审核通过。

## 前一轮 — 呈现资源写入契约与提交device引用


接0f1760d7继续核对：DISPLAYABLE_SURFACE未被资源路径支持，NVK提交也缺少WrittenPrimaries。
匹配dxgmms2确认该列表拒绝别的device的allocation句柄；GPU VA相同不能替代所有权。
已实现NVK提交端同backing新device引用、受控长度/归属验证、分页移除处理和真实提交列表入口。
**UMD写入跟踪及NVK队列连接仍未接通，现代协商保持关闭，新ICD没有安装。** 不把基础函数存在
写成契约已闭合。下一步处理该连接、16项分批及引用退休，再做普通DWM列扫描验证。

07:56 UTC三项原生ARM64 CPU控制退出0，DWM仍1176。07:57已删除本轮临时控制目录；未安装、
切方向或重启，设备组件沿用下节。Wine既有共享控制在写合并属性检查失败，原源码复测相同；
Windows原生控制通过，没有放松检查。详见[呈现资源契约](DISPLAYABLE_RESOURCE_CONTRACT_20261116.md)。
完整源/构建/哈希/结果保存在OUT `modern-present-resource-20261116/`；GPU上屏未新增验收。

## 前一轮 — 现代入口失败已恢复；本地修正异常Fence完成值

57a0845c资源入口候选已做原生验证：DWM现代设备创建及15个buffer成功，但随后发生资源拒绝、
设备失败和DWM重建。没有成功Present，也没有切方向；此前两次事件10成功应为SetDisplayMode。
现代接口已再次撤下默认声明，保留typed实现和资源修正。详见[原生结果](UMD_LINEAR_RESOURCE_ADMISSION_20261116.md)。

失败UMD SHA256 `f9bec71061d2f6ed0c1fdb207beffa480cb048d2937e54c6b41d17a930094c44`。
07:16:53 UTC恢复原注册，启动时间07:17:34.328844Z；07:21确认DWM PID1176实际加载原UMD/ICD，
07:25:01完整核对11项文件、Code0及平台配置。当前活动目录仍为
`C:\ProgramData\SwitchWOA\GpuUpdate\tgd3d-copy-lifetime-5b66bbec-20261116-122524`：

| 组件 | SHA256 |
|---|---|
| UMD | `5b66bbec86af299941812e8376cc1ac4a5630385828c895b7a52ec96d5c5ad7a` |
| KMD | `e95287cbeb1d2c325baacb9685affd1d24783b22ecadc1801ba1dffcfe7cc25e` |
| ICD | `95b97df4e83a2a90315a8784d83e75771021093dec34f98362b584d1f546bdff` |

Handheld、85%充电限制、fan1、EMC204/1331.2/1600/2500、双向Fence和登录后启用保持。
临时任务、ETW会话、失败包、测试目录、上传包和收据临时文件已清理；原活动包保留。
恢复实屏未另行验收；测试前后电源原始读数不同，未记录为全程插电。完整11项哈希和状态见OUT
`modern-resource-audit-20261116/exact-recovery-verification.json`及`recovery-state-1.json`。

PID500首个提交的completed=UINT64_MAX，旧代码可能据此提前退休。本地新增私有渲染Fence检查，
并在分页等待前/后拒绝全1值；ARM64/x64构建和x64 CPU控制通过。新ARM64 DLL
`4f73f56dc2efc675a82fb4b8d9e7d2de37e0a2a78555e4ff144280a4a51dd179`未安装，没有新增GPU验收。
新控制程序`3760750f8657076844afe9a882147f5eaf103fda4b1594125787be28f7db872b`仅构建。
冻结源码和产物在OUT `modern-resource-audit-20261116/fence-fix/manifest.json`。

已对照匹配dxgkrnl.sys、同机Noble交接及WDDM2.6适用契约，见
[上下文和Fence审计](WDDM26_CONTEXT_FENCE_AUDIT_20261116.md)。下一步处理32×32呈现纹理请求、
NVK设备丢失和分页等待的剩余缺口，再恢复现代入口验证；不重复安装57a失败组合。
列扫描、已知像素、vkcube均未因此通过。Gemini待2026-11-20T23:53:18Z额度恢复，VPS未登录未完成审计。

## 前一轮 — 2026-11-16 WDDM2.0试验失败，已恢复

`0b9b7044`启用现代接口的UMD `d17ff16d431cc93ac5a51d3d4384fba681540b43d197c2347b36a87200c4ed14`
在切换方向前黑屏、鼠标时隐时现；DWM由PID1416重建为1496。本轮未开始横屏测试，不能归因为列扫描。
已恢复UMD `5b66bbec86af299941812e8376cc1ac4a5630385828c895b7a52ec96d5c5ad7a`，保留E95 KMD、
95b ICD及Fan33/Core7/Battery14。06:31:18 UTC核对Code0、DWM1416／06:21:26.779Z、
恢复启动06:20:54.419Z、11项文件及Handheld／85%／风扇模式1／动态EMC；临时任务和失败候选已清理。
恢复后的物理画面未另行验收。没有再启动同一失败候选，也未关闭双向Fence。

当前源码撤下现代接口默认声明，保留实现；补上MakeResident失败时动态裁减／重试、处理Evict
更新的预算、耗尽可裁减对象后有限退出，保护目标／在途／映射／扫描对象。ARM64/x64构建、
本地CPU控制通过，未上机。ContextTypeFlags和对齐未发现可直接定罪的证据；缺失预算重试是
确定的契约缺口，但本轮没有取得该失败分支的实机调用证据。工具打包缺少ETW helper／consumer，
并有JSON编码读取错误，均独立记为工具失败。完整范围见[审计报告](WDDM2_CONTEXT_RESIDENCY_AUDIT_20261116.md)。

下一步继续检查新接口实际资源描述符及runtime/NVK跨设备所有权，不把这次控制通过当修复验收。
列扫描与vkcube仍未通过；Gemini独立审核待2026-11-20T23:53:18Z额度恢复，不提前重试。

## 历史进度 — 2026-11-16 05:53 UTC
新增[共享数组与跨设备所有权](UMD_SHARED_ARRAY_CONTRACT_20261116.md)，以及
[Trim驻留／能力查询](UMD_RESIDENCY_CONTRACT_20261116.md)。ARM64/x64构建与本地控制通过；
驻留控制在Switch原生ARM64退出0，范围及确切哈希见后者。现代表仍未对系统启用。
发现WDDM2.0上下文创建要经核心层的新回调并填ContextTypeFlags，下一轮从这里继续设备协商；
不要直接把长表暴露给系统后继续沿旧上下文入口。

实机仍加载5b66bbec UMD，保留e95287cb KMD／95b97df4 ICD及当前电源、双向Fence和登录启用配置。
本轮没有安装DLL、切方向或重启；控制临时文件已清理。05:53:39 UTC的启动时间、DWM PID／
启动时间及UMD完整哈希与此前一致，Code0。列扫描实屏、现代接口运行及GPU共享像素尚未通过。
Gemini本轮差异待审（额度恢复2026-11-20T23:53:18Z）；没有声称审查完成。

# tgd3d 冷启动重启：探索记录与后续交接

## 当前工作 — 2026-11-16 05:06 UTC UMD MPO1 与同表面 Blt

本轮补充匹配 DWM/D3D11 runtime 的呈现入口门槛，添加 DXGI1.4 MPO1、WDDM2.0 类型适配，
并修复 DXGI 同表面 Blt 的源快照与临时图像退休。详见
[UMD MPO1 契约记录](UMD_MPO1_CONTRACT_20261116.md)。ARM64/x64 构建和 CPU 控制通过，
Switch 原生 ARM64 控制退出0；没有真实 GPU 像素或列扫描通过结果。
现代接口尚未协商，新候选 DLL 未安装；设备继续使用下节5b UMD/E95 KMD/95b ICD。
本轮没有安装、切方向或重启。下一步是完成现代能力/资源契约后启用接口验证普通 DWM 列扫描。
05:06:57 UTC已收回结果、删除本轮临时控制工具和上传包；DWM PID1416、启动时间和已加载5b UMD
均未变，显示Code0。自包含控制程序的通过不能替代新DLL的桌面验收。

## 当前工作 — 2026-11-16 UMD 复制与生命周期

**已安装候选 UMD，短时自动窗口流程完成；未提升列扫描基线。** UMD SHA256为
`5b66bbec86af299941812e8376cc1ac4a5630385828c895b7a52ec96d5c5ad7a`，当前目录
`C:\ProgramData\SwitchWOA\GpuUpdate\tgd3d-copy-lifetime-5b66bbec-20261116-122524`。
KMD仍为`e95287cbeb1d2c325baacb9685affd1d24783b22ecadc1801ba1dffcfe7cc25e`，ICD仍为
`95b97df4e83a2a90315a8784d83e75771021093dec34f98362b584d1f546bdff`。
04:37:48 UTC核对11项文件、Code0、Handheld、85%充电限制、风扇模式1和动态EMC。
仅一次计划加载重启，启动时间04:26:20；登录启用任务仍有效。原4c8 UMD、注册和任务备份
保留在既有恢复工具中。没有回退`.59`、切显示方向、安装KMD/ICD或改变平台策略。

04:34:15–04:35:01 UTC在ruje交互会话执行45.31秒GDI/WinForms窗口流程：移动、缩放各7次，
遮挡、最小化和恢复各1次，按时关闭、退出0。前后DWM PID1416及启动时间相同，
同一原生LUID`000000000007c1d1`，实际tgd3d Vulkan设备计数均为1；系统启动时间不变。
**这只属于自动窗口检查，未验证本轮物理画面、已知GPU像素或列扫描。** 两次各30个VBlank
等待样本的唤醒频率约59.95/59.99Hz，不能当桌面FPS。DWM时序计数来自不同探测进程，
不得跨进程相减宣称累计呈现帧数。DWM私有内存单次增长约6MiB，不能据此判断泄漏与否。

本轮工具失败分别为：CPU检查器没有保留有效进程退出码、安装器只识别正斜杠清单路径、
窗口任务误用SYSTEM会话。均已区分为工具问题并修正；前两次失败没有安装/启用新驱动，
错误窗口任务在交互会话校验处退出，没有执行窗口测试。修正后的原生CPU控制退出0、
安装完成、ruje窗口任务退出0。原始失败记录与最终记录均保留在OUT交付目录。
04:39:45 UTC已删除本轮4个临时任务、测试exe／脚本／SPIR-V、上传包及失败安装留下的
未注册目录；当前候选、默认登录启用任务和恢复备份保留，清理没有重启或更换驱动。

用户取消强开 KMD 的物理验证，要求继续 UMD；随后授权由本窗口自行实机测试。
本地已补齐重叠 buffer／2D／3D／带行填充 staging 复制的先保存后写回，真实完成后的
scratch 退休，以及创建／分配／绑定／命令申请失败清理。间接绘制使用 GPU 参数缓冲区，
启用实际支持的 drawIndirectFirstInstance；只读 DSV 随视图切换更新写掩码，补 mip／层范围
校验。完整 DXGI1.3 表按类型赋值。现代表仍未加入 GetSupportedVersions／CreateDevice，
没有扩大 Feature Level 或多采样、UAV、计算着色器等声明。ARM64／x64 构建和 CPU 控制通过；
实机测试进度以 OUT `modern-ddi-resources-20261116/` 的状态文件为准。

接口依据：微软[间接绘制DDI](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/nc-d3d10umddi-pfnd3d11ddi_drawinstancedindirect)、
[只读DSV标志](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/ne-d3d10umddi-d3d11_ddi_createdepthstencilview_flag)，
以及[Vulkan间接绘制规范](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdDrawIndirect.html)。
头文件为SDK/WDK10.0.26100.0；语义复核使用Microsoft/D3D11On12
`c6b5a961ee5e0761136fc2091b5e57f7bd9235f2f`及D3D12TranslationLayer
`4b9c8f8a49c9b68df19be817d56f123d65704fd2`。仅参考对应契约，未整批移植上游代码。

**纠正前一节因果判断：旧 UMD 已能通过 runtime 到达 KMD MPO3。** 本轮 PresentId=241
来自 `panel_scanout.c::set_primary_mpo`；旧 D3D10 primary 创建也已有1280×720/pitch5120路径。
仍未解释的是横向 primary 的请求／选择及 plane0 最终为何采用 identity 竖向存储。
“现代 UMD 表未启用”是实现状态，不能作为列扫描黑屏的已证根因或必要前提。
Noble 已证明硬件列扫描能力；不再增加强开寄存器探针。本轮拟只换 UMD，保留当前
E95 KMD、95b ICD、电源／风扇和双向 Fence；不把功能控制通过记为列扫描验收。

## 当前状态 — 2026-11-16 03:36 UTC

**本轮短时横屏正常上屏，由用户照片确认；Tegra DC列扫描没有启用。** 用户随后再次明确
“是的，要列扫描”。本轮在普通DWM上只做了一次入口测试：逻辑1280×720、VidPN90°，
但实际720×1280/pitch2880/rotation0、WIN_OPTIONS=0x40000000。显隐开启、新地址已锁存；
不得再把这一组合或旧红屏强开当列扫描证据。由于入口未进入列扫描，四色窗口未启动。
照片及原始结果保存在OUT `mpo-null-context-20261116/`。

新KMD只修正匹配dxgkrnl反汇编所示的无context MMIO Present误拒绝。该分支本轮是否到达
未证实，也不称为重启根因。KMD102218b0+原候选UMD379e521d+ICD95b97df4，完整哈希见
`candidate-package.json`。ARM64控制和13项Windows签名检查通过；正常桌面观察证据
不能代替GPU像素或列扫描验收。安装前源树e534766c及具体diff完整保留。

当前**已恢复本轮之前的E95/4c8/95b、Fan33/Core7/Battery14**：03:33:51 UTC核对11文件、
Code0、DWM原UMD/ICD、登录启用任务，以及当前/保存720×1280方向0/60Hz。
Handheld、85%充电限制、独立风扇策略保留。恢复启动03:28:45，来源为明确发出的恢复任务；
本轮安装和恢复各一次计划重启，入口测试至恢复前没有观测到自行重启。
恢复任务在用户照片到达前已启动；正常横屏候选和照片已保留，没有丢弃这项进展。
03:35:40已清理6个临时任务及21个临时路径，探针/图案工具已删除；备份及候选来源保留。
清理首个SSH客户端超时，随后原生cmd读回cleanup.json确认完成，未重复执行清理。

**后续复核已修正此判断（见顶部）：**tgd3d只协商原D3D10接口，但旧UMD仍可经runtime
到达KMD MPO3；横向primary创建路径也已存在。缺少的是横向表面最终被选用并完成列扫描
锁存的证据，不能仅凭未注册现代typed table认定接口缺失导致黑屏。
NVK ICD仍为现有修复版，不应将所有普通纹理改方向。
下一轮先补足普通DWM的横向主表面路径，在实际1280×720/pitch5120/COLUMN锁存后才做图案。
保留现有面板时序、电源及双向Fence，不再以仅修改KMD的逐行结果替代该步骤。

本地另完成现代DDI Offer/Reclaim的runtime私有分配、真实paging fence及CPU映射刷新，
ARM64/x64构建和CPU控制通过，未部署、未声称现代DDI实机通过。详见
[内核与呈现交接审查](DXGKRNL_COLUMN_HANDOFF_AUDIT_20261116.md)及
OUT `runtime-offer-reclaim-20261116/checkpoint.json`。
Gemini独立审核仍待2026-11-20T23:53:18Z额度恢复，未重复尝试。


## 当前状态 — 2026-11-16 01:54 UTC

**当前验收基线已恢复：E95/4c8/95b + Fan33/Core7/Battery14。用户新要求的一次真实列扫描
四色测试尚未执行；继续接通普通DWM列扫描路径。** 01:44:35 UTC精确核对11项文件、
Code0、DWM PID1392的4c8/95b及默认登录任务；启动时间01:39:17，当前及保存方向均为
720×1280/方向0/60Hz。Handheld、85%充电限制及独立风扇策略保留。01:48:52清理5个
已结束任务和20个临时路径，另一个图案任务已不存在；恢复备份与来源清单保留。

本轮对比 `01e5aa3e` 的父提交 `54dc559731a48408266177bdbae192f04a643536` 与恢复代码。
旧/新四向列扫描布局一致；历史代码及逐行红屏强开均不作为列扫描已通过基线。
修正 `panel.c` 的一处显隐契约：源Visible=TRUE重新启用主平面，保留目标门控和
FormatAwaitingPrimary。ARM64 CPU控制由修前468变为修后0；没有伪造Flip完成。
详见 [历史列扫描审查](COLUMN_SCAN_HISTORY_AUDIT_20261116.md)。

相对前轮列扫描候选cf30/379/95b，本次只换KMD为7cf，保留379 UMD、95b ICD、GPU bridge
和全部平台组件。01:34:26–01:34:32进行普通GDI/DWM四色测试；用户明确确认实屏横屏、
四色图案、回到正常横屏桌面约2秒后自动重启。**短时实屏横屏有效，但实际读回仍为逐行
扫描720×1280/pitch2880/rotation0，列扫描未接通。** 图案退出0后，脚本开始恢复原竖屏；
持久状态停在Restoring、无RestoreExit。01:35:13的新启动确认异常重启。
原始面板输出及图案日志在重启后为全零，只采用原子保存的状态字段，不能补造地址或像素。
这定位了失败时间段，尚未证明具体回调、GPU指令或资源退休根因。

已主动复核匹配的dxgkrnl.sys（10.0.22621.2792、SHA256 99d425e1…），GUID及DBI age匹配。
新增只读反汇编覆盖11个显示相关入口，并追踪GetContentRotationHw和目标虚拟化标志。
已证明存在把硬件旋转变为identity的虚拟化分支；**尚未证明本轮进入该分支**。
当前基线的官方虚拟分辨率查询返回ERROR_NOT_SUPPORTED(50)，没有修改该设置。
不要据此直接改注册表、强开窗口或把现代UMD原型整批启用。

源、控制、组件完整哈希、实机状态、用户观察、精确恢复及清理记录位于OUT
`column-scan-history-audit-20261116/`。只读虚拟化查询工具及临时文件已于02:02:12 UTC清理，
未改显示设置。后续反汇编复核见[内核交接核对](DXGKRNL_COLUMN_HANDOFF_AUDIT_20261116.md)。
Gemini审核仍待2026-11-20T23:53:18Z额度恢复。现代DDI11类型桥仍未协商、未安装。
下一步依照真实内核调用与DDI契约补齐DWM新主表面→DC列扫描，以及切回时的锁存和退休；
满足1280×720/pitch5120/COLUMN位后执行用户要求的同样四色测试，不重复逐行预旋转试验。

## 当前状态 — 2026-11-16 00:55 UTC

**已恢复 E95/4c8/95b + Fan33/Core7/Battery14 验收基线；回到普通 DWM 横屏路径。**
00:50:26 UTC 核对 11 个文件哈希、Code 0、DWM PID1376 的 4c8/95b 模块和默认登录
启用任务。恢复启动为 00:42:29 UTC；当前及保存方向均为 720×1280、方向 0、60 Hz。
保留 Handheld、电池限制 85 及独立风扇选择。00:53:29 清理本轮 6 个结束任务和
16 个临时路径，保留备份及正常启动任务。没有提升显示基线，也没有新的物理屏幕验收。

本轮唯一诊断重放沿用前轮 cf30/379/95b，不含新的驱动修正；变化仅是已通过本机
CPU 控制的进程内 ETW 观察器。计划安装启动为 00:31:28 UTC，00:34:57 NativeEnabled。
00:37:07–00:37:13 的独占窗口探针设备和窗口交换链创建成功，进入独占 E_FAIL，
退出状态 1，没有测试 Present；随后退出独占并恢复方向。该轮未观察到非计划重启。

34 个完整事件全部属于探针 PID1456，零事件/缓冲丢失；该进程没有进入 primary 创建或
SetDisplayMode。**观察器不覆盖 DWM/KMD，因此不能据此推断桌面没有执行模式回调。**
这明确区分了新增独占窗口失败与用户指出的“逻辑横屏已切成、物理面板黑屏”。独占
全屏不再作为桌面横屏的前置条件。已退掉本轮临时 ETW 源码，冻结副本和结果仍保留。

用户要求核对 WDDM 2.6，本轮完成显示路径静态审查，见
[WDDM 2.6 显示契约核对](WDDM26_DISPLAY_AUDIT_20261116.md)。明确缺失的是
CollectDiagnosticInfo；更贴近横屏的问题是活动路径方向到主表面/DC 的衔接，以及
现代 UMD 呈现接口尚未注册。三项分别记录，均未冒充已证明的黑屏根因。
实际 VBlank 完成回调存在，不添加伪造 HsyncFlipCompletion，不盲目强开显示窗口。

本次审查没有部署新驱动或追加重启；只是完成上一轮已请求的恢复和清理。
下一步复用 `rotation_pattern.cpp` 经普通 GDI/DWM 验证新 backing、源可见性及实际
DC 地址/布局锁存。源码/WDK 哈希在 OUT `wddm26-display-audit-20261116/`；诊断与
精确恢复记录在 `fullscreen-return-contract-20261116/test-assessment.json` 和
`recovery-archive/`。Gemini 仍待 2026-11-20T23:53:18Z 的额度恢复。

## 当前状态 — 2026-11-16 00:13 UTC

**已恢复用户验收的 E95/4c8/95b + Fan33/Core7/Battery14 基线，横屏仍未通过。**
00:11:54 UTC 核对全部 11 个文件哈希、Code 0、DWM 原 UMD/ICD 及默认登录启用任务；
恢复启动时间为 00:08:03 UTC。当前和保存配置均为 720×1280、方向 0、60 Hz。
这属于系统配置读回；本轮没有新的物理屏幕验收。00:13:11 UTC 移除 6 个已结束任务和
20 个本轮临时路径，保留恢复备份、候选来源资料及正常启动任务。没有升级基线。

本轮补齐 UMD/KMD 的私有主表面布局协商：DescribeAllocation 使用不可变存储方向
区分扫描布局；UMD 只在 INCOMPATIBLEPRIVATEFORMAT 时尝试调整显示源并重试一次；
KMD 在 VBlank 先隐藏旧布局，再设置新布局，等待有效的新主表面绑定后恢复 fetch。
新绑定或新格式请求优先，超时和失败不伪造 Present/GPU 完成，UMD 不盲目回写过时格式。
采用微软源格式/分配格式契约，接口仍为 WDDM 2.6，编译头为 10.0.26100.0。
这是一项契约修正候选，**尚未证明与原生故障有因果关系**。

ARM64 构建完成。原生 CPU 替身控制验证通过（旧 KMD 在新增检查退出 213，新 KMD 和
UMD 为 0），13 项本机签名/目录检查通过。扩展测试曾在 POST 恢复处失败，原因是测试
时钟只模拟 WINDOW 锁存、遗漏 GENERAL 锁存；修正模型后通过，没有放宽生产超时。
这些测试没有执行真实 GPU，也没有证明真实 VBlank 节拍。

实际测试组合：KMD
`cf30b6405cb2859c31f28471fe60292d374470aea2951506794124f6a1813d80`，
UMD `379e521d1d6d82c2eb6d8ef0be179c1bd1270d07965b70ffcf7a4d204585649d`，
ICD `95b97df4e83a2a90315a8784d83e75771021093dec34f98362b584d1f546bdff`。
ICD、平台组件及保留的电源策略未改变，双向 GPU bridge 开启。
安装计划启动为 00:00:35 UTC；00:04:00 完成 NativeEnabled，DWM PID1412 加载匹配的
UMD/ICD。测试原生 LUID 为 `00000000000c10cc`。

**00:05:14–00:05:18 的 90° 原生测试失败。** 临时逻辑模式切到 1280×720、方向 1，
返回 0；同 LUID 原始 KMT 模式仍为 720×1280、orientation=2。前台窗口准备、原生设备
及交换链创建成功；`SetFullscreenState(TRUE)` 返回 `E_FAIL(0x80004005)`，紧接着
`GetDeviceRemovedReason` 为 S_OK。未到达测试图案上传、完成查询或第一次 Present。
退出全屏和恢复原方向返回 0；00:05:58 的读回仍为同一次启动。没有本轮异常重启证据，
但不能把单次未复现当稳定性修复；也不能据此声称新的私有格式 callback 已实际执行。
没有横屏 DC 锁存、已知图案像素或 OLED 物理输出通过结果，vkcube 尚未进入测试。

只发出一次恢复请求，完成上述基线恢复。本轮两次计划启动分别用于安装和恢复。
下一步沿匹配系统 DXGI/D3D 的全屏主表面失败路径审查，明确实际 SetDisplayMode/
私有格式回调是否到达及其返回点，再决定单项改动，不反复安装本轮候选。
独立 Gemini Carbon High 审查仍待额度恢复（2026-11-20T23:53:18Z），未宣称已审查通过。

精确记录位于 OUT `private-scanout-format-20261115/`：`candidate.diff`、
`source-manifest.json`、`reference-review.json`、`native-controls.json`、
`native-signatures.json`、`test-assessment.json`、`exact-recovery-verification.json`、
`cleanup.json`。源码维护仍在本分支；该目录的 source/ 是实际构建冻结副本，未整批取用
工作树中的未注册 Present1/MPO 原型或其他未部署改动。

## 当前状态 — 2026-11-15 23:22 UTC

**已恢复用户验收的 E95/4c8/95b + Fan33/Core7/Battery14 基线。**
23:22:10 UTC 核对全部 11 个文件哈希、Code 0、DWM 加载路径及默认登录启用任务；
本次恢复启动时间为 23:18:29 UTC。只读交互会话查询确认当前及保存配置均为
720×1280、方向 0、60 Hz。横屏候选没有升级为基线，物理横屏及 vkcube 均未验收。
设备操作授权仍由本聊天统一负责；本轮失败后停止扩大测试，继续源码审查。

本轮实测区分了两种尺寸：90° 下 DXGI 暴露 1280×720，而同一 LUID 的
`KMTQAITYPE_CURRENTDISPLAYMODE` 返回 720×1280、orientation=2。
结合匹配本机 Windows 22631.2861 的微软公开符号及 WDK 10.0.26100.0，
确认 `DXGDEVICE::SetDisplayMode` 在比较旋转值之前先比较模式宽高。
候选只修正 `DescribeAllocation(CheckDisplayMode)` 的显示模式尺寸；普通资源查询、
真实 backing、pitch 和 DC 扫描方向保持原有描述。旧代码在新增控制断言退出 890，
修正后为 0；ARM64 构建和签名校验通过。这些结果不构成 GPU 或实屏通过。

本轮候选完整标识：KMD
`a82f2510be5b6718f91349956e1eea706cf60c4012e11c16865d1426389ce32c`，
UMD `dd6be11e0396322a3c9f8f06b97d9fa7e23274dfd984de9e7d21f8ef97cb46f1`，
ICD `95b97df4e83a2a90315a8784d83e75771021093dec34f98362b584d1f546bdff`。
双向 GPU bridge 保持开启。23:12:01 UTC 完成 NativeEnabled，DWM PID1384 加载
对应 UMD/ICD；测试原生 LUID 为 `00000000000be887`。

**原生测试失败。** 23:12:49 开始 90° 测试，模式预检查成功；最后持久化状态为
`Applying`，`mode-apply.json` 为 0 字节，没有后续模式查询、图案 readiness 或成功
Present 的记录。随后检测到 23:13:19 的非计划新启动。不能从这些缺失记录还原崩溃栈，
也不能断言原因是 Fence、着色器、供电或 DC。修正后的 fullscreen 调用本身尚无完成结果。
只发出一次失败恢复请求，随后完成上述精确基线恢复；本轮不重装同一 GPU 候选。

安装工具另有一次本聊天引入的打包错误：重新生成外层 Files 字典时使用 `/`，
与重启后脚本按 `firmware\bl33.bin` / `gpu\display158.aml` 取值不匹配。
第一次包在候选驱动替换之前被校验拦截，并触发恢复重启。已改为 Windows 分隔符，
补上缺失必要条目及错误分隔符的重启前检查；修正包五项 schema 检查通过。
此故障与上述随后发生的原生模式切换失败分别记录。

重启后任务的注册、CIM/COM 读回及 OS 存储刷新检查已完成 11 项本机控制验证；
本轮安装及恢复任务也实际跨计划重启执行。它们证明本轮行为，不解释历史任务丢失根因。
新增代码的 Gemini 独立审查仍受额度限制，待 2026-11-20T23:53:18Z 后再继续；
没有把审查状态标为通过。用户已授权不等待审查即可构建和测试。

下一步审查显示模式尺寸、实际存储布局的兼容标识，以及 modeset 时地址、几何与旧
backing 的交接。微软 `DxgkDdiSetDisplayPrivateDriverFormat` 契约需与 UMD callback
一并核对；仅增加布局编号而未实现相应协商的旧原型仍未装机，不能直接启用。

完整结果与恢复记录位于 OUT `fullscreen-mode-contract-20261115/`，尤其
`static-findings.json`、`native-signatures-controls.json`、`package-v2/test-assessment.json`、
`package-v2/exact-recovery-verification.json`。23:25:47 UTC 清理完成：移除 5 个已结束
任务和 32 个本轮临时路径，异常重启后已不存在的图案任务另行记录；保留默认启用
任务、恢复备份及候选来源材料。实际回执为 `package-v2/cleanup.json`。

## Current device state — 2026-11-15 22:14 UTC

**Accepted baseline remains E95/4c8/95b + Fan33/Core7/Battery14.** It was
restored and all eleven component hashes verified at22:14:17UTC, Code0,
720x1280 original direction, normal login activation enabled. The recovery
boot is22:05:41UTC. No candidate was promoted; no new physical landscape or
vkcube acceptance exists. The owner has authorized this chat to operate Switch.

The direct fullscreen-primary trial used signed KMD
`08f34c1b2b5be619fbb1b7c5301afbecfe3821a4b3501d99f08b88816d330cdd`,
UMD `dd6be11e0396322a3c9f8f06b97d9fa7e23274dfd984de9e7d21f8ef97cb46f1`,
and the retained ICD95b. Windows11Pro22631.2861, native LUID`00000000000b92f4`.
Its production delta from the preceding candidate is the reviewed functional
VidPN topology/transform/mode validation. The new negative CPU fixture fails
the prior code at233 and passes the correction at0; ARM64 compilation and
native control checks passed. These are not GPU/display acceptance.

At22:02:28–22:02:33UTC, native device creation and windowed swapchain creation
succeeded; `SetFullscreenState(TRUE)` returned`E_FAIL(0x80004005)` before the
test's upload/query/Present. Leaving fullscreen and restoring the original
desktop mode returned0. **Zero test frames were presented.** This result does
not test column scan and does not locate a Fence, shader or electrical fault.
The optional color test's snapshot validator now correctly compares rotation
in degrees(90), not the internal enum ordinal(1), and checks active-layout,
latched-address and four exact memory-color samples separately from real-panel
acceptance. A snapshot remains insufficient to prove OLED visibility.

An earlier packaging attempt in this round incorrectly changed `kmd_variant`,
which is the recovery-family selector. The existing package guard rejected it
before any candidate installation or graphics test. The corrected package
retains `reviewed-dvfs-emc`; `Merge-Check` now validates the complete nested
GPU package before staging can close the startup gate. File-only native checks
passed for the real package and rejected both the observed invalid selector
and a wrong recovery hash. Do not bypass those guards or reuse a signed SYS
with an unrelated INF.

Recovery then exposed a separate tooling failure: after the requested reboot,
`Tgd3dDvfsRestore` was absent and the candidate remained gated atCode43.
The cause of the missing task is unproven. A guarded continuation verified
the new boot, closed gate, exact candidate/backup hashes and absence of a
running recovery, recreated only the missing task and restored the accepted
pairing in the same boot. **No extra reboot was requested for that continuation.**
Verify persistence of postboot recovery registration before another deployment.

Read-only mode inventory at22:15:12UTC on the restored portrait baseline
(LUID`00000000000ed2e6`) enumerated one BGRA87 mode,720x1280@60; a1280x720
closest-mode request returned that portrait mode. XRGB88 andRGBA28 had no
listed modes. This was not a landscape-mode query and does not prove absence
of rotation support. Next inspect the actual fullscreen mode/primary negotiation;
do not blindly add a newer DDI format-support bit to the legacy interface or
reinterpret an already pre-rotated DWM primary as horizontal storage.

Exact records: OUT `rotation-primary-native-20261115/package-v2/`
(`test-assessment.json`, `exact-recovery-verification.json`, `recovery-archive/`)
and `rotation-primary-native-20261115/mode-inventory/summary.json`.
New mode-reader and nested-schema-preflight changes remain pending independent
review because Gemini's quota was exhausted; this limitation does not turn a
local build, control check or API success into native acceptance.

## Current device round — 2026-11-15 21:36 UTC

The owner explicitly handed Switch testing back to this chat. The earlier
SSH hold below is superseded. Only this chat operates the device.

The accepted Fan33/Core7/Battery14 and E95/4c8/95b graphics pairing was
restored and verified at 21:36:11 UTC after the hardware-capability trial.
All eleven retained files match, Code 0, DWM loads the original UMD/ICD,
720x1280 original orientation, Handheld power/fan and charge85 retained.
The normal login activation task is enabled. No baseline promotion occurred.

The candidate `fc6af00f… / dd6be11e… / 95b97df4…` corrected the VidPN
`DriverRotation` declaration. Its 90-degree test reached logical1280x720,
source-visible/target-active and enabled Window A, but the actual primary
remained720x1280/pitch2880/identity scan at21:30:14. `Displayed.PresentId=193`
proves the KMD MPO3 path was reached even with the legacy UMD interface.
This is a failed column-scan selection result, **not physical black-screen
proof**. Original-mode restoration returned0 and exact baseline recovery
completed; the five temporary tasks and24 temporary paths were removed.

Next: test a bounded ordinary DXGI fullscreen primary without application
pre-rotation, separately from DWM's NONPREROTATED composition choice. Do not
reinterpret portrait pixels as a landscape allocation or fabricate flip
completion. The snapshot `rotation` field is in degrees, not enum ordinals;
new acceptance checks use90 and require actual layout/latch and pixel matches.
Native fullscreen/physical landscape/vkcube remain unaccepted.

Evidence and full hashes: OUT `vidpn-hwrotation-contract-20261115/package-v2/`
(`exact-recovery-verification.json`, `cleanup.json`, `device-results/landscape90`).
Gemini's rotation and completion-contract reviews were received. Its installer
review ended on quota exhaustion without a final report; new independent review
remains pending until the reported reset2026-11-20T23:53:18Z. Do not retry early
or change account/model configuration. The optional VPS Claude second opinion
returned "Not logged in"; it did not perform a review.

## Current round — 2026-11-15, Fan33 accepted by owner

**Latest instructions:** publish the EMC/tools sources and tables, then implement
landscape. The owner has finished manual testing and reported success ("完美").
SSH/SFTP is disconnected; no remote commands will run until device access is
explicitly returned. Do not let older authorization or baseline entries below
supersede this section.

| Item | Current result / next action |
|---|---|
| Installed pairing | Fan33/Core7/Battery14; WinX2500MHz boot request, original `.158`+bit14 KMD and UMD/ICD; bidirectional GPU bridge, login+5s, dynamic512MiB GPU budget |
| RAM |204/1331.2/1600/2500MHz trained points; Noble ACTMON watermark IRQ plus CPU/display bandwidth floors; no manual RAM lock |
| Native readiness | Boot16:11:46UTC, enabled16:14:58UTC;16:18:12UTC Code0, RAM1600MHz,532 completed transitions, IRQ flag256, all EMC/ACTMON/QoS errors0 |
| Power choices | First-use/reset Handheld; independently restore last confirmed power/fan choices. Final check OC_CPU2/Cool2, charge85% (80–90% band) |
| Desktop tools | Both responding in session1; saved startup, Joy-Con icon,50% Pacecar opacity, fresh battery net power and live RAM readback |
| Acceptance | Owner manual test passed. No agent Fan33 workload, duration-specific stress, new landscape or vkcube acceptance claimed |
| Next | Keep the accepted power/platform pairing; implement four-direction landscape display, then native Win32 vkcube. SC7 off; FPS/DX12 deferred |
| Cleanup | Final optional cleanup command timed out with no receipt. Do not claim probe cleanup completed or reconnect only to check it; no local SSH/SFTP or load-test process remains |

[Current component hashes, source references and remaining governor differences](DVFS_REVIEWED_20261114.md#noble-alignment--2026-11-15).
Exact local receipts: `OUT/emc-runtime/noble-profile-tools-20261115/`, especially
`fan33-install-final-parsed.json`, `fan33-bundle.json`, `ssh-disconnected.json`.
Source manifests preserve the pre-commit build identities. Four unrelated Mesa
`src/` resource/API-marker edits are not part of this EMC publication or installed
UMD/ICD and require separate validation before any graphics update.

## Historical records (later user instructions supersede holds below)

更新：2026-11-14（本机时区；交付回执使用 UTC）。包含原生桌面、冷启动探索、旋转
候选及后续审查；较早交接状态按时间保留，当前设备约束以本节为准。

## 当前设备授权（2026-11-15 UTC）

**最新用户修订：EMC本轮暂记测试通过；下一阶段为Tegra DC列扫描横屏，随后Win32原生vkcube。**
保留`.158` KMD、bit14、当前修复ICD、双向GPU Fence、登录后约5秒启用tgd3d、Handheld1、85%充电限制和动态512MiB GPU预算。
横屏采用Tegra DC列扫描，协调0／90／180／270度的源表面布局、pitch／offset、VidPN模式、VBlank锁存与真实翻转完成通知；物理面板保持720×1280，正常手持逻辑桌面为1280×720。
以同机Noble新读回及微软WDDM2.6契约为参考，保留每台机器既有的面板／DSI时序；不得全局写死该OLED面板参数，不整体回退较新的显示／资源退休／Fence修正，不把GPU预旋转作为本阶段默认路径。
四方向分别构建、独立审核及验收；无实屏证据时明确未验收。完成横屏后再验证原生vkcube；FPS、SC7和DX12继续后置，SC7默认关闭。
本轮RAM实机记录：53次采样、204000→1331200→204000kHz，成功切频计数2→4，所有EMC／QoS／ACTMON错误为0、采样最大年龄171ms；Handheld→Console→Handheld成功，CPU错误0，GPU Code0，启动时间保持12:29:50。
Fan28于12:33:06完成安装，DWM PID780加载原UMD／ICD；Pacecar与Switch Power在session1自动启动。1600000kHz启动读回已记录，持续高负载1600MHz尚未验证，不扩大本轮通过范围。
回执见OUT `emc-runtime/fan28-native/installed-candidate-native.json`、`memory-load-summary.json`、`mode-switch-summary.json`。

**最新设备限制：用户接回实机自己测试，明确停止SSH以免增加负载。本窗口只做本地源码、构建、审核和打包；用户交回前不连接、安装、重启或远程取样。**
最后一条SSH连接在建立前超时，未执行拟议的身份补查及清理；已没有本窗口在运行的SSH／测试进程。Fan28安装任务已禁用，Fan26／Fan28暂存任务无触发器，实际负载已结束。
临时CAT目录、上传ZIP、Bounded-EmcLoad脚本及三个已完成任务的清理留待重新授权设备操作；恢复包和已取回结果继续保留。首次独立NVK身份检查缺少GM20B实验设备开关而退出10：KMT LUID为`00000000000b4e07`，不能算Vulkan枚举通过；不据此认定桌面或EMC故障。
应用目标卡片仍为进行中；现有目标工具只支持修改状态，不支持正文，自动控制Codex应用也被工具拒绝。因此这里是最新工作目标修订记录，不能声称已经修改了目标卡片正文。

以下为按时间保留的历史记录，当前授权及路线以上述最新用户修订为准。

**最新：用户明确“你可以链接switch测试”，本窗口设备操作授权已恢复。先读取实际系统／组合，再完成EMC匹配更新和验证；EMC通过后才安装、测试列扫描横屏。此次授权覆盖下面10:18的临时本地限制。**

**12:28 UTC：Fan28匹配包完整预检通过，已发起一次安装任务及计划重启。尚无动态EMC通过结论。**
10文件独立审核已经收尾；内核收尾事务未发现阻断问题，审核指出的包清单Fan27／门禁Fan28不一致已按一行建议diff修正。
错误包未启动安装或重启，远端已清理。重建包在设备上同时通过Merge-Check和Check-Package，保留当前KMD／UMD／ICD／固件／ACPI与Fan26恢复组合。
修正包ZIP SHA256为`b73a483a9bb9aa6bb9960ee4328396ddfa760ca292eae1a4621f19b5277c6bc6`；任务`Tgd3dEmcFan28Stage`只启动一次。
审核新增全部部分归零组合、无回调／错误目标、失败回退及电压票保持控制通过，独立ARM64构建与本机构建逐字节相同；这不代替实机验收。
审核CLI进程退出0，但聚合终结仍带quota error，按实际交付文件核验接受，未记录CLI成功。详见OUT `gemini-full-audit-20261115/post-mtc-zero-delta-20261115/DELIVERY-VERIFIED.json`。

**12:11 UTC 实机最新：Fan27试验失败后，已恢复原Fan26／`.158`／Code0、Handheld1／85%。**
计划重启后于11:49:53启动；Fan27先通过平台安装与1600000kHz读取，GPU原生启用后在设置Handheld的IOCTL `0x22a04c` 返回Win32错误21。
安装器于11:53:03启动恢复，11:53:49再次启动，11:56:21完成恢复；第二次为恢复脚本发起，不能当成硬件看门狗重启的证据。Handheld未就绪的具体条件仍未确定。
这次单独的变频后快照已定位EMC的真实-2：204000kHz后`grant/CPU_READ/CPU_WRITE`全为0，DIS16、RING1 53、RING2 12、其余ISO／LA／DRAM配置保持已准备值。
当前恢复后的Fan26实际停在1331200kHz，仍是`QosError=-2`、成功计数0；用户看到RAM1331与此相符，不代表动态调频通过。
原始回执及恢复证据见OUT `emc-runtime/fan27-native/native-diagnosis.json`、`recovered-state.json`、`cleanup-restored.json`。
Fan27临时安装／恢复任务、签名暂存目录和远端ZIP已清理；保留匹配恢复包、运行日志及本地证据，没有重复安装同一失败候选。

**Fan28本地修正已构建：只处理已确认的“变频后三项时钟配额全零”状态。**
独立收尾回调要求pending事务、GET_RATE与目标一致、所有非时钟字段保持不变；先写grant并锁存验证，再写CPU配额并锁存验证。
不修改DIS／RING／其他客户端；部分清零、无pending、其他字段变化继续失败。部分写入失败先清CPU并锁存，再恢复零grant并锁存，仍返回失败，不伪造完成。
真实控制器调用链、三档所有组合、五个写失败点／丢写、shadow与immediate CPU模型、异常拒绝及ARM64构建通过。
新增安装器policy设置前只读状态快照，用来定位21的来源；未放宽该失败处理。未签名SYS SHA256 `45238c88ddbc5af312bb9b79da1a94ad2a146b26825c1a3810f8496148730fb3`，
签名SYS为`93bfb84a1c909ed7225bd6ef0841fe3622b69982e81f453542d3b7d93719860d`（89608字节）；尚未上机验收。10文件差异由同一Gemini会话独立补审。
源码与控制见OUT `emc-runtime/post-mtc-zero/`，待安装包见`emc-runtime/fan28-native/`；显示KMD／UMD／ICD、固件与ACPI保持当前组合。

**2026-11-15 10:18 UTC起：用户接回Switch切换Noble，明确要求先本地开发。停止SSH、安装、重启及所有实机操作，直到用户重新交回设备。Fan26的临时目录／无触发器暂存任务清理也后置；安装任务已完成并自行禁用，没有新发恢复任务。**

**11:19 UTC 本地最新：Fan27新增切频全配对准入检查，修复已有较高父配额时“降得下去、升不回来”的边界。**
旧源码在Ring295／300场景下允许先降到204MHz，新回归明确失败；修正后在任何MC寄存器／时钟写入前拒绝，Ring294边界及现有三档正常升降通过。
对已有未知配额继续保留，不能因新增显示需求而挪用；Dis0／Ring274的8个额外配额保留后为Ring282，重复重绑不会继续增长。
独立审查另指出静态AML名称校验与第二阶段prepare失败记录缺口，已处理；147项ACPI控制通过，生成的4839字节表仍为`50b68ede…`，显示配置未改变。
本地ASan／UBSan、NDEBUG与ARM64构建通过，最新未签名SYS为`b30983b45b1ca3da21b54329657ad88d4ecd102f882b66482d0e67a78a48f202`（84992字节）。
源码／控制在OUT `emc-runtime/qos-admission-review-fixes/`；此前两个未安装Fan27构建已被替代，不能误用旧产物打包。
9文件最终报告／建议diff已收到并核验9/9源码哈希；CLI退出0，但聚合结果仍带与上一轮相同的quota ERROR，不把它记为CLI成功。
4文件修正冻结为`gemini-full-audit-20261115/qos-admission-delta-20261115/`，按最新提示等待到11:43:28 UTC后补审，不改账号配置。
动态EMC仍未实机验收，Fan26的post-clock -2具体字段仍未知。

11:28 UTC：同一源码的Fan27 SYS已完成本地Authenticode签名与校验，签名后88072字节，
SHA256 `61bdd10d0fd90d2388e7e64ba198af0c59969e42ceefcc6ff2bab90053c264af`；INF为`83ecca22d16d847c36e5509ba2e403139ad255cd4257c0856a91466790ad06e3`。
尚未生成CAT／匹配安装恢复包，尚未安装，不是可发布的完整驱动包。回执见OUT `emc-runtime/qos-admission-review-fixes/signed-component.json`。
固定Hekate源码与Switchroot bootstack文档（`5a3fec9cf3a19dab20e4868cf4a872e208ae3df8`）已核对：公开Minerva针对T210，B01由独立BPMP固件／MTC表处理，后两者源码未公开。
本地WinX仅包含MRQ_CLK调用端，不能据其成功返回推断BPMP对MC字段的具体写入；没有据此修改固件或猜测实机-2的根因。

**侧聊天新交接：EMC完成后，横屏改回Tegra DC列扫描路径，按每台机器既有面板配置工作。**
侧聊天报告同机OLED在Noble24.04.5／内核#149实屏正常，两次Window A读回为WIN_OPTIONS `0x40010011`、stride `0x1400`、H_OFFSET `0x13ff`、V_OFFSET0，物理720×1280／60Hz。
这覆盖较早Noble2024内核SCAN_COLUMN=0的观察，但不能替代Windows实屏验收，也不能把该机DSI参数全局写死。
Windows隔离探针回执Stages7／ProbeStatus0／RestoreStatus0；逐行图获用户确认，列扫描的“纯绿”反馈后来有疑问，仍记实屏未确认。
DWM两张图的回执为DwmFlush0，用户确认完整；独立D3D11第一次Present为`0x887A0005`，不算呈现通过。
侧聊天已于11:00:58恢复原KMD `e95287cb…`，记录Code0，后续报告tgd3d UMD `4c8a13d0…`及默认启动任务正常、临时项已清理。
本窗口未据此恢复设备操作。交接与原始文件哈希见OUT `emc-runtime/qos-admission-review-fixes/column-scan-handoff.json`。
当前源码中，DC提交会清除列扫描位，KMD主表面只接收竖屏布局，tgd3d则要求runtime走旋转Blt；三处必须协调改动，不能只恢复一个寄存器位或整批回退旧提交。
后续还要把旋转布局纳入锁存完成比较，并单独核对列扫描取数的EMC带宽条件。只完成本地源码定位，显示源码尚未改动；7文件哈希和微软契约见OUT `dc-column-scan-restoration-20261115/source-audit.json`。

执行方式按用户最新要求自主判断；原目标中已被用户撤销的技能要求不再适用。

**最新工作顺序：EMC内存调频修复 → 横屏上屏 → Win32原生vkcube。Gemini并行接续NVK Windows、Zink/OpenGL与已有DXVK8–11工作；DX12与SC7后置，SC7继续默认关闭。**
**07:20 UTC 实机确认：Fan21/Core7/Battery13与新版KMD/UMD/ICD、固件和ACPI已整套安装。用户要求保留本组合为工作基线，修复RAM调频后再升级，不回退旧版。两个应用已在本次实际重启后自动运行；FPS暂缓。**

**07:58 UTC 后续：Fan22候选已安装，07:53:25计划启动后于07:56:40完成原生启用，GPU Code0，DWM PID816加载原UMD/ICD。显示、固件、ACPI均未改。Fan21仍是已冻结的恢复组合；动态EMC尚未验收，不把Fan22标成调频修复完成。**
这次已明确枚举失败位置：固件MRQ32返回204000/1331200/1600000kHz三个有效电压点，电压全部1100mV，
在驱动1050mV上限校验处被拒绝，尚未调用ROUND_RATE。允许重复频点并未解决当前故障；下一步核对
T210B01的Linux电压归属和固件训练档位，不能仅抬高电压上限来让检查通过。
回执及完整数据见OUT `emc-runtime/emc-enumeration/phase-6.json`、`enumeration-diagnosis.json`。

**08:18 UTC 最新状态：Fan23于08:11:31原生启用，启动时间08:08:22，DWM PID676加载原UMD/ICD，四设备Code0。**
Mariko DVB选择逻辑修正后，真实枚举204000/1331200/1600000kHz三档，EnumerationError0、ACTMON有效。
剩余失败为显示带宽QoS检查返回-2；尚未成功切换频率，TransitionCount0，RAM保持1600000kHz，不能记为动态调频通过。
原始MRQ32诊断值已由Fan23清除；新数据见OUT `emc-runtime/mariko-dvb/phase-5.json`及`installed-candidate-native.json`。
用户曾手动选择Perf_OC_All6并关闭工具，随后明确交回设备控制；08:18已恢复Handheld1。
桌面新增用户要求的“打开硬件工具.cmd”，普通交互令牌实际运行退出0，两个工具运行于session1，前后模式均1、充电85%。
源码位于WindowsDrivers `app/SwitchPowerTray/Open-HardwareTools.cmd`；按用户要求等EMC修复结束一起提交推送，不单独push。

**08:59 UTC 更新：Fan24第一次安装因显示配置保存工具的完成通知超时而中止，未启用原生GPU。08:53:44已恢复Fan23／当前`.158`，Code0、Handheld1和85%充电限制核对通过。**
显示工具实际输出status0并写入1192字节快照；其JSON约28秒后落盘，安装器只等25秒便停止任务，留下0字节退出回执。
`Prepare.ps1`现改为临时文件写完后重命名发布退出码，拒绝空／畸形回执，等待90秒、任务限时100秒，并分别报告超时与非零退出码。
直接运行同一生产代码块的原生控制于08:58:00通过：5.1089秒、退出0、快照1192字节，任务已清理；显示只读状态仍为720×1280／60Hz。
这不是EMC／GPU验收。Fan24七文件独立审核已收尾PASS；新增Prepare一文件另行补审，不阻塞已授权部署。
修正安装包见OUT `emc-runtime/snapshot-completion/matched-bundle.json`，Fan24 SYS仍为`137abbfb…`；KMD／UMD／ICD／固件／ACPI未改。
诊断目标仍是首次QoS prepare的17个寄存器，成功定位并修正契约后删除临时快照代码；尚无调频成功记录。
桌面启动脚本的独立复核已验收；两条可选加固建议不影响当前交互式桌面用途，处置见OUT `emc-runtime/desktop-launcher/review-disposition.json`。

**09:05 UTC 实机更新：修正完成回执后的Fan24于09:04:38完成原生启用，启动时间09:01:19，DWM PID780实际加载当前UMD／ICD；四设备Code0，Handheld1／85%，两个工具随登录运行。**
17个PTSA只读值已明确唯一失败谓词：`MC_PTSA_GRANT_DECREMENT=0`；DIS/RING/CPU及其它已读PTSA rate也全为0，
DIS/RING min/max为0x3f，LA为0x001e001e，DRAM配置0x9160a00d的低两位确认为LPDDR4。
这说明旧实现“固件已经初始化grant/CPU PTSA”的前提在当前启动链不成立；不能删掉非零校验后直接SET_RATE。
参考固定Switchroot `76e6d489…` 的独立`tegra_latency_allowance_init → t21x_init_ptsa`和Hekate `8e1392df…`的MC up/down寄存器更新顺序。
WindowsDrivers新`emc_ptsa.h`已移植LPDDR4的grant/CPU速率计算，保留上游MHz截断和DDA取整；ASan/UBSan及逐位参考对照通过。
**该计算尚未接入MMIO，动态EMC仍未通过**；需要完成参与客户端初始化、ACPI授权范围、时序锁存与变频前后归属，再独立复核及上机。
当前Fan24是定位用候选，不是调频已完成的发布基线；失败快照代码在契约修正实测后移除。安装暂存任务／CAT临时目录已清理，保留匹配恢复包。
数据见OUT `emc-runtime/snapshot-completion/qos-diagnosis.json`、`installed-native.json`和`emc-runtime/ptsa-init/clock-port.json`。

后续本地实现已补上24组客户端的PTSA配置计划及事务函数：先验证复位状态（包括未改RATE的其它客户端），
按上游先写grant并触发MC时序锁存，再配置父队列和客户端；保留保留位，逐项读回，任何部分写入失败都恢复完整原值并区分回退失败。
ASan／UBSan控制覆盖全部60个写入失败点、丢写、混合状态拒绝及回退失败。09:54已完成WDF映射与QoS调用链接入、最终ARM64构建及组合控制；这些仍只是主机控制与构建通过。
同机Noble启动参数已确认`latency_allowance.ptsa_rework`。Minerva up/down表不包含显示A的DC_0／scaled LA，
因此保留它们的严格值比较；此前关于显示LA会随该表更新的初步推断已撤回，不据此放宽检查。
当前资源、复核及实机步骤见OUT `emc-runtime/ptsa-init/WORK-IN-PROGRESS.json`。
ACPI工具已补FAN0／完整设备列表重编译比较及清单文件覆盖保护，正向重编译与当前4106字节AML逐字节相同，负向控制拒绝损坏FAN和增加设备；
完整98项Memory32Fixed及权限验证已接入；98项权限逐一破坏、错误FAN范围／状态、普通固件表及旧表的负向控制通过。最终13文件增量复核仍在进行，不能把工具检查记为EMC硬件通过。

**09:58 UTC Fan25部署进度：SYS／INF／CAT原生签名校验通过，88文件匹配包与实际Fan24恢复身份核对通过；09:58:41只启动了一次安装任务，09:58:58记录为BackingUp，随后SSH在计划重启期间断开。尚未确认Fan25安装或调频成功。**
Fan25 SYS为`7df099f5a470c55956fc43aecd8f6be589e9b2d850ec5582e4c9dbe954aa75bc`，显示覆盖AML为4839字节、
`50b68ede51d6a60cf97281fe4e7930a8254db911fce2f5e6d9530cfa711b3c3d`；只替换FAN0资源，保留其余24个显示／平台设备。
当前恢复目标是实装Fan24 `137abbfb…`与相同`.158`图形组合，不是早期Fan21／Fan23。完整包、签名、源码冻结与回执见OUT `emc-runtime/ptsa-init/`。
PTSA算术审核的显式策略与NDEBUG测试两项建议已落实；后续60写事务／WDF／ACPI不冒用该算术审核的覆盖结果，另有13文件独立审核在途。

**10:03 UTC Fan25结果：09:59:44计划启动，10:03:00完成原生启用，DWM492加载原UMD／ICD，四设备Code0；EMC初始化读回返回-4，事务回退未返回-5，尚未SET_RATE，RAM1600000kHz、TransitionCount0。调频验收失败。**
发现主代码在grant写入与MC_TIMING_CONTROL之间提前比较读回，未覆盖影子值在锁存前不可见的情形。
Fan26改为完成该写入对后、任何客户端获得非零credit之前验证grant。旧顺序在新增影子寄存器模型中明确失败，新顺序与原立即读回模型、全部60个写失败回退、丢写控制通过；ARM64已构建。
这尚不能证明Fan25的全部实机失败都由该顺序引起：Fan25没有保存具体失败offset。Fan26只记录既有必需读回的首次不匹配offset／expected／observed三个DWORD，不额外扫描MMIO；与原临时快照在确认修复后一起删除。
当前保留Fan25的安全拒绝状态，未追加重启或重跑同一失败候选；Fan26进入签名打包，资料见OUT `emc-runtime/ptsa-latch/`。

**10:17 UTC Fan26结果：10:13:09计划启动，10:16:24完成原生启用，DWM772加载原UMD／ICD，Fan26／GPU Code0。BPMP实际读回已经由1600000变为204000kHz，但变频后QoS检查返回-2并锁存，TransitionCount仍为0。**
因此“频率已改变”通过，“完整动态调频”未通过；这轮不能记为升降频修复完成。用户随后确认画面似乎正常、Pacecar显示约200MHz，切换模式仍约200MHz，与驱动暂停后续调频相符。
Fan26没有bootstrap首次不匹配记录，说明锁存修正已越过初始化检查；当前缺少的是变频后具体哪个QoS字段不合法，不能直接认定为grant被BPMP清零。
保留现有严格检查；本地补齐按真实频点计算的预算及失败读回。Gemini概念审核因额度耗尽中断，其scratch建议已发现旧固定预算在204MHz的grant下超过100%：DIS216×4≥589，RING571×2≥589。
该建议未正式交付、未验收，并且仍含旧策略／锁存实现，不能整份覆盖当前源码。它只模拟“固件更新正确值”或“固件保持旧值”，未覆盖本轮实际-2的未知字段，不能用其模型PASS冒充本轮故障已修复。
实机记录见OUT `emc-runtime/ptsa-latch/phase-3.json`及`user-handoff-noble.json`。三个Gemini会话均因quota ERROR停止，最早10:28:32 UTC后再恢复；优先收尾最新源码审核，不重新全量探索。

10:27本地进度：新增`emc_qos_budget.h`按实际训练频点计算DDA预算，三档得到display16、RING53／232／274，并拒绝达到100%的过量预留；未将该规划器接入WDF切频，因为Windows私有BPMP在切换后的寄存器归属仍未证明。
QoS结构单独保存必需的变频后读回，失败时一次记录，不再用初始化前的17字快照解释变频后的-2。控制覆盖grant为0、另一种CPU字段越界和读中断清零，避免把不同协议失败误认成同一根因。
本地Fan27 ARM64构建及相关ASan／UBSan控制通过；**未签名、未安装，未解决或验收全部EMC同步问题**。用户仍持有设备，本窗口没有恢复SSH。代码与待办见OUT `emc-runtime/post-clock-local/status.json`。

**11:05 UTC 本地后续（覆盖上一段“规划器未接入”的实现状态，实机结论不变）：频点预算、clock配额事务和WDF调用链已经接入。**
降频前先降低CPU配额并锁存，随后降低父配额，再降低grant并锁存；升频前先提供父配额余量，确认实际升频后再完成grant／CPU项。按真实三档检查全部切换组合，不使用虚构训练频率。
事务保存保留位，在写入前检验中间状态，部分写入失败按相反方向完整恢复；读回不符与回退失败分开。整体控制直接调用生产`sw_emc_set_rate → QoS → PTSA`，验证两种已知固件行为（保持已准备值／写入正确目标值）及三类未知字段错误。
**该实现仍拒绝未知或零grant的变频后结果，并保留失败读回与电压要求；没有据此声称解决Fan26的实际-2。** 正常／NDEBUG的ASan／UBSan、每个部分写入失败点及ARM64完整驱动构建通过；未签名、未安装。
Gemini的15文件报告、覆盖清单和建议diff已收取并核验哈希；CLI终端仍带quota ERROR，不把CLI记为成功。F1的预算与时序缺口在后续源码中推进；F2首次检查失败清prepared、F5不可删除的CHECK控制已处理；F3补QosMap重复／只读资源保护；F4补静态AML作用域及资源流边界。
主窗口额外修正了建议解析器仍可接受root Device／嵌套相对_SB的问题；98项权限翻转及9类异常输入全部拒绝，IASL重编译仍与已安装4839字节表`50b68ede…`完全一致，没有改变显示表。
当前本地源码与控制见OUT `emc-runtime/qos-clock-integration/`；9文件新差异于11:05交给同一Gemini会话补审，目录`gemini-full-audit-20261115/qos-clock-delta-20261115/`。NVK／Zink／既有DXVK任务于11:09恢复原整理会话，先收敛现有52文件改动和多架构构建，不扩展DX12或操作设备。

| 项目 | 当前结论 | 仍待完成 |
|---|---|---|
| Switch Power | SHA256 `b4dc8feab13e13cf84ebac1addc57d1d0d1f14777f929775cd128cd9957ee1da`；07:06:47启动后自动运行于session1，登录任务启用；Console→Handheld实际切换断言通过 | 充电电流效果另需在相应供电和电量条件下验证 |
| Pacecar | SHA256 `fdf4949e1a78934e259218a176f471516b8ab8d2baf3b3114ecdee43864d5469`；同次启动自动运行于session1，登录任务启用；原生采样返回 `ram_khz=1600000`、valid127、error0 | FPS按用户要求暂缓；动态RAM随EMC修复验证 |
| 审核 | 9文件最终报告已核验；准入／解析器／失败记录问题已处理，4文件修正已冻结 | 补审按额度提示排队；实机条件未满足 |
| EMC | Fan26实际204MHz、post-QoS=-2／完整成功0次；本地Fan27已接入预算／配额事务及所有升频组合准入，控制与ARM64构建通过 | 未安装最新候选；动态EMC仍未验收，未知实机字段继续拒绝。等待用户交回设备后明确post-clock契约 |
| 横屏／SC7 | 接收新版Noble同机列扫描证据；Windows列扫描实屏仍未确认；SC7默认关闭 | EMC完成后按现有面板时序接回DC列扫描并验收，再vkcube；SC7后置 |

当前组合的完整哈希、安装回执、源代码位置与未完成项见OUT `tgd3d-current-baseline.json`的active记录和`emc-runtime/progress.json`。
`emc-runtime/fan21/installed-working-baseline.json`只保留最初冻结组合；Fan25包恢复目标为实际Fan24 `137abbfb…`，不能把Fan21或Fan23冒充本轮恢复目标。
KMD `.158` 为 `e95287cb…`、Core7为`54c5529e…`、Battery13为`b2f53b48…`；UMD `4c8a13d0…`与ICD `95b97df4…`不变。
固件为`d1387482…`，显示ACPI为4106字节`caeeb93b…`。Handheld1、85%充电档、双向Fence及登录后启用保留。
安装/恢复任务已禁用，默认登录启用任务仍启用，没有待执行回退。
显示状态工具只读返回当前及保存模式720×1280、0°、60Hz；该工具未输出LUID，本轮LUID尚未补录，不能用上述结果代替实屏横屏验收。
Console/Handheld第二次控制的实际字段断言通过，但输出用Select-Object直接处理Hashtable导致摘要字段null；
原始回执已标注该工具格式问题，不能把null当成设备值。随后完整读回确认Handheld及目标上限。

用户随后取消了Gemini的DX1–12图形兼容及其缓存扩展任务，要求删除本轮任务和隔离源码/产物。
任务已停止、隔离工作树及任务目录已删除，没有兼容改动合入当前主基线；不得自动恢复此任务。
最小取消回执见OUT `cancelled-dx1-12-20261115.json`。该自研前端任务不恢复。
用户随后另行授权接续NVK Windows、Zink/OpenGL和旧DXVK8–11工作，DX12后置；新会话先按要求停止，
主窗口核对17项来源提交及旧验收后整理统一交接，再恢复同一新会话。完整范围和提交矩阵见OUT
`delegation-nvk-windows-20261115/CONSOLIDATED-HANDOFF.txt`及`INVENTORY.json`；禁止侧任务操作实机或整批合并main。

以下06:25及更早安装过程保留为历史记录；“旧驱动”“待安装”和“待审核”不覆盖上面的当前状态。

应用更新回执：OUT `emc-runtime/review-fixes/desktop-tools-reviewed-installed.json` 与
`desktop-tools-reviewed-final-readback.json`。05:15第一次请求SSH断开；05:19确认没有安装任务／回执且仍为旧应用，
之后才重新发出更新。05:20:53–05:21:01安装完成，退出0；没有安装驱动、改电源模式或请求重启。
备份保留在设备 `C:\Users\ruje\AppData\Local\SwitchWOA\Backups\desktop-tools-20261115-052054-856`。
临时更新任务及未执行的旧应用暂存包已清理，没有创建桌面快捷方式。06:09:34恢复重启后，06:24读回确认两个应用和任务均在运行；凭据为OUT `emc-runtime/review-fixes/lifecycle/current-after-resume.json`及`matched-recovery-status.json`。

06:06首次匹配安装错误地把普通固件DSDT作为显示覆盖表，缺少`SWOA0F01`设备，安装在平台驱动更新前被门控检查阻断。06:12已恢复原固件、ACPI、驱动和Handheld/85%。该包标记禁止重装。修正工具只把新平台FAN0资源合入原显示覆盖表，保留其余24个设备；结果4094字节、SHA256 `94ce1cbe308e35d1b54d2abaf13f88f95a3bdf9e67313feffb087da4084ba94f`。错误的普通DSDT已被负向控制证明会在创建包前拒绝。修正包为OUT `emc-runtime/review-fixes/lifecycle/matched-emc-acpi-20261115-0627`；不得把已上传和签名通过当作安装通过。

本轮生命周期补丁位于OUT `emc-runtime/review-fixes/lifecycle`；独立复核目录
`gemini-full-audit-20261115/lifecycle-closure-20261115`，会话`e661211e-a61b-4876-9eb3-b363841db7b2`。
控制覆盖延迟IRP完成晚于Close、完成例程注册失败、PnP移除发生在句柄发布期间、重连索取电源轨失败，
以及未隔离GPU不得交还电源轨；EMC停机控制使用实际函数和WDF/BPMP替身，检查读回及释放失败传播。
这些均不是GPU／DRAM硬件验收；生命周期复核及最终签名已完成，当前待审仅为新增ACPI打包修正。不能使用旧的作废签名包。

以下04:31及更早内容保留为历史记录，涉及“尚未安装”的应用状态以上表为准。

**04:31 UTC 收尾进度（不覆盖下列实机基线）：** 1285项全量和81项增量审核均已收到并校验交付；
本轮23项修正仍在同一Gemini会话复核，目录`gemini-full-audit-20261115/review-fixes-20261115-0404`。
新版Pacecar ARM64为`fdf4949e1a78934e259218a176f471516b8ab8d2baf3b3114ecdee43864d5469`，
配对Switch Power为`b4dc8feab13e13cf84ebac1addc57d1d0d1f14777f929775cd128cd9957ee1da`；
更新包已上传并核对四个成员，目录`C:\ProgramData\SwitchWOA\GpuUpdate\desktop-tools-review-20261115`，
**尚未执行安装、重启或电源模式变更**。04:14读回仍为旧Power PID6136、Pacecar任务Disabled且进程缺失。

为避免继续使用OUT中临时修改的旧脚本，canonical部署链补齐Core驱动备份和恢复顺序，
取消Fan13/固定7恢复判据、保留登录启用任务、原生启用后明确应用Handheld/85%。
这3文件已另交Gemini会话`e661211e-a61b-4876-9eb3-b363841db7b2`审核，目录`deployment-delta-20261115-0422`。
后续发现GPU打包/Prepare/Common仍绑定旧哈希和Fan13，已新增`reviewed-dvfs-emc`匹配包路径，
固定回退当前`3ada55a4…`完整SYS身份，只允许Fan20、双向Fence和登录启动；新增3文件冻结于
`deployment-helper-delta-20261115`，已单独并行交Gemini补审这3个新文件，不重复此前范围。Python语法及全部新增PS的Windows原生Parser通过；
直接冷启动、未核验签名和错误Fan版本的打包输入均被拒绝且未创建输出目录。解析临时文件已删除。

当前最新未安装构建：Fan20 `226932c4…`、ICD `95b97df4…`、固件`b98d0faf…`、
DSDT `3d6b8678…`、M06 UMD `4c8a13d0…`；完整哈希和验证结果见OUT的`emc-runtime/progress.json`
及`emc-runtime/review-fixes`。GPU初始EMC电压保留和异步I/O/PnP生命周期仍在收尾，
不要使用作废的`catalog-20261115-0204`，也不要把本地构建或包已上传写成安装/开机自启验收通过。
04:34已核对实机现存ARM64 `Pacecar.Sensors.exe`：`60966d1351aeb7f91fe3ce207877a948cb7272181659fe5f4db9e1f4a273a065`，
与已安装整合包清单一致；采集进程当前未运行。FPS真实事件/节拍及界面仍需随更新验收，不能把组件存在当作功能通过。

**最新约束与进度（2026-11-15 UTC）：** 正常启动、部署及恢复均为 Handheld（模式1）；
充电默认85%档（80–90%迟滞）。00:12实机读回该档有效。用户随后再次报告Pacecar未随重启恢复，
因此旧的自启动通过记录不能代表当前问题已解决。源码已定位并修正Pacecar启动时用默认false
反向禁用登录任务的问题，ARM64构建通过，新的实际登录验证尚未进行。
03:22 UTC只读SSH再次确认：Switch Power PID6136及其登录任务正在运行；
Pacecar登录任务为Disabled，按`Pacecar*`查找也没有运行进程。两个工具尚未完成最终交付，
EMC动态调频候选仍未安装；不得将旧的一次自启动通过记录当作当前已经解决。

首轮审核的M-06已按建议单独修复：动态纹理Unmap在无法创建命令时释放尚未提交的上传Slice。
x64/ARM64构建及真实DDI CPU控制通过；使用旧ResourceUnmap运行新控制会因上传Slice未释放而失败，
修正后通过。该控制不执行GPU；修正未安装，不宣称它已解释横屏或异常重启。
完整源码哈希、构建及反向控制存于OUT的`resource-unmap-audit/umd`，新增控制已冻结待补审。
WinX离线安装脚本也已应用首轮审核的单行SWOA0601重绑修正，与WindowsDrivers副本保持一致。

Antigravity CLI / Gemini 4 Carbon High 正在审核本窗口全部现行自研及修改代码，冻结清单1285项，
包括相对固定上游的所有Mesa差异、WindowsDrivers、WinX、私人Pacecar和部署工具；不只审本轮diff。
会话 `7f9f6a7f-3fd0-404b-8da3-f6444071c32a`；清单、逐文件进度和建议补丁位于OUT的
`gemini-full-audit-20261115`。未改动的上游Mesa排除；02:11覆盖表已记录1211项已审、
40项上下文、34项二进制资产、0项未审，首轮于02:44:57 UTC正常完成并交付报告和六份diff；主窗口已核对清单、快照与产物。
独立审计交付已收到，建议补丁尚未整批采用，错误处理/电源/指标真实性等建议需分别核实。
用户追加规则：以后每轮新增代码也由Gemini独立审核一次，相同源码版本不重复，修改后补审差异；
已记录于根目录AGENTS.md。本轮81项新增EMC/ACTMON/QoS及修正于02:48:28 UTC在同一会话启动增量审核，
快照`followup-20261115-022016`；原首轮报告、快照和补丁保持不变。

EMC运行期BPMP读回、CPU/Core/GPU电压协同已进入本地实现：新GPU电压请求在空闲边界确认，
失败明确拒绝，GPU隔离后仍保留RAM所需的共享电压；未知RAM读数不再冒充启动1600MHz。
ACTMON独立采样定时器、负载选档及显示带宽保护已接入，原生GPU所有权及电压确认就绪后才调用
SET_RATE；频率和QoS读回成功后才降低电压。相关控制与ARM64构建通过，4份SYS/CAT已签名，
Windows签名校验22个成员全部通过。结果存于OUT的`emc-runtime`；增量独立审核和实机验证待做，
这批修改没有安装或推送。01:55 SSH读回`.158 / Code0`，Switch Power在运行，Pacecar未运行。
实机仍为下述Fan19/Battery12及原KMD/UMD/ICD组合。
03:33增量审核发现并由主窗口用固定Switchroot源码确认：未安装候选误将ACTMON 119映射到CAR U组。
驱动和ACPI已改为V组，状态读取也改用正确的0x358/0x360寄存器；ARM64编译和地址控制通过，
旧地址在同一控制中失败。`emc-runtime/catalog-20261115-0204`已放置`INSTALL-BLOCKED.json`，
禁止继续安装旧签名包。源码MUX采用固定上游的bits31:30/parent3，未采纳未经证实的三位MUX建议。
新增修正及剩余电源生命周期/API分类问题需完成复核后再出包，不能把81项覆盖表完整当作交付结束。
SC7交付继续后置、默认关闭；待EMC/Pacecar实机启动验证完成后统一更新表格并按仓库各推送一次。

**23:49 UTC历史实机记录：用户指定以后正常部署、测试及恢复均以 Handheld（模式1）为基线，
固定7不再作为菜单选项或正常默认。实际读回模式1，CPU DFLL和GPU动态策略已启用。
此前模式0–6短时切换通过。本轮两个工具改为每用户交互登录任务，23:46:28 UTC的一次
计划重启后均已自动运行于session1，并读回Handheld；双向Fence保持开启。当前`.158 / Code0`，目录
`C:\ProgramData\SwitchWOA\GpuUpdate\tgd3d-b1a77251-20261115-051506`，KMD
`3ada55a42a3a116d41951a452893b2c09e3f01cc440e67ffd4b8b06b6d5c7918`，UMD
`8d7da8c1a3d9e9556a8e88b2b8bef67956dcf588c980d763e5f6684aaf6489c2`，ICD
`30a8d6a97d96d917b93ecb1f8a34489ce5613ca2082752b04c5823d6cb408e8d`。
Fan19/Battery12、DFLL handoff v2固件及扩展ACPI保持；Handheld的CPU上限1581000、
GPU上限537600kHz，RAM当前1600000kHz，充电限制0/off。登录后5秒启用，生产双向GPU Fence保持开启。
本轮恢复目标是此整合配对及竖屏，不是下文历史I配对、Fan13或`.59`。**

完整平台哈希与模式验收见[DVFS交付记录](DVFS_REVIEWED_20261114.md)和OUT的
`tgd3d-current-baseline.json`。最新23:46:28 UTC启动、DWM824，23:48:47确认原生启用完成。
22:51交互只读核对当前及保存方向均720×1280/orientation0/60Hz，DWM实际加载上述UMD/ICD。
22:53已核对上一轮四个临时任务及工具/上传包清理完成，23:26默认启用任务Ready。SSH为
`ruje@192.168.8.6`；没有保持现场限制。用户取消五分钟交互，也不再做离屏检查。
下文早期“未合入”及恢复配对均是历史状态，不能覆盖此节。

23:36已删除桌面旧`Switch硬件监控.exe`，WindowsDrivers中的`app/SwitchTelemetry`
源码也已移除。23:50:06在确认开机启动后，删除Pacecar及Switch Power桌面快捷方式。
EMC交付的12项哈希/长度已核对，控制/传输核心已加入WindowsDrivers工作树并通过ASan/UBSan；
运行期BPMP/ACTMON/电压投票仍待接入，未发布或安装EMC动态调频驱动，RAM显示仍取启动交接值。
NVK主表面写入提交候选曾构建并通过CPU/API控制，但未启用；实机仍用上述显示组合。
01:55审计确认该实验缺少跨设备句柄有效性保证，另有批次数量/清理问题；已按原冻结SHA核对并
归档撤下其12文件未提交修改，记录在`gemini-full-audit-20261115/coordinator-patches/withdrawal.json`。
不得将其旧UMD/ICD产物混入本轮EMC部署。GPU桥接同步I/O的无限等待等审计发现仍需进一步处理。
23:45已清理该未启用候选及控制工具的实机临时目录，本地构建/结果保留。

Switch Power启动版73b9b759…及Pacecar38eb8017…已经实际重启验证。用户随后报告切换时
弹出硬件详情；源码确认切换成功不弹窗，但托盘双击绑定了ShowDetailsDialog。已移除该
隐式入口，详情仅在明确菜单项打开，失败提示保留。UI修正版df3e9299…已编译；23:51:53
发出`SwitchPowerUiUpdate`交互更新任务，只换托盘程序，不重启、不改用户当前电源模式。
随后SSH暂时失联；恢复后确认设备在23:52:32 UTC再次启动，这次没有由本窗口发出重启，
原因未定。23:55读回Code0、Handheld、CPU DFLL active、GpuPolicyMode1、CPU错误0，
修正版托盘df3e9299…（PID5016）及Pacecar38eb8017…（PID4092）均已自动启动于session1。
更新任务结果0，但详细回执是179字节全零，已按SHA256取回，不能据此标记回调测试通过；
停止扩大实机测试，不重复安装或重启。实机临时任务/安装目录按既定流程清理，备份保留。
WindowsDrivers本地提交962e253，
私人Pacecar本地提交69d4e43，尚未推送；EMC完成后按用户要求统一交付到对应仓库。

22:44轮直接验证当前整合生产配对270→90→180，沿用已有屏上图案、扫描面和逻辑抓屏，
没有安装驱动、计划重启或新增ETW/转储。270°短时逻辑像素及扫描面检查通过，90°图案/
抓屏阶段再次非计划重启，180°未执行，整轮失败；此配对的横屏与物理面板尚未验收。
恢复保留同一整合配对、双向Fence及固定频率，没有追加系统重启。结果详见文末。
此前CPU同步对照同时移除了桥接并增加CPU等待，只能缩小并发/完成/生命周期排查范围，
不能单独证明双向Fence死锁。正常路径静态核对尚未找到可确认的环形等待；不据此宣称
同步实现已经正确，也不为制造补丁而修改未证实的路径。

## 先前旋转轮次（历史，不作为当前恢复指令）

**最新：b1a77251空区域修正完成构建、Windows ARM64 CPU控制及一次首测270°实机试验。
模式切换和三次扫描面四色检查成功，随后图案/抓屏阶段再次非计划重启，整轮失败，
90°和180°未执行。18:58:50 UTC已无追加重启地恢复原I配对`.158 / Code0`，DWM828加载
原UMD/ICD；18:59:33只读确认当前与保存方向均720×1280/orientation0/60Hz。
19:02:56确认本轮临时任务、文件和ETW会话已清理。空区域修正不足以解决此次重启，
不将本轮候选作为稳定基线；详见文末。**

**上一轮：2f6e35c7修正Draw错误回报后直接首测270°，该方向模式调用成功，三次扫描面
快照的地址/可见性/四色均正确；随后在图案/抓屏阶段非计划重启，整轮失败。
90°、180°未执行，本组合没有实屏或五分钟验收。18:18:50 UTC已恢复原I配对`.158 / Code0`，
没有追加重启；DWM768加载原UMD/ICD，当前和保存方向均720×1280，频率/风扇保持。
18:25:11清理本轮3个剩余任务及15项文件目录，默认启用任务Ready。详见文末。**

上一轮35eb1c79补上SetDisplayMode真实GPU完成等待，并完成一次直接90→180→270测试。
90°和180°扫描面检查通过，应用270°时再次发生非计划重启，整轮失败；新增等待确实
执行并完成，但不足以解决重启。17:46:05 UTC已恢复原I配对`.158 / Code0`，无追加重启。
DWM700加载0cd34833…/dee3d580…，当前/保存方向720×1280，频率/风扇保持。
17:49:24清理本轮3个剩余任务和15项文件/目录，默认启用任务Ready；详见文末。

上一轮e276a32d模式方向修正已完成直接旋转测试：90°和180°显示源/WIN_ENABLE恢复，
新扫描地址的四色采样与方向预期一致；进入270°图案阶段后发生非计划重启，整轮失败。
没有物理方向验收或五分钟结果。16:59:40 UTC已恢复原I配对`.158 / Code0`及原频率，
恢复利用重启后的关闭门控状态完成，没有追加重启。当前/保存方向720×1280，DWM800
加载0cd34833…/dee3d580…，默认启用任务Ready。17:03:53确认本轮临时工具/任务已清理。

以下为de3f5b25平滑旋转候选的历史结果，完成直接横屏测试仍未上屏。SetTimings实际调用并
成功更新1→2；CommitVidPn/UpdateActive本轮没有调用。DWM随后反复隐藏显示源。
16:27:12 UTC已恢复1a86a02b…/0cd34833…/dee3d580…及`.158 / Code0`；当前/保存方向
均720×1280，双向Fence、频率与登录后启用保持，16:31:03确认本轮临时任务/工具清理。
当时正在修正主表面的显示模式Rotation与扫描存储Rotation混用。

以下为MPO有效掩码修正e28f5886的历史结果，已推送并完成一次正常UMD横屏测试，仍失败。
14:49轮的source_visible/WIN_ENABLE仍为0，实际扫描面12点为0；逻辑图案5020点正确
不能替代上屏。该轮没有自动重启，14:49:50恢复竖屏；14:54:21发出计划恢复重启，
14:58:14已恢复原缓存组合1a86a02b…/0cd34833…/dee3d580…、Code0及原频率。
15:21:33交互只读确认当前/保存方向720×1280、DWM828加载原缓存UMD/ICD。
当时按用户新要求实现真正的GDI Rotate Blt及横屏shadow；后续结果见文末。

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

### WDDM2.6平滑旋转与模式入口核对（候选构建完成，尚未安装）

用户确认继续WDDM2.6，要求核对CommitVidPn、SmoothRotation/MPO声明及
UpdateActiveVidPnPresentPath。实际构建宏和QueryAdapterInfo均为2.6；使用
WDK10.0.26100.0不等于声明3.1。远端Claude尚未恢复，本轮没有调用它。

静态审查确认：已有四方向RotationSupport，但SupportSmoothRotation为0，
UpdateActiveVidPnPresentPath明确拒绝任何不同角度。微软
[Optimized Screen Rotation](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/optimized-screen-rotation-support)
要求支持旋转的WDDM1.2及以上驱动支持这条更新路径；这是可确认的契约缺口，
并非已证明上一轮201次SetDisplayMode重试的唯一根因。

候选只补齐这一处行为：显示适配器声明SupportSmoothRotation；合法的四方向
UpdateActive仅更新后续Rotate Blt使用的PathRotation，不改旧扫描地址、pitch、
时序、SourceVisible、TargetActive、Pending/Displayed或完成值。CPU控制逐字节
核对Panel状态只有角度变化，并覆盖无效角度保持原状态。

SupportMultiPlaneOverlay/MaxOverlayPlanes仍为0。当前只支持完整主平面，
没有额外图层混合/缩放/YUV实现；MPO3承载主平面不等于多层overlay能力。
本轮不虚报该能力，也不改MPO准入或伪造完成通知。

为直接回答实际调用问题，在现有KMD ETW provider新增event4/version1：记录
CommitVidPn、SetTimingsFromVidPn、UpdateActive和SourceVisibility的入口/返回、
NTSTATUS、LUID、请求/前后角度和显隐状态。入口STATUS_PENDING不是成功；
Commit/SetTimings只有VidPN句柄时RequestedRotation记0，不能当作identity。
不增加磁盘/转储/GPU工作；仅本次显式启用ETW会话收集，结束停止并清理。

本地解析器6项检查通过，冻结基线中只移植event4后对应4项检查通过；没有携入
HEAD较新的reset-packet/执行诊断。ARM64构建通过，Switch原I基线Code0下运行
test-context和test-panel-timings均退出0，包含能力位、四方向路径更新和完整入口/
返回配对。这仍是CPU控制，尚无本候选GPU/物理结果。UMD d28d5669…、ICD
dee3d580…保持上一轮冻结版本，不重编译无关组件。

### 平滑旋转实机结果与恢复（2026-11-14 16:19–16:31 UTC）

候选源码de3f5b25537112ef0b66e90f98c0ff9201197b34，SYS
`33bcb3d7faa09e622b8f4a1db007864b85abde35be00cdcd258808f91203c1b6`，UMD
`d28d5669ca284f1b2dcd7541bccc05c2ca5d409f6cf6283884dadade110999c4`，ICD
`dee3d5805af80696f87d094ec1c5f557d662f435336207992baf588e2dc1cee0`。
16:19:39原生启用完成，Code0，DWM816，LUID`00000000000c65e1`，外接电源Online。

模式跟踪完整返回456条入口/出口记录：CommitVidPn=0次、UpdateActive=0次；
SetTimings=2次，其中16:20:25.6800715–16:20:25.6800951的实际旋转调用返回0，
PathRotation从1变2。SourceVisibility共226次，其中DWM线程224次重复Visible=FALSE，
所有返回成功。这排除了本轮“Commit/Update回调卡住”的说法，不能排除其余契约问题。

90°阶段的软件扫描检查失败：source_visible=0、target_active=1、WIN_ENABLE=0，
地址仍`0xfc2bd000`、PresentId36，无pending；像素还是旧桌面。DWM随后进入反复主表面
建立/隐藏循环。未继续180/270，没有横屏物理验收。16:21:02恢复竖屏，16:23:31请求
一次计划恢复重启；16:27:12确认原I缓存组合恢复，DWM812加载原UMD/ICD，频率、风扇13、
默认启用任务及当前/保存720×1280均核对通过。16:30:18删除3任务19项，16:31:03清理
辅助脚本并核对无本轮残留。未恢复`.59`，未追加超时重启。

本地保留OUT/smooth-rotation的53项取回结果，15067314字节且逐项哈希/长度一致；
ETL SHA256 `ec1be44bddfa0cea9cc82683bd3bd7b663be4d681010b3d5b7c6f91236960975`，
导出12991记录、456模式记录，UMD实时匹配12533记录且loss/invalid均0。成功回调不是
成功上屏，日志结束完整也不代表渲染正确。

### 主表面模式方向与扫描存储方向分离（本地修正，实机待验证）

WDK10.0.26100.0把DescribeAllocation.Rotation注明为“Rotation for display mode”；
[公开结构页](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/ns-d3dkmddi-_dxgkarg_describeallocation)
仍标reserved，不能据此忽略真实调用。固定物理720×1280/storage identity不意味着
所处显示模式也是identity；[NONPREROTATED](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/dxgiddi/ns-dxgiddi-dxgi_ddi_primary_desc)
允许应用预先调整输出方向。

再次只读核对系统文件：dxgkrnl.sys10.0.22621.2792，SHA256
`99d425e1c997eae32909fa4f37d6c8e2e846a5374c602295546c8be2547aa5e6`；
d3d11.dll10.0.22621.2715，SHA256
`ccc352f7097d0320368000dbe9ff92772d8fc909185d95c8a0753cb8f115f880`。
与11月11日本地分析文件逐字节一致。DXGDEVICE::SetDisplayMode在RVA1494c8调用
DescribeAllocation，1494ec/269ab4比较非UINT_MAX的返回Rotation与显示模式；
不等时269b04取269ff0的`STATUS_GRAPHICS_PRESENT_MODE_CHANGED=0xc01e0005`。
此前SetDisplayModeCB摘录实际属于**d3d11.dll**中的NDXGI，不能按dxgi.dll地址解释。
其RVA54a50识别该状态、54b20保存线程结果，54b3c在特定分支向UMD返回S_OK；故
UMD的228次S_OK不能单独证明内核模式校验通过。此为静态分支证据，尚非本轮实际
命中该分支的调用栈证明。

修正把48字节私有描述末尾原reserved DWORD用于ModeRotation：tgd3d保留输入的
Primary.ModeDesc.Rotation，标准primary记录创建时的路径方向；DescribeAllocation
返回这份不可变模式元数据。Rotation/Width/Height/Pitch仍描述真实扫描存储，
不改变GPU旋转复制、MPO准入、地址、屏障或完成通知。ModeRotation=0兼容旧描述；
旧KMD会拒绝新字段非零，需配对部署本轮UMD/KMD，现有ICD不变。CPU控制覆盖四个
模式方向对应同一720×1280存储、旧对象元数据不随新创建请求变化，以及非法方向拒绝。
这是一处可检验修正，不把推测写成横屏已修复。

ARM64 KMD、ARM64/x64 UMD均构建通过；Wine x64控制和Switch原I基线上的
test-context、test-panel-timings、tgd3d-contracts均退出0。全部为CPU契约/模型检查，
没有新增GPU离屏测试。候选尚未启用，下一步签名打包后直接测旋转。

### e276a32d实机：90/180扫描面正确，270阶段重启（整轮失败）

配对修正`e276a32dde2cf176f985d98afc6f3d98707dfa5c`已推送。SYS SHA256
`7b745085de256e32d7f1b62e9168a4174729f6113b6312a78d5667368bf441b7`，UMD
`8c9998ca58272ec3815078d8bc39aa461bf23842c52f19f87c359a18ee937153`，ICD仍为dee3d580…。
13项原生CAT校验和71项包成员校验通过，GPU460800/RAM1600000/CPU1497600kHz、风扇13、
登录后启用、双向GPU Fence保持。首次暂存脚本错误地对运行中的Code0调用仅适用于
关闭门控状态的Check-Baseline；只影响暂存校验，未安装驱动，随后按运行中基线重核通过。

16:45:00请求一次更换KMD的计划重启，16:49:06确认旧I内核Code43/默认启用Disabled，
尚无安装记录；SSH超时没有触发重复安装或重启。16:50:35新版启用Code0，DWM836加载
新UMD，LUID`00000000000d7542`。16:51:14直接启动90/180/270序列，未重复竖屏暖机。

- 90°：16:51:26切换，16:51:33扫描面720×1280/pitch2880，source_visible/target_active/
  WIN_ENABLE均为1，锁存与displayed地址均`0xfbb91000`；四点分别绿/黄/红/蓝，容差0、
  差异0。30秒逻辑图案5020个RGB点也全部符合预期。
- 180°：16:52:01切换；16:52:10四点黄/蓝/绿/红全部正确，显示源和扫描布局检查通过，
  30秒逻辑图案5020点差异0。
- 270°：16:52:37切换API返回0，最后保存状态为RotatedPattern，未得到该阶段扫描检查
  或完整结果。启动时间从16:45:54变为16:52:56，确认非计划整机重启；停止扩大测试。

本轮DWM保留138次Flip呈现返回成功，只有11次SetDisplayMode、没有TGD3D_TRACE_BLT
记录；90/180的结果是已旋转主表面的真实扫描采样，不是shadow→旋转Blt路径的实机证明。
WDDM标准shadow及GDI Rotate复制仍是保留的回退实现。SetTimings保存了1→2、2→3成功
返回，显隐调用5次，前一轮224次DWM重复隐藏的现象未再出现。270相关尾部没有完整留存，
不能把最后一条已落盘调用当作触发重启的GPU指令。

ETL SHA256 `b6ddcd267103ddda7ad212618122fae3c0caf0e62a44c793dca0c833aaa4f34c`，
导出4630记录、16条模式记录；UMD实时文件4608行，最后TraceState为matched4586，
没有TraceEnd，仅最后保留状态的lost/invalid为0，不能宣称尾部完整。4条887b0001来自
staging Map的DONOTWAIT未完成返回，不应计为device lost或直接当作重启原因。
73项结果7432342字节已逐项验证：其中umd-live.jsonl的目录枚举Length缓存为269，
文件实际826012字节且SHA256一直相同；用同一FileStream的Length/摘要重新核对，
修正清单计量，未丢弃或裁剪日志。

16:54:59确认重启后候选Code43、默认启用Disabled；16:58:37调用原恢复工具，利用
关闭门控状态直接换回原I内核/UMD。16:59:40完成`.158 / Code0`恢复，无额外重启；
DWM800使用原缓存UMD，频率/风扇符合基线，交互只读查询确认当前及保存方向720×1280。
完整本轮材料在OUT/primary-mode-rotation。物理面板方向、触摸对应及五分钟横屏仍未验收；
下一步审查真实Flip链路中的连续模式切换和旧主表面退休，不能先归咎于未证实调用的
shadow旋转命令，更不重装同一失败组合代替分析。

17:03:37清理3个任务、21项本轮目录/上传文件，17:03:53删除清理脚本并核对本轮前缀
无残留；默认任务Ready、原I目录及Code0保持。12项恢复记录14967字节已取回并核验。
Gemini SC7仍在原Antigravity Carbon High会话独立开发，尚无SC7报告/diff；DVFS/Pacecar
交接包已接收，但未混入本轮显示修正或部署。

### SetDisplayMode的真实完成边界（2026-11-14，实机结果见下节）

对e276a32d的实际Flip路径继续审查，发现tgd3d的SetDisplayMode只调用`flush(dev, true)`。
双向Fence保证两个提交上下文之间的顺序，但该函数只在超过两笔在途提交时限流，
不能保证最新提交已经完成。`pfnSetDisplayModeCb`同步选择扫描allocation，没有传入
渲染hContext，不能当作普通Present自动排在该上下文的GPU wait之后。旧Zink桌面前端
`DxgiFns.cpp::_SetDisplayMode`在`RuntimePrimarySetMode`之前调用`RuntimeResourceFinish`。

微软[SetDisplayModeCb文档](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dumddi/nc-d3dumddi-pfnd3dddi_setdisplaymodecb)
（页面更新2021-04-01，本轮查阅2026-11-14）说明此回调设置实际扫描的allocation；
其中R200示例不能直接证明NVK独立提交上下文已完成。另核对实机Windows 22621组件和
微软公开符号，以下RVA/调用结论仅适用于这些精确二进制，不外推到其他Windows版本：

- dxgkrnl.sys 10.0.22621.2792，SHA256
  `99d425e1c997eae32909fa4f37d6c8e2e846a5374c602295546c8be2547aa5e6`。
  UpdateModeWithPrimary（RVA ef348）调用FlushScheduler(reason8)；fe618读取调度接口
  的0x308槽位，再以type1调用该函数。
- dxgmms2.sys 10.0.22621.2792，SHA256
  `94fa72e5cee778b7cd12bedf13550a78414231df8612547c74f0bc3ed11a62c5`，公开PDB
  AF63A8A49D01FA371A266959E133EB0B/age1。该槽位为VidSchFlushQueuePackets（RVA18730），
  下游type1撤销待执行的MMIO Flip并清理FlipDevice；不是独立的VidSchFlushAdapter。
  不能把这里的Flush理解为全部NVK渲染完成。
- d3d11.dll 10.0.22621.2715，SHA256
  `ccc352f7097d0320368000dbe9ff92772d8fc909185d95c8a0753cb8f115f880`。
  RotateResourceIdentities（RVA19210）由runtime交换内部内核身份，随后通知UMD；
  现有storage交换和runtime_rebind未发现需改动的句柄对应错误。

本次只改SetDisplayMode：先正常提交并保留双向桥接，再等待`submitted`的真实Vulkan
timeline完成，成功后调用runtime。等待失败则返回设备丢失，不发布主表面，也不由CPU
伪造完成。新增等待原因6用于区分模式切换与普通Present回压。KMD、ICD、日常Present
和GPU旋转着色器均不变；此次发现不是270°阶段重启根因已被证明。

控制测试调用真实DXGI DDI表及runtime owner实现，覆盖一笔未完成提交、已完成免等待、
超时和设备丢失。新测试配旧SetDisplayMode对象时在`modeCompleted == 1`断言失败；
补丁后的x64 Wine控制全部通过，ARM64/x64 DLL与控制工具构建完成。仅为CPU控制，
不代表GPU、实屏或稳定性通过。冻结候选只移植这处修正，未混入HEAD其他query修改。
ARM64 UMD SHA256 `22c4be6184deb402779412bdccc8a6d645538b9b2c5c79227dd17cedd3d005bd`；
复用e276a32d的7b745085…签名KMD和dee3d580…ICD，维持WDDM2.6。
材料位于OUT/mode-change-completion，二进制审查材料在OUT/flip-retirement-audit；
微软二进制/PDB仅本地分析，不纳入仓库。

### 35eb1c79实机：等待生效，270阶段仍重启（整轮失败）

提交`35eb1c791eb3d68686c0f8e302bcea96b6eb3d94`已推送。新UMD为22c4be61…，
KMD/ICD仍为上一轮7b745085…/dee3d580…；冻结源码差异只有SetDisplayMode、等待原因枚举
和对应CPU控制。Windows原生CPU控制通过，71项包内容及控制脚本核对通过。
安装目录`C:\ProgramData\SwitchWOA\GpuUpdate\tgd3d-35eb1c79-20261115-013732`。

17:33:44 UTC请求一次计划门控重启，17:34:28启动；17:38:33原生启用完成，DWM792
加载新UMD，原生LUID`00000000000d02af`，Windows10.0.22631/UBR2861，电源Online。
双向Fence开启，CPU1497600/GPU460800/RAM1600000kHz、风扇13及登录后启用保持。
没有做竖屏暖机或离屏GPU测试，直接进入方向切换：

- 90°于17:39:08应用；30秒图案完成。扫描地址/已显示地址均`0xfb17f000`，
  source_visible/target_active/WIN_ENABLE为1，720×1280/pitch2880、偏移0。
  四点绿/黄/红/蓝均正确；逻辑图案5020个RGB点、容差0、差异0。
- 180°于17:39:43应用；30秒图案完成。扫描地址/已显示地址均`0xfb503000`，
  激活/几何检查通过，四点黄/蓝/绿/红正确；逻辑图案5020点差异0。
- 下一阶段保存为ApplyingRotation/270，没有保留该次API返回或图案就绪结果。
  SSH断开，恢复连接后启动时间变为17:40:42.0416540 UTC、Code43，确认非计划重启。
  立即停止扩大测试，没有五分钟或物理方向验收，不重装同一候选。

新增模式等待实际执行三次：sequence39/40/82分别等待6.054/17.175/20.245ms，
VkResult均成功、completed等于目标序号。这证明补丁运行并取得真实GPU完成值，
不证明重启根因已修复。保留的DWM记录包含120次成功Flip Present、9次SetDisplayMode、
120次RotateResourceIdentities、0次Blt；仍不能把shadow旋转复制当成本轮实际路径。
KMD16条模式事件含SetTimings的1→2、2→3成功更新。未保留270°模式回调或等待，
不能据此断言系统没调用它，也不能把最后一笔保留的提交认作致故障指令。

UMD实时材料4144行，最后TraceState为matched4122、invalid/lost均0，但没有TraceEnd，
尾部不完整。7条887b0001均来自冻结resource.cpp:994的DONOTWAIT staging Map待完成，
不能按设备丢失统计。ETL SHA256
`8e48c445fb02adbeddf0b4b8a4087d4ca543e37d0cb361acf1b69269e83c3317`；4166条原始导出
SHA256 `dc7408c52ae00b4489fa11fa077b0ce8c3fcac5b58a1a890752da64677610c0b`。
70文件6355663字节已取回并校验，长度/摘要来自同一FileStream，避免前轮目录缓存误判。

17:45:01请求直接恢复，利用已经重启后的Code43门控，没有再发系统重启；17:46:05
完成原I内核1a86a02b…/UMD0cd34833…/ICDdee3d580…恢复。DWM700加载原组件，Code0，
频率/风扇保持。17:47:01交互只读核对当前和保存方向720×1280/orientation0/60Hz。
13项恢复/控制记录20737字节已取回校验。17:49:24清理本轮剩余3个任务与15项文件目录；
旋转任务和方向恢复任务此前已停止/删除，原默认启用任务Ready。17:49:38删除清理助手，
核对本轮前缀文件与任务均无残留；共享签名工具和原I包保留。

下一步继续静态审查实际Flip路径在连续模式切换中的主表面、视图与在途资源退休。
两轮都在第三次方向切换附近失败，仍不能区分270°专属问题和连续切换累积问题。
保留已确认的完成边界修正，不把它包装成稳定性修复，也不扩大到同时修改全部组件。

### Flip退休审查与Draw错误契约（2026-11-14，实机结果见下节）

在35eb1c79失败后继续静态审查实际Flip链路。ImageStorage.lastUse随身份交换移动，
DestroyResource正常路径等待该序号；storage_destroy清理其他逻辑资源缓存的同一image视图，
并将视图排在image/memory之前退休。shader与sampler同样按lastSeq延迟释放。未发现这些
正常路径中可以直接判定为提前释放的代码，不因此增加每帧CPU等待或删除双向桥接。

KMD扫描段BaseAddress就是其系统物理基址，显示DDI地址经边界检查后仍为同一物理地址，
VBlank报告实际锁存地址；没有发现这里重复加段基址。对照微软
[中断数据](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/ns-d3dkmddi-_dxgkargcb_notify_interrupt_data)
及[SetVidPnSourceAddress参数](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/ns-d3dkmddi-_dxgkarg_setvidpnsourceaddress)
核对，显示隐藏时仍保留非零扫描地址。此结论仅覆盖当前固定pitch扫描段，不能外推成
整个分页/缓存链已被证明无误；本候选不改KMD或ICD。

确认另一处DDI契约缺陷：descriptor_set_alloc将所有失败改报E_OUTOFMEMORY，且把系统OOM
也当作池耗尽重试；Draw中的视图创建、懒编译也会直接调用通用SetErrorCb。微软
[Draw](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/nc-d3d10umddi-pfnd3d10ddi_draw)、
[DrawIndexedInstanced](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/nc-d3d10umddi-pfnd3d10ddi_drawindexedinstanced)
（2024-02-22）及[DrawAuto](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/nc-d3d10umddi-pfnd3d10ddi_drawauto)
（2021-04-01）规定这些DDI只应回报D3DDDIERR_DEVICEREMOVED，其他错误将被runtime视作严重错误。
Vulkan[描述符分配规则](https://docs.vulkan.org/refpages/latest/refpages/source/vkAllocateDescriptorSets.html)
区分池容量不足与主机/设备内存不足；上述文档均于2026-11-14查阅。

最小修正：五个Draw入口设置局部错误作用域，离开时恢复；本驱动未声明free-threading。
Device::setError先记录原始HRESULT，再在Draw作用域将失败转换为设备移除并停止后续工作。
非Draw的资源创建保留原OOM等错误。描述符分配仅在OUT_OF_POOL_MEMORY/FRAGMENTED_POOL时
尝试另一池，其他错误直接传播，失败的第二次分配不再额外创建未使用的池。
DrawAuto仍未实现流输出，不会被标成支持；其错误也遵循Draw契约。

通过真实DDI表注入pool创建、set分配和image view创建的主机OOM、设备OOM、设备丢失，
覆盖四种普通Draw入口，确认只回报移除、没有发出绘制，重复Draw不再分配。
另验证DrawAuto、无操作Draw后非Draw OOM语义，以及池耗尽/碎片化后的成功重试。
新控制配旧对象时在“仅回报设备移除”断言失败；修正后的x64 Wine控制通过，ARM64/x64
构建完成。ARM64 UMD SHA256 `7e3d5b254e68333b81790e9acf29d19409c0540dcde4fb7ed044eea046c38ca0`。
这是CPU控制和明确的契约修复，尚无证据证明前轮270°重启触发了此错误分支。
下一轮只携带该UMD差异，直接按270→90→180测试，以区分方向特有问题与连续切换累积问题。
材料位于OUT/draw-error-contract；原I恢复基线、频率、双向Fence、登录后启用均保持。

### 2f6e35c7实机：首测270°短时扫描正确，随后重启（整轮失败）

`2f6e35c7e997594361a84a87ac2706815ebb5bd9`已推送，ARM64 UMD为7e3d5b25…；
KMD仍为7b745085…、ICD仍为dee3d580…。冻结源码仅修改Draw错误作用域、描述符失败
分类及对应控制，Windows原生CPU控制通过；不混入其他query/电源/SC7工作。
安装目录`C:\ProgramData\SwitchWOA\GpuUpdate\tgd3d-2f6e35c7-20261115-020823`。
测试改为270→90→180，并在每个方向完成后检查DWM/Explorer身份，重建即停止后续方向。

18:04:57 UTC请求一次计划门控重启；18:05:37.4157210启动，18:09:24原生启用完成。
DWM824加载新UMD，LUID`00000000000c8955`，Windows10.0.22631/UBR2861、电源Online，
双向Fence开启，CPU1497600/GPU460800/RAM1600000kHz及风扇13保持。
18:10:08请求唯一一次测试，18:10:20的270°模式调用返回0：逻辑1280×720/orientation3，
path_rotation4，保存的设置仍是竖屏。图案窗口ready文件记录session1、1280×720。

18:10:25.341、26.529、27.638的三次原始扫描快照均flags7，source_visible/target_active/
WIN_ENABLE为1，物理720×1280/pitch2880、offset0；地址和已显示地址均`0xfc2ac000`，
DisplayedPresentId70。按预先定义的270°期望比较，四点蓝/红/黄/绿连续三次均正确，
共12次RGB比较、容差0、差异0。underflow读数2与切换前相同，不能据此认定新增下溢。
这证明短时扫描backing/锁存状态，不能替代物理面板和持续交互验收。

抓屏助手18:10:28.8574063保存了geometry检查点，范围1280×720、Default桌面、Code0；
源码中该检查点位于Graphics.CopyFromScreen之前。没有完成PNG或capture结果，图案
控制state文件484字节全部为0，不能读取为有效状态。geometry里的Exit1是初始化占位，
不能当作工具已返回错误。重连后启动时间为18:10:49.0462470、Code43，确认非计划重启；
30秒图案未完成，90°/180°没有执行，不扩大测试，不重装同一组合。

保留的UMD记录3681行，DWM有70次成功Flip Present、5次SetDisplayMode、70次身份轮换，
未见Blt。序号41的模式等待12.305ms，completed41、成功。10条KMD显示事件记录
SetTimings1→4成功及随后source_visibility恢复为true。未保留新的Draw OOM/移除分支，
不能声称该修正已处理本次重启；5条887b0001仍为DONOTWAIT staging Map待完成。
UMD事件最多到约18:10:23.248，消费者状态到25.061；没有TraceEnd，尾部不完整。
未保留DWM读回提交，也不能由此排除28.857之后的读回/复制或定位最后故障指令。

ETL SHA256 `1d2d8866d668f3e0cf61b62a0db586aea0852f17db4aecacf3c1ac88b3ec2a07`，
3676条原始导出SHA256 `00aaf76f4d6c2dc332012f98eff746ab842e727a8ddc2cc0f8a77f1fdb5c8dd7`。
45文件4834971字节已取回并验证。重启后Short/Restore任务不在调度器，但保存状态和
快照证明实际执行过；修正收集/恢复助手的“任务必须存在”假设，只将存在且Running的
任务视作仍在途，不重建或重跑失败测试。相关助手diff/hash保留在本轮目录。
另准备next-test-tools中的下一轮模板，改用现有Common.Save-Json的WriteThrough及
Flush(true)保存图案状态/结果；保留本轮实际使用的原脚本，尚未部署新模板。

18:17:37请求无重启恢复，18:18:50完成原I的1a86a02b…/0cd34833…/dee3d580…配对，
Code0、DWM768、CPU/GPU/RAM/风扇核对通过。18:21:06交互只读确认当前和保存方向均
720×1280/orientation0/60Hz。13项恢复记录21009字节已取回验证；18:25:11清理3个
剩余任务和15项本轮文件目录，原默认启用任务Ready。
18:25:49删除清理助手并确认本轮前缀任务/文件无残留，保留共享签名工具及原I包。
OUT/tgd3d-current-baseline.json原先仍记录08:22的B组合；已依据本轮恢复/模块/显示核验
更新为实际I配对，将旧用户选择保留为历史，避免未来恢复误用旧包。

本轮改变排查边界：270°放在第一次切换仍会失败，且已短时产生正确扫描面，不能仅以
“第三次切换累积”或“270°主表面完全没绑定成功”解释。下一步审查读回/复制、抓屏涉及
的GDI/KMD路径，以及其提交完成与资源生命周期；抓屏未完成只是关联点，尚不能认定
工具是根因。稳定性、物理方向和五分钟交互仍未通过。

### 空区域复制/更新修正：本地构建和控制通过，未部署

静态审查发现`copy_region`先将有符号端点之差转为`VkExtent3D`的无符号宽高深，
只检查差值是否为零。倒置区间会成为巨大复制范围，能够进入GPU命令或CPU staging
拷贝；`ResourceUpdateSubresourceUP`也没有在上传/等待/重命名前统一过滤空区域。
[微软ResourceCopyRegion DDI](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/nc-d3d10umddi-pfnd3d10ddi_resourcecopyregion)
明确规定任一轴起点大于或等于终点即为空，不执行复制；
[UpdateSubresource](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-updatesubresource)
也规定空区域不更新。按该契约在两个入口先比较端点，NULL仍表示完整子资源。
本次只改此问题，不扩大为通用边界校验或同时修改KMD/ICD。

新增控制经真实DDI表覆盖三轴等于/倒置及全零共7种box，在image→image、image→staging、
staging→image、staging→staging、buffer→buffer路径分别执行复制和更新。已有未完成工作
不应触发额外命令、等待、提交、错误、重命名或内存修改；NULL的65×17×2完整复制仍保留
偏移16、row pitch260和depth pitch4420，并核对CPU目标区外字节未变。
新控制链接上一轮对象，立即在“空区域不得提交复制”断言失败；修正对象的x64 Wine
CPU控制通过。ARM64和x64均编译完成，ARM64控制尚未在Windows执行，无GPU/实屏结果。

冻结源码沿用OUT/draw-error-contract/source，仅带入resource.cpp和contracts.cpp的本次
差异，双向GPU Fence、着色器缓存开启。完整构建输入、对象哈希和结果在OUT/empty-copy-box：
ARM64 UMD SHA256 `73bbeb98a98e2cd284198f9a71806867952cba9d55dfcca79548c2594519ff67`，
x64 UMD SHA256 `4dc367d015f4b2d0518790008c2293a364c61857ae9fe0f47589d897a830030c`。
尚未观察到实机传入这类box，不能认定它解释了270°重启，也未安装新候选。

读回和shadow审查的当前边界：标准shadow可为逻辑1280×720，primary固定720×1280；
KMD的GDI Rotate Blt执行真实GPU搬运，tgd3d的DXGI rotated_blit则使用Vulkan着色器绘制。
最近保留的DWM记录是Flip且无UMD Blt，不能声称每帧都走这两条旋转复制路径。
CPU Map等待sliceLastWrite；NVK已有执行/数据缓存屏障。KMD在GPU的WFI→Reference标记
完成后调用`t214_frontend.c`的sync_memory，执行FB flush及L2写回/失效并检查完成，
然后才置GpuCompleted，不能仅因Windows的NVK屏障使用WFI就断言缺少DRAM写回。
这些是代码路径核对，尚未证明故障轮具体读回提交的顺序、范围和退休正确；继续沿实际
读回及GDI调用链查明后再选下一轮实机候选，不修改测试来回避既有失败。

### b1a77251实机：空区域修正未解决首测270°重启

本轮从18:45:31核验的原I/Code0、DWM768、外接电源状态开始；CPU1497600kHz/1120mV、
GPU460800kHz、RAM1600000kHz和风扇13保持。与上一失败候选相比只修改空区域复制/更新，
KMD仍7b745085…、ICD仍dee3d580…，UMD为73bbeb98…；双向Fence、缓存均开启。
71项包文件及31份PowerShell解析通过，Windows ARM64 CPU控制通过，无离屏GPU测试。
图案状态保存采用上一轮已准备的WriteThrough/Flush(true)修正，实际图案、抓屏和判断
步骤保持；没有通过删掉抓屏来避开原失败。

18:47:24请求唯一一次计划门控重启，18:48:00.6170110启动。18:50:42启动安装任务，
18:51:52确认原生启用、DWM836加载本轮UMD，任务退出0。18:53:12仅启动一次270→90→180
测试；Windows10.0.22631/UBR2861，原生LUID`00000000000ccc6b`、电源Online。
18:53:25模式API返回0，逻辑1280×720/orientation3，path_rotation4，保存设置仍竖屏。

18:53:34.175、35.425、36.753三次快照均flags7，显示源和目标可见，WIN_ENABLE为1，
地址/已显示地址均`0xfc2bf000`，PresentId96，无pending地址；物理720×1280/pitch2880、
offset0、DC rotation0。四点蓝/红/黄/绿共12次比较、容差0、差异0；underflow7与切换前
相同。图案状态文件485字节为有效JSON，明确记录`PatternVisible`、session1及1280×720；
此前全零状态文件问题本轮未复现。18:53:38.6601019保存抓屏geometry检查点，此处仍在
Bitmap/CopyFromScreen之前；没有完成PNG、capture结果或30秒图案结果。geometry的Exit1
仍是初始化占位，不能作为返回错误。

重连确认启动时间变为18:53:56.4473080、Code43；这是非计划重启。90°和180°未执行，
没有实屏方向或五分钟通过结果，不重跑同一候选。保留4019行UMD记录：DWM96次成功
Flip Present、5次SetDisplayMode、96次身份轮换、未见Blt；模式等待seq47实际完成47，
用时13.666ms。4条887b0001来自DONOTWAIT staging Map（本次源码line1002）；未见新的
Draw移除错误。DWM事件仅保留到约18:53:32.192，末尾其他进程事件到32.546，缺TraceEnd，
不能定位38.660之后的实际读回提交或最后GPU指令，也不能认定抓屏工具就是根因。

45文件5016796字节已取回并逐项核对长度/SHA256。ETL SHA256
`6d25495b903682e683f8f8f714f21ed2f6af4a4a866f107831920fd5a9b6aa89`；原始事件导出保留，
输入ETL未改变。18:57:44请求原I恢复，18:58:50.0566783完成，未追加重启；
KMD1a86a02b…/UMD0cd34833…/ICDdee3d580…、DWM828、Code0和平台频率均核验。
18:59:33交互会话只读确认当前与保存方向720×1280/orientation0/60Hz及原模块哈希。
13项恢复记录21291字节已取回校验；19:02:32清理3个剩余任务和15项本轮文件/目录，
默认启用任务Ready。19:02:56删除清理助手，确认本轮前缀文件、任务和ETW会话无残留。
OUT/tgd3d-current-baseline.json已同步到本次实际恢复状态，仍保留原I作为后续恢复组合。
本轮材料位于OUT/empty-copy-box，包含完整构建、安装、失败、恢复记录和round-result.json。

结论仅为：空区域契约错误已经修正，但不足以解决当前重启。继续审查实际复制/读回与
共享资源退休边界；已有执行等待和缓存写回不等于整条路径已被证明正确。保留本轮失败，
不把短时扫描面正确写成旋转或稳定性验收通过。

并行SC7收件：Antigravity会话03b85b9a已生成报告及mesa/windows-drivers/winx三份diff，
分别与a2250144/b19f617e/a30d33b2相对声明基线的git diff完全一致，工作树无已跟踪改动。
CLI进程仍在，尚未观察到终态。仅确认交付文件匹配，54项测试的声明未由父任务独立验收。
初审已记录报告误称host1x syncpoint（实际是USERD.Reference完成链），以及warmboot/BPMP
配置、显示恢复和XUSB完整重载等未闭合条件；未合入或部署。详情在委派目录的
sc7-received-review-status.json，不能将此交付标记为SC7实机完成。


## 2026-11-14 CPU-sync timing control and recovery (UTC)

Only `TGD3D_CPU_SYNC_CONTROL` differed from the frozen b1a77251 source set.
ARM64 UMD SHA256 `4ad681c06275d4228835ebcaeab264de860a3bbc748a1b03ac231bc8857fe87d`;
KMD remains 7b745085… and ICD dee3d580…. This diagnostic retains the local Vulkan
timeline, omits runtime bridge ingress/egress, and waits for real GPU completion
on ordinary Flush/Present. It is not the production default or a final fix.
CPU controls passed on Wine x64 and native ARM64; no offscreen GPU test was run.

Native boot 19:16:05.4986840, DWM772, LUID `00000000000d1a0f`, Windows22631/2861,
external power Online. The one 270→90→180 sequence ran 19:22:34–19:24:38 and
returned `SoftwareChecksCompleted`, exit0. Each angle completed its capture:
5020 RGB samples, tolerance0, differences0 (15060 total); nine scanout snapshots
retained physical720×1280/pitch2880/DCrotation0 and expected corner colors.
DWM and Explorer PIDs survived the sequence; the 19:27:58 query retained the same
boot and Code0. No physical-panel or five-minute stability acceptance is claimed.

The 4562-row trace includes TraceEnd: matched4521, invalid0, eventsLost0,
buffersLost0, processStatus0. TraceEnd query_status4201 means the session was
already stopped. All 211 Vulkan submit pairs have phase2; no runtime phase1/3
bridge submits occur. DWM has160 successful Flip Presents,160 sync-control waits
and3 readback waits. Actual captures submit/complete local seq106,123,161.
Four pending Map reports are DONOTWAIT, not device removal. ETL SHA256
`66685ce5317f89a80a1e4df65dbce70eb68f70f746402db58cc0982c242f0867`.
This narrows the failure toward asynchronous ordering/concurrency/lifetime;
it does not identify a particular Fence deadlock or exclude GPU timing effects.

93 evidence files/9983368 bytes and14 recovery files/21666 bytes were retrieved
and fully hash-verified. One planned recovery restart booted20:29:37.9173480;
restoration finished20:32:42.1650187, original I KMD1a86a02b…/UMD0cd34833…/ICDdee3d580…,
DWM820, Code0, CPU1497600/GPU460800/RAM1600000, Fan13. The20:35:33 interactive
read-only query verified current/saved portrait720×1280/orientation0/60Hz.
20:44:23 cleanup confirmed no rotcpu files, tasks or ETW session remain. The
production GPU bridge and logon startup are restored. Full records and the
machine-readable result are in OUT/rotation-cpu-sync-control; the current baseline
record is updated. Do not treat the unobserved wall-clock gap as a stability run.


## 22:44 整合生产配对旋转失败及同配对恢复

本轮没有构建/安装新的GPU驱动，也没有关闭双向GPU Fence。测试的是顶部记录的
3ada55a4…/8d7da8c1…/30a8d6a9…整合配对，Fan19、Battery12、handoff v2及扩展ACPI。
UMD的源码哈希集合和保留链接输入此前已与冻结b1a77251核对一致；重新编译路径与
二进制布局使DLL哈希不同。它不是4ad681c0… CPU同步诊断版。
Windows10.0.22631/UBR2861；本轮原生LUID0000000000081941，原启动22:17:19 UTC、
DWM836，外接电源Online。固定7保持CPU1497600/GPU460800/RAM1600000kHz。

* 270°：模式调用返回0，图案完成约30秒；逻辑1280×720，四块16×16及一像素白边
  共5020个RGB像素，容差0、差异0，PNG已查看。三次扫描面快照均720×1280、pitch2880，
  source_visible/target_active/WIN_ENABLE均开启，DC列扫描旋转位关闭；实际地址和
  displayed_address均0xfaf35000，蓝/红/黄/绿四点符合270°预期。
* 90°：模式调用返回0，三次扫描面快照同样几何、可见性正确；实际地址和
  displayed_address均0xfb2b9000，绿/黄/红/蓝四点符合90°预期。
  22:45:48.359 UTC保存了抓屏前的Default桌面/1280×720/图案窗口前台记录，随后没有PNG、
  图案完成结果或最终测试结果。SSH断线后读回新启动22:46:08.317 UTC、DWM772、Code43，
  确认非计划重启。该检查点只能定位到图案/抓屏阶段，不能证明具体某个API已经是根因。
* 180°未执行。整轮失败，不记为横屏、物理方向、触摸或长期稳定性通过；没有五分钟
  交互、离屏检查、ETW或新增转储。

22:48确认重启后停用本轮测试及定时恢复任务，不重测同一组合。22:49只启用一次当前
整合配对，22:50:10恢复Code0和竖屏，没有请求系统重启。22:51交互核对当前与保存方向
均720×1280/orientation0/60Hz，DWM772实际UMD/ICD路径与完整哈希匹配；CPU/GPU/RAM
恢复固定7，Fan19和充电限制85保留。Pacecar1704及SwitchPowerTray4044均session1，
HKCU Run两项及登录后5秒tgd3d任务保持。没有回退旧I、Fan13或`.59`。

结果位于OUT `rotation-integrated-baseline/evidence/`，59个文件、1168093字节的SHA256
和同一打开文件流的长度全部核对通过；ZIP SHA256为
`578cc472008212bf3d60367d09e336f5b9f255e92304d686ede140a942bea7a5`。
原枚举清单把90°图案stdout文件长度记成0，实际流长度114，SHA256本来就一致。
保留原始清单及差异，并用同一FileStream读取长度和计算哈希修正工具记录，没有改写
试验内容。22:53:49只读确认四个临时任务、测试目录、上传ZIP和证据ZIP均已清理。
首次清理命令观察超时后只读核对发现已经执行完成，没有重复删除或启动测试。

### 静态审查结论与下一处契约核对

`tgd3d_cmd.cpp`正常链是runtime signal→Vulkan wait/render/signal→runtime wait，
下一次runtime signal排在上一次runtime wait之后；尚未证明存在环形依赖。
NVK的正常GPU信号走KMT GPU队列，完成值读取在当前Clang构建使用acquire原子读取。
KMD正常硬件完成要求GPU marker、通道退休及CPU可见性确认后才通知DMA完成。
这些只说明检查到的正常路径，不能证明错误/销毁路径或跨设备共享已经正确。

继续审查两个具体边界：一是runtime资源与Vulkan资源各自最后一次使用后的GPU VA释放；
二是`kmt_native.c::tg_kmt_submit`现在没有填`NumPrimaries/WrittenPrimaries`。
微软的[D3DKMT_SUBMITCOMMAND说明（2024-10-22）](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmthk/ns-d3dkmthk-_d3dkmt_submitcommand)
和[SubmitCommand回调契约](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dumddi/nc-d3dumddi-pfnd3dddi_submitcommandcb)
要求GPU VA提交报告写入的primary/swapchain backing，供VidMm协调渲染与Flip。
该接口适用于当前WDDM2.6路径；不能拿WDDM3.x硬件队列替换现有接口。
需要追踪runtime allocation是否属于提交设备、哪些实际写入以及句柄存活期，再做局部
修正；不能把另一个设备的句柄直接塞进数组，也不能把此缺口直接写成此次重启根因。
该NVK提交路径也由Zink共用，源码差异与旧CPU同步对照仍需分别解释。
