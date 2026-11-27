# Switch Windows ARM64 图形稳定性交接

## 当前工作 · Windows ARM64 本机验收

### 首次 Present · KMD 入返和 GPU 围栏同轮取证

诊断 KMD `bfbc849f36b5` 在启动 `94004` 消耗一次性许可，以交流供电、手持模式和 GPU Code 0 进入桌面。其余七个配置哈希与正式基线一致。复用已核验旧式 DDI `0x000b000f` 的签名夹具，普通窗口的两个 BGRA SEQUENTIAL 缓冲均无应用写入，首次 `Present(1,0)` 在 16.888 ms 后返回 S_OK。应用 DMA `4328` 的 52 字节包在同一 QPC 时钟下串起 Windows 提交、KMD 进入和 NTSTATUS 成功、GP_PUT、GPU BAR1 完成回读以及 Windows 中断、完成和退役。画面、输入、正常退出和同一桌面通过。DWM DMA `4321` 的 8336 字节包在同一时间段有独立全链。[同轮实载、原件哈希与两条提交全链](windows-native-graphics/dma-return-calibration-94004.json)

从同一 ETL 复核首次调用后的 DWM 包，节点 0 有 83 个普通非 Present 包，其中两个为历史相同的 3524 字节。均未出现同一队列两次抢占后再次提交。启动 `91476` 的 DWM 队列 `48660` 第三次提交 `152586` 仍缺当次 KMD 入返和完成回执，原三次重启根因也继续开放。独立的应用资源围栏等待仍需同一对象的证据。[DWM 抢占对照](windows-native-graphics/rounds/win-dwm-third-return-control-20261121-01.result.json) · [历史故障边界](windows-native-graphics/rounds/win-board-cdd-dwm-third-submit-kmd-20261121-01.result.json)

启动 `94176` 恢复正式基线 `2c35ae83`、`oem30.inf` 与签名 KMD `69ba174ebe17`。已读回八项全哈希、永久许可、Code 0、DWM 1428 和交流手持状态。四次受控转换逐次匹配 Windows 1074，期间无新增 41、6008、1001 或 4101。诊断包、试用许可、回退任务和采集均已清理。[恢复与清理回执](windows-native-graphics/dma-return-calibration-94004.json)

### 现行基线 · NVK ARM64EC 兼容实现

启动 `93748` 在交流供电下以一次性许可进入 Code 0 桌面。x64 与 ARM64EC 客户端分别通过系统 D3D11 只请求 FL11_1，加载兼容槽中的 ARM64EC NVK `d31b862075fe`，完成三角形像素校验并以 0 退出。原生 ARM64 对照加载 `a143bee5ba42`，三组均回到同一 DWM。选定后已读回永久许可与所有组件哈希，工作目录内已移除旧的纯 x64 NVK。首次 Present 故障继续登记。[绘图与启动验收](nvk-arm64ec-d31b8620/qualification.json) · [永久许可与兼容槽核对](nvk-arm64ec-d31b8620/selection.json)

### NVK 各架构选定基线

| 进程 | 选定的 NVK 实现 | 已验收的入口 |
| --- | --- | --- |
| ARM64 | 原生 ARM64，`a143bee5ba42` | Windows 系统 D3D11 与原生 Vulkan ICD |
| x64、ARM64EC | ARM64EC，`d31b862075fe` | 系统兼容槽 `tg_nvk_x64.dll`，两个 ABI 分别通过绘图与退出 |
| x86 | 32 位 x86，`1064131dcda0` | 应用私有 ICD，GM20B 绘图读回、Code 0、同次启动和正常退出 |

ARM64 与 ARM64EC 共用一份开机许可。x86 选定的是已通过绘图验收的私有实现，系统级 WOW64 Vulkan 注册仍待独立验收。《冰封王座》私有 WSI 包 `5bd669c74530` 已确认能枚举 GM20B，保留为另一份应用专用构建。[64 位双路径验收](nvk-arm64ec-d31b8620/qualification.json) · [x86 实机验收与完整哈希](dxvk-games-20261118/x86-d3d9-private-baseline.json)

### 历史基线 · KMD-only `e1b22087`

Windows 启动 `92696` 在交流与手持模式下一次性加载签名 KMD `69ba174ebe17`，原正式六个用户态组件哈希逐一匹配，ARM64 NVK 为 `51ff97f63d85`。GPU Code 0，交互 Explorer 有响应，同一会话 DWM 已加载预期模块，闭门和一次性启动均有受控关闭回执。正式晋升检查通过后，在同一启动授予永久许可并完成读回。恢复包保留，本轮试用任务和转运临时文件已清理。[独立验收](kmd-only-e1b22087/qualification.json) · [永久许可](kmd-only-e1b22087/permanent-boot.json) · [桌面与系统复核](kmd-only-e1b22087/final-boot.json)

720p 的专项旁证来自联合包启动 92336 的同一签名 KMD，当时使用候选 NVK。当前选定的桌面验收以其自己的启动和实载证据为准。WSI 与直接 DIB 保持候选。历史首次 Present 重启及 91476 的 DMA 152586 仍待定位。[720p 旁证](combined-13-4ab7804d/native720.json) · [首次 Present 与围栏](windows-native-graphics/index.json)

### 13 组联合候选的一次性实机验收

签名 KMD `69ba174ebe17` 和 ARM64 NVK `a143bee5ba42` 随源码 `4ab7804d` 在交流、手持的单次许可启动 `92336` 实载，GPU Code 0。原生 720p 的六次 KMT NVDEC 提交、六个 Host1x 同步点与六帧软件参考吻合，子进程正常退出并回到同一 DWM。板端包装器误排 POC，已用原始归档按整数重核，留存初次判定及订正回执。[本轮及 13 组逐项覆盖](combined-13-4ab7804d/trial-summary.json) · [720p 原始归档复核](windows-native-graphics/rounds/win-combined-13-candidate-native720-20261121-02.result.json)

同一 640×360 Vulkan 窗口的 12 秒测量，DIB OFF 为 383 帧，旧包 OFF 对照为 414 帧。DIB ON 简单裁剪两次分别为 283 和 423 帧，帧间隔 p95 为 113.11 和 35.52 ms。复杂裁剪前后均为 type 3，窗口动画、Escape、退出码 0 和同桌面通过。独立原生 API 夹具比对了直接提交与参考 blit 的像素。生产 NVK 进程逐帧的 GDI 调用路线仍缺独立观测。[DIB OFF](combined-13-4ab7804d/wsi.json) · [DIB ON 首轮](combined-13-4ab7804d/dib-on.json) · [DIB ON 复跑](combined-13-4ab7804d/dib-on-repeat.json) · [复杂裁剪](combined-13-4ab7804d/dib-complex.json)

晋升检查因呈现相关开放故障拒绝该联合包，首个 Present 的历史根因保持开放。候选上机前的 `92083→92154` 有单独的 41/6008，已登记为原基线开放故障。[晋升检查](combined-13-4ab7804d/promotion-result.json) · [独立重启记录](combined-13-4ab7804d/baseline-restart-92154.json)

受控回退后的启动 `92516` 已恢复 `7f626083` 的永久许可与 Code 0，七文件和实际加载 KMD、DWM 模块相符。本轮任务、试用包与令牌均已清理。[回退与清理](windows-native-graphics/rounds/win-combined-13-restore-20261121-01.result.json)

上一正式 WDDM 2.6 源码 `7f626083`、实载签名 KMD `30331aa3cb57`。启动 92083 的原签名窗口首次 Present(1,0) 用时 14.302 ms，返回 S_OK。画面、输入、退出和桌面连续。DWM 同一 GM20B 节点 0 的 DMA 9652 / 队列序号 4444 有同线程 5063 进入与返回，以及同包中断、完成与退役。91476 的历史根因保持开放，91918→91989 是切换前独立故障。手持取证未覆盖 PL=1 高频。[上一基线回执](nvgpu-init-30331aa3cb57/permanent-boot.json) · [同轮原始证据与边界](windows-native-graphics/rounds/win-seq-new-baseline-profiler-20261121-01.result.json)

应用自己的首笔 52 字节 Blt 在 92083 对应 DMA 9651。Windows 回执绑定调用、包装和同包完成。KMD 在完成前的 GPU 围栏快照仍为 observed 0、expected 3272，上一完成值 9650，随后同包完成。有效 GR/FECS 缓存在该采样时点为零。[应用 Blt 同包证据](windows-native-graphics/rounds/win-seq-application-blt-fence-20261121-01.result.json)

本轮在手持、交流供电的 Switch 上验证 Windows 11 ARM64、GM20B 和正式 WDDM 2.6 基线 `c137ce75`。先核对普通双缓冲 SEQUENTIAL 首次 Present 与同对象的 GPU 围栏等待，再验收原生 D3D11、应用目录 DXVK、冷启动和受控重启。意外重启和 Wi-Fi 恢复各自保留启动与系统事件。《仙剑四》继续暂缓。[Windows 问题、优先级与进度](windows-native-graphics/index.json) · [单项轮次与开放故障](open-faults.json)

已有 27 轮 Windows 原生实测在新索引中逐项映射。124 条静态核对和 298 条来宾专属条目归入历史旁证。[原始缺口索引](wsl-dxgkrnl-contracts/gap-index.json) · [既有实机回执](wsl-dxgkrnl-contracts/native-27.json)

当前实机集中在原空窗口首次 Present。应用无写入，旧式 DDI 为 `0x000b000f`。先取同线程 Windows 内核接收和返回，再沿同一源分配、上下文与围栏值核对分页、等待、复制和 DWM。首轮清屏用例为正式基线对照。应用与启动专项随后接续。[Windows 本机验收索引](windows-native-graphics/index.json)

开机 `91476` 上，原无写入窗口第一次 `Present(1,0)` 在绘图 PID/TID `5992/5988` 取得 Windows 内核和 DXGI 的成功回执。源分配的最后一次分页检查早于公开调用 775.464 ms；普通上下文的 52 字节复制包正常退役。DWM 事件 91 的 EventId 与本次 HWND 一致，随后客户区 4,508 个采样点全部与空帧参考匹配，输入有效，应用正常退出。[原始应用日志与 Windows 事件载荷](windows-native-graphics/local-windows-logs.json) · [内核接收与返回](windows-native-graphics/rounds/win-empty-present-20261121-01.result.json) · [源分配](windows-native-graphics/rounds/win-empty-source-20261121-01.result.json) · [复制包](windows-native-graphics/rounds/win-empty-blt-20261121-01.result.json) · [DWM 调度](windows-native-graphics/rounds/win-dwm-window-id-20261121-01.result.json)

原空窗口绘图线程的值 1 信号在调用前退役，截至首次公开返回没有同线程的 GPU 等待请求。独立清屏窗口则有两组同设备、同对象及同值的跨上下文等待包完成。原三次故障客户端在公开 Present 与持久 RETURN 标记之间还要读取设备移除原因。三次原次的 CALL 之后均无匹配图形回执；当前实机回读的 System XML、WER、转储和滚动 ETL 未补出原次位置。继续开放 `GM20B-LEGACY-SEQUENTIAL-RESTART`。[GPU 信号与等待](windows-native-graphics/rounds/win-empty-fence-20261121-01.result.json) · [原次最后已返回的调用](windows-native-graphics/rounds/win-historical-call-marker-20261121-01.result.json) · [历史 Windows 系统证据](windows-native-graphics/rounds/win-historical-reset-20261121-01.result.json) · [原客户端进程记录](windows-native-graphics/rounds/win-historical-process-log-20261121-01.result.json)

原 27 轮中的 8 项通过判定保留。19 项未知里，15 项已取得缺少设备能力或操作未实现的回执。共享监控围栏的对象身份、普通围栏的值与退役仍可在 Windows 上核对。共享资源成员数和 CPU 映射保护方式只在与本次源或目标分配相符时引用。[27 轮与本机问题的逐项映射](windows-native-graphics/index.json)

五处历史目录的 155 份结果归档按内容去重后为 154 份。36 份配置对应原签名客户端，其中三份原无写入故障仍缺 CALL 后事件。较早的另一客户端在启动 `79396、79467、79692` 上也出现无写入首次 Present 后的异常开机。它把设备移除查询排在公开返回和 DwmFlush 之后。[原归档、组件与六次启动的边界](windows-native-graphics/rounds/win-first-present-archives-20261121-01.result.json)

开机 `91476` 的正式基线增加了一次独立实时见证。绘图 PID/TID `5540/7812` 的 DXGI 42/43、DxgKrnl 166/184 均已写到控制端，载荷和同次文件 ETL 逐字节一致。四次 fsync 确认返回 Windows 时进程都在运行。公开调用历时 13.242 ms，实时消费者首次取到事件已在公开 RETURN 后 11.358 ms。最长观察到的持久化往返上界为 47.022 ms。调用内发生即时复位的区间仍缺回执。本轮结束确认三个专属跟踪会话均已关闭，任务与临时目录已清理。[实时 Windows 回执、原始传输日志与哈希](windows-native-graphics/rounds/win-present-live-20261121-02.result.json)

启动 `86815` 的真实拒绝用例中，Windows Blt 166 比同一绘图线程的 miniport Present 入口早 29.635 微秒。事件 167 给出同一 DMA 缓冲的裁剪矩形，随后 KMD 普通上下文检查返回 `0xc00000bb`，KMT 返回 `0xc01e0006`，公开接口返回 `0x087a0001`。该次未提交呈现包。[同一份 ETL 中 Windows、KMD 与公开返回顺序](windows-native-graphics/rounds/win-blt-ddi-order-20261121-01.result.json)

在正式基线启动 `91476` 上，2 ms 名义等待的专属 ETW 会话完成 76 次刷新。以首个刷新开始和末个刷新返回计算，平均间隔约 16.55 ms。该次首次 Present 用时 15.364 ms，DXGI 42 消费回调早于公开 RETURN 0.246 ms，Windows Blt 166 消费回调晚于 RETURN 0.218 ms。四条事件都在目标进程存活时持久传到控制端，本次调用内尚缺内核事件的落盘确认。[短刷新窗口的实机计时与边界](windows-native-graphics/rounds/win-present-incall-20261121-01.result.json)

正式基线的三个无写入窗口各有两种同步对象。绘图线程在初值 0 的对象上发出值 1 的 GPU 信号，Windows 在同一对象登记 CPU 异步通知。源分配另用初值 7000 的分页对象，三次驻留及三次 GPUVA 映射在返回待处理后均出现同步 CPU 等待，目标分别为 7001、7002，随后 UMD 的完成检查返回 S_OK。原三次异常启动的 ETL 只覆盖信号，仍缺同对象等待和完成。[同设备的两个对象、六次同步等待与原次边界](windows-native-graphics/rounds/win-empty-fence-identity-20261121-01.result.json)

正式基线启动 91476 的另一无写入窗口由 PID/TID 7924/1936 绘图。专属高分辨率定时器完成 511 次刷新，按首末 QPC 折算平均间隔约 2.651 ms。首次 Present 历时 15.363 ms。Windows 实时消费者在公开 RETURN 前 4.013 ms 读到同窗口 Blt 166。控制端 fsync 的确认回到 Windows 时晚于 RETURN 3.983 ms。DXGI 42 的落盘确认早于 RETURN 0.542 ms。四条原始载荷均与同次文件 ETL 相符。[原始 QPC、载荷与持久化确认](windows-native-graphics/rounds/win-present-incall-20261121-02.result.json)

原三次启动共留下六个 ETL 文件。扫描全部 856,064 字节时，五个目标图形提供程序的 4,770 处标识均对应已正常解码的记录。34 个物理缓冲在已解析记录之后共余 225,648 字节，全部是 0xff 填充。三个原次都能检出 CALL 前的值 1 信号。CALL 后至复位之间仍缺同一绘图线程的内核回执。[原三次逐缓冲核对和正常调用对照](windows-native-graphics/rounds/win-original-etl-bytes-20261121-01.result.json)

启动 91476，绘图 PID/TID 584/1664。首次公开 Present 历时 19.221 ms。同窗口内核 Blt 166 的本机原始记录为 148 字节，两端 SHA-256、CRC32 及独立 ETL 均吻合。写入及文件刷新耗时 10.416 ms，写透返回相对公开 RETURN 为 +5.688 ms，相对 DXGI 43 为 +5.710 ms。 当前基线的写透结果与原三次故障按启动号分别保存。[本机原始记录、文件刷新时间及控制端回读](windows-native-graphics/rounds/win-present-local-blt-20261121-01.result.json)

原三次启动保留的 25 条 GM20B KMD 执行快照均来自 PID 4 的抢占观测，状态及队列错误均为零。五条位于 GPU fence 阶段的工作与 Windows DMA 提交、队列上下文、序号、字节数及完成事件全部吻合。发起 PID 分别为 5480、5604、5624，各包早于原窗口首次 CALL 92.352 至 112.563 ms 正常退役。原窗口自身调用前的监控围栏信号仍按值 1、队列序号 2 归属。三个恢复开机中的 Code 43 有各自启动许可拒绝回执。[原 KMD 事件、五个 Windows 包与恢复次状态](windows-native-graphics/rounds/win-original-kmd-telemetry-20261121-01.result.json)

正式基线的五次无写入窗口中，CDD 286 的目标经适配器分配 33、设备分配 36 对应到同一窗口的 Blt。复制包完成后 2.150 至 16.818 ms，各自的 DWM 事件 91 均有一个 EventId 数值等于当次 HWND。各窗口直到截图为止的 DWM 进程记录共 1,054 条，仍缺同提交的明确表面消费回执。[DWM 调度、目标分配寿命及翻转消费对照](windows-native-graphics/rounds/win-dwm-shared-destination-20261121-01.v2.result.json)

原三次的 Windows 进程生命周期审计未补出 CALL 后的退出位置。Security 环形日志最早留存记录的墙钟晚于目标时段，系统原始进程分析通道在查询时关闭。Switch 事件日志归档目录及 246 份本地历史 ZIP 均缺对应通道的归档。[原进程身份、正常查询对照及日志保留范围](windows-native-graphics/rounds/win-original-process-lifetime-20261121-01.result.json)

上述五次 52 字节复制在 Blt 166、队列提交 178 和 DMA 提交 175 携带相同的缓冲指针。DMA 完成号对应 177/176，队列包标识及序号对应 180。原先送审的摘要省略了 178/175，补核回执列出每次完整顺序。[五个应用复制包与后续 DWM 91 的逐字段核对](windows-native-graphics/rounds/win-dwm-blt-dma-identity-20261121-01.result.json)

五次正式空窗口的 DWM 91 均位于同线程的 63/64/65 令牌读取批次内。64 返回 S_OK，各批汇报 2 或 3 个令牌。两次批次结束后，另一 PID 的 Model=7 提交出现与应用 Model=3 相同的令牌地址。逐帧消费仍缺与应用提交对应的令牌记录，翻转对照则有事件 467。[同线程批次、令牌及翻转对照](windows-native-graphics/rounds/win-dwm-present-history-batch-20261121-01.result.json)

当前 Switch 启动 91476 能通过 Windows 公共接口读取并校验 FACP、DSDT。BERT、ERST、HEST 的逐表查询均返回 1168。原三次运行启动到对应恢复启动之间，System 未查到所选 WHEA 或 volmgr 转储初始化和写入事件。当前 Windows 配置小型转储并有活动分页文件。[固件表回读和原次 System 记录范围](windows-native-graphics/rounds/win-platform-reset-path-20261121-01.result.json)

同次 DBG2 描述的调试口是 NVIDIA 16550 的 `\_SB.UAR2`，与 DSDT 的 NVDA0100 设备对应。BCD 全局调试类型为 Local。控制端未取得来自该 UARTB 的采集回执，三次历史故障仍缺能与各自 CALL 对齐的板级证据。[调试口与控制端只读核对](windows-native-graphics/rounds/win-reset-external-transport-20261121-01.result.json)

先前四轮中，三轮的控制端 Blt 166 写入按消息因果顺序晚于 DXGI 43。高分辨率对照的上下界为 DXGI 43 前 3.986 ms 至之后 4.010 ms。[既有回执的写入时限](windows-native-graphics/rounds/win-controller-fsync-order-20261121-01.result.json)

随后在正式启动 91476，用同一驱动和应用，只实时转发该绘图进程的 CDD 286、Blt 166。两条记录均完成控制端 fsync，Windows 收到 ACK 的时点分别比 DXGI 43 +30.028 ms 和 +30.439 ms。回执附同窗口、适配器及 CDD 到 Blt 目标分配关系。[定向实时见证与完整 ETL 对照](windows-native-graphics/rounds/win-present-focused-20261121-01.result.json)

六份正式空窗口的会话覆盖所有进程和线程，共有 772 条 GPU 等待 API 和 195 条队列等待。三份历史故障在 CALL 前存有 84 条 API 和 21 条队列等待。两组均未发现等待目标与应用值 1 围栏匹配。独立清屏对照确认另一上下文的同对象、同值等待可以被当前解码识别并配对退役。[全进程围栏回执](windows-native-graphics/rounds/win-fence-all-owners-20261121-01.result.json)

原三次空窗口故障的值 1 信号退役后 0.274–0.391 ms，同绘图线程的 tgd3d 第 2 阶段返回 S_OK，随后旧式设备创建成功。同期翻转对照顺序一致。六份现行正式空窗口将同对象的 CPU 异步登记与初始化成功配对。原三次仍缺 CPU 句柄等待及首次 CALL 之后的回执。[初始化与信号阶段复核](windows-native-graphics/rounds/win-fence-cpu-return-20261121-01.result.json)

同卷 12 对文件样本中，提前预写使 148 字节同步写入和刷新中位数从 8.130 ms 降至 4.664 ms。5 次预写样本低于此前的 Blt 时间窗，11 次低于更早的 CDD 时间窗。[板端文件回读与逐样本 QPC](windows-native-graphics/rounds/win-local-receipt-storage-20261121-01.result.json)

开机 91476，绘图 PID/TID 4500/972。独立双路 ETL 与板端 64 KiB 文件中的 148 字节 Blt 166 记录匹配，验证 nonce、CRC 和文件尾部。同步写入及刷新用时 3.472 ms，成功返回相对 DXGI 43 +0.296 ms，相对公开 RETURN +0.278 ms。 同次核对了客户区、输入、退出和同一桌面。原三次重启仍需各自 CALL 后的持久记录。[预写槽实机回执](windows-native-graphics/rounds/win-present-preallocated-blt-20261121-01.result.json)

启动 91476 的同一绘图线程 4836/1256，板端槽的 84 字节 CDD 286 原始回执与独立 ETL 逐字节相符。CDD 目标经 global 33 与 device 36 绑定到同一 HWND 的 Blt 166。写入加刷新耗时 3.535 ms，成功返回相对 DXGI 43 -5.904 ms，相对公开 RETURN -5.926 ms。 同次窗口完成输入、退出及桌面连续检查。三份旧故障仍缺自己的 CALL 后回执。[同次 CDD 分配及板端写透回执](windows-native-graphics/rounds/win-present-preallocated-cdd-20261121-01.result.json)

正式基线启动 91476 的一次板端 CDD 取证发生意外重启，恢复为 91557。重启后完整读回同窗口 CDD 286，旧启动原件同时保留了复制退役、首次 Present、DwmFlush 和首帧资源释放成功。最后一条同绘图线程内核事件在公开返回后 138.531 ms。正常退出和复位首因缺回执。恢复后核对了 GPU Code 0、全哈希、手持设置和专属探针清理。旧三次故障各自的 CALL 后缺口继续开放。[本次故障及重启后回读](windows-native-graphics/rounds/win-present-board-only-cdd-20261121-01.result.json)

回读启动 91476 的全部原始图形提供程序。DWM 1400/2252 的窗口 91 在同次公开返回后 2.402 ms 匹配 HWND 328302。同一 DWM 线程的 tgd3d 资源进入事件 17 和 8 是后续留存末端，最后一条比公开返回晚 154.764 ms，尺寸 640×360。两份 ETL 的缓冲余区均为 FF。 此次全提供程序扫描补充了前轮按 DxgKrnl、DXGI 与 KMD 过滤时的末端。DWM 末端资源与原窗口的具体对象归属、该资源调用返回及复位原因仍待证。[DWM 末端与同一窗口配对](windows-native-graphics/rounds/win-board-cdd-dwm-tail-20261121-01.result.json)

两路异常 ETL 确认 DWM 的 3892 字节 DMA 包 48648 在首次 Present 返回后 43.870–43.871 ms 正常退役。晚些时候 KMD 通知的 152584 匹配另一个系统队列包 209489，该包也完成。随后 DWM 3524 字节包 48660 经两次抢占，于 +145.342 ms 第三次提交 152586。两路档案均缺该次 DMA 完成和队列退役。[异常轮提交与完成身份](windows-native-graphics/rounds/win-board-cdd-post-present-retire-20261121-01.result.json)

同一启动的健康对照中，同一 DWM 线程和上下文的 4632 字节包也被抢占两次。第三次发出后 6.719 ms 正常退役，继而同对上下文退役同一同步对象值 21985 的信号、等待及后续呈现。异常链的同步对象值为 24055，已确认信号、等待及下一 DWM 呈现入队，退役回执缺失。档案未能将这条链归属初始 HWND 或连到复位首因。原三次历史故障仍缺各自 CALL 后的直接观测。[同对象健康对照与异常链](windows-native-graphics/rounds/win-board-cdd-dwm-preempt-control-20261121-01.result.json)

物理缓冲定位表明，异常轮两个共享 ETL 在 152586 后分别仍保留 0.126 和 9.423 ms 的其他事件。此前处理同队列完成的线程 4/884 在两路都没有后续记录。封口缺失，档案只能给出事件留存范围。[物理缓冲及对应完成线程](windows-native-graphics/rounds/win-board-cdd-dwm-completion-coverage-20261121-01.result.json)

同次实载 KMD 的最后一条遥测是匹配 Windows 抢占请求 20571 的成功状态，早于第三次 DMA 提交记录 0.276 ms。正式 KMD 的常规成功提交路径没有配套进入或返回事件。152586 的最早直接缺口在 miniport 准入及硬件开始，之后还缺完成。[KMD 源码点与原始请求对照](windows-native-graphics/rounds/win-board-cdd-dwm-third-submit-kmd-20261121-01.result.json)

恢复开机 91557 的 dxgkrnl.sys 全哈希与事故轮一致。主发布者可读 640 份模板，三份健康档案均有同号 DMA 完成，八份健康与事故 ETL 中均未采到队列时间戳、GPU 历史缓冲或硬件队列进度。SysMm 发布者的模板读回报数据无效，正式基线源码给出的 GPU 时钟和历史缓冲能力见回执。[图形提供程序与健康样本核对](windows-native-graphics/rounds/win-native-submit-event-catalog-20261121-01.result.json)

在固定基线的隔离工作树中生成了本地 ARM64 诊断包。按 ETW 关键字启用后，同一提交号可以记录进入、准入、GP_PUT 已发布并读回、GPU 完成值已匹配。x64 WDK 受控队列夹具通过，包含拒绝准入和发布前失败。下一轮须完成许可流程、实载身份与正常负载配对，之后才能采用其硬件观测。[候选构建与夹具回执](windows-native-graphics/rounds/win-kmd-dma-boundary-build-20261121-01.result.json)

Switch 正式启动 91557 上的签名 ARM64 队列夹具返回 0。目录签名及 12 个成员均经同机验证，本轮目录已清理，前后同一启动、DWM 1392、正式全哈希和 Code 0。签名诊断包保留本机，实载后的正常 GPU 提交配对仍待单轮验证。[原生夹具和签名包回执](windows-native-graphics/rounds/win-kmd-dma-arm64-fixture-20261121-01.result.json)

候选的一次性启动前，91650 已持久记录令牌值 1、Code 43，并收到 Windows 重启请求 1074。其后 91732 记录 6008/41，BugcheckCode=0。应用未发出，未取得候选 Code 0 或四阶段配对。最后一条关机实证是 91725/6006，距下一次有记录的内核开始 57.750 秒。前后两次受控对照均有 109/13，本段缺失。 启动 91832 已恢复正式 Code 0、永久许可和加载哈希。 没有收到候选 Code 0 或工作负载回执。异常开机转换的第一失败组件、板级记录及原案 152586 的实际 KMD 接收仍待证。 [实载 DMA 校准与恢复回执](windows-native-graphics/rounds/win-kmd-dma-hardware-calibration-20261121-01.result.json)

启动 91476 的场景 ETL 记录了另一个 DWM 上下文。该上下文先前的 DMA 246、后续信号和呈现均退役，随后在主上下文的 152586 后 9.054 ms 记录 DMA 247 提交。两路未封口 ETL 此后都没有留存任何上下文的 DMA 完成中断、完成或队列退役。较晚上下文仍缺适配器和节点归属，152586 仍缺同号 KMD 准入和完成。[两路原始载荷、正常对照与当前实机回执](windows-native-graphics/rounds/win-dwm-late-adapter-152586-20261121-01.result.json)

16:27 UTC 的 SSH 断线跨启动 91832 至 91918。RuntimeBroker 先发出重启请求，随后有完整 Windows 关机回执。新启动 16:28:31 WLAN 自动连回，SSH 第一次三分钟重试成功。读回 Code 0、交流供电，功耗档位为 6。[Windows、WLAN 和 SSH 回执](windows-native-graphics/rounds/win-wifi-recovery-20261121-01.result.json)

启动 91918 复查时已回到 Handheld1，本轮只读。两份完整读回仍为同桌面、Code 0、交流供电，正式永久许可及 7 份组件、5 份实载哈希全部一致。[同启动手持模式及正式基线回执](windows-native-graphics/rounds/win-post-restart-handheld-20261121-01.result.json)

对照开机 91476 同一 DWM 实例的七个正常归档，较晚上下文有 105 个 DMA 号能对应 Windows 完成中断及非抢占完成。十六路原始 ETL 中，只有主上下文得到与正式 GM20B KMD 状态匹配的编号。较晚上下文的适配器、节点和 247 的完成仍待证。[同启动跨档案、字段和哈希](windows-native-graphics/rounds/win-dwm-late-context-cross-archives-20261121-01.result.json)

在正式基线启动 91918，只读 ETW 状态快照把 DWM 7 个既有上下文、节点、两个适配器的 LUID 和调度对象字段直接配对。其中 GM20B 有 4 个上下文，同一原生上下文留有两组完整的正常 DMA 提交及退役。采集过程保持同桌面及 Code 0，专用会话和临时 ETL 已清理。[状态快照载荷及实载哈希](windows-native-graphics/rounds/win-dwm-context-state-capture-20261121-01.result.json)

正式启动 91918 复跑原双缓冲空窗口。状态快照先落盘，首次 Present 耗时 16.629 ms，52 字节应用 Blt 退役。调用至客户区采样期间，DWM 的四个 DMA 和四组跨设备围栏信号、等待均在 GM20B 节点 0 匹配完成。同包 12776 有正式 KMD 快照。4,508 个客户区采样点与独立空帧对照相同，输入、退出及同一桌面通过。[同次启动的原始回执与哈希](windows-native-graphics/rounds/win-seq-first-present-dwm-rundown-20261121-01.result.json)

比对匹配 Windows ARM64 映像和 Microsoft PDB，确认 DxgKrnl 175.1 在 VidSch 路径中先于释放 DPC 队列锁，随后才选择物理、虚拟提交或抢占 DDI。历史 152586 与正常 12776 的事件描述符完全一致。历史启动缺 dxgmms2 文件哈希以及同号的锁释放、进入及返回回执。[指令位置、接口表和两份原始载荷](windows-native-graphics/rounds/win-dma175-ddi-order-20261121-01.result.json)

正式启动 91918 仅启用 Windows Profiler。8 组同线程事件 175.1、105、106 可以配对，7 组归属 GM20B。DWM 的 DMA 17588 在已确认的 GM20B 节点 0 经过虚拟提交包装进入、返回及同号完成。两路历史异常 ETL 无此事件族。会话和临时档案已清理。[Windows 自带 Profiler 的原始回执](windows-native-graphics/rounds/win-dma-ddi-profiler-20261121-01.result.json)

正式启动 91918 的同一限时会话中，52 条实时记录均与原始 ETL 一致且完成控制端 fsync 和 Windows 确认。DWM 的 21991/8136 覆盖 GM20B 节点 0 的 5063 进入与返回以及实际完成。同包持久化确认延迟实测为 19.186 至 22.556 ms，短于该延迟的骤停区间仍有证据缺口。[同包原始记录、耗时及清理回执](windows-native-graphics/rounds/win-dma-ddi-profiler-live-20261121-01.result.json)

原签名窗口首次 Present 15.600 ms 成功，图像、输入、退出和同桌面通过。实时会话上限早于 CALL 651.542 ms，已独立留存 768 条原始回执。应用视频 ETL 完整，另一路显示 ETL 达上限并丢 4 个缓冲。[联动时间线及失败边界](windows-native-graphics/rounds/win-gated-seq-profiler-present-20261121-01.result.json)

启动 92083 同一原窗首次 Present(1,0) 约 36.285 ms 成功。实时接收 3020 条均与封口 ETL 相符，含 KMD 事件 58 条。3 条同围栏生产 KMD 快照有场景 ETL 副本、控制端 fsync 和 Windows ACK，覆盖 submit、prepare、mappings 阶段，均在同提交完成之前发出。首条是 DWM DMA 13425。[生产 KMD 实时取证回执](windows-native-graphics/rounds/win-seq-live-kmd-fence-cache-20261121-01.result.json)

启动 93748 的正式兼容基线使用原生 ARM64 应用复测旧式双缓冲 SEQUENTIAL 空窗口。两轮首次 Present(1,0) 均返回 S_OK，分别用时 14.900 和 25.616 ms。两次 52 字节 Blt 都在返回前退役，桌面客户区各 4,508 点与 WARP 空窗口对照一致，输入检查通过，进程退出码为 0，DWM 未重启。第一轮实时流在调用前 141.097 ms 截止，封口场景 ETL 保留了 DMA 8575 的完成与退役。第二轮重新对齐实时窗口，核实 DMA 11582 的 Windows 5063 包装进入、返回、同号完成中断、DMA 完成和退役。应用包无独立 KMD 硬件起动标记。[第一轮](windows-native-graphics/rounds/win-seq-arm64ec-kmd-fence-20261121-01.result.json) · [第二轮](windows-native-graphics/rounds/win-seq-arm64ec-5063-realigned-20261121-01.result.json)

两轮都有同 HWND 的 DWM 91 调度。按同次 Blt 目标和呈现史令牌复核，仍缺能把该应用表面直接连到 DWM 消费的回执。历史启动 91476 的最后一条已知 KMD 回执是抢占请求 20571 成功，约 0.276 ms 后 Windows 发出 DWM 的第三次 DMA 提交 152586。该次提交的 miniport 准入、返回、完成及首个复位异常仍待证；两项历史重启故障保持开放。[DWM 分配与令牌复核](windows-native-graphics/rounds/win-dwm-consume-arm64ec-93748-20261121-01.result.json) · [历史内核边界](windows-native-graphics/rounds/win-board-cdd-dwm-third-submit-kmd-20261121-01.result.json)

启动 93748 的第二轮客户端正常退出后，显示 ETL 在 Windows UTC 22:11:07.250 封口。距下一次内核启动 93818 的记录为 60.010 秒。窗口任务清理有回执，随后在 Mac 上的回写和租约交接分别计时。41/6008、BugcheckCode 0 的异常启动继续登记为开放故障。[93748 清理与异常启动时序](windows-native-graphics/rounds/win-post-seq-gap-93818-20261121-01.result.json)

恢复后的正式启动 93818 保持已选组件。两次诊断准备分别受 4,096 条实时记录上限和 8 MiB 应用 ETL 限制，均停止于公开 Present 之前。关闭测试专用 ETW 后，同一签名空窗口的首次 Present(1,0) 返回 S_OK，用时 97.894 ms，设备状态与 DwmFlush 正常，输入响应已观察，应用退出码 0。缺本次返回后客户区像素。[实时采集上限](windows-native-graphics/rounds/win-seq-post-exit-tail-93818-20261121-01.result.json) · [应用 ETL 上限](windows-native-graphics/rounds/win-seq-post-exit-ungated-93818-20261121-01.result.json) · [同签名应用回执](windows-native-graphics/rounds/win-seq-post-exit-plain-93818-20261121-01.result.json)

同一应用清理后，第一个只读采样为 14.718 秒。20 条同启动与同 DWM 读回覆盖应用退出后 51.634 至 180.667 秒，采样最长间隔 8.053 秒，期间没有新增 41、6008、BugCheck 或 4101。正式组件、交流供电、GPU Code 0 和永久许可均复核。历史启动 93748 的首个异常、旧 DWM DMA 152586 的准入和完成仍待相应故障证据。[尾部观察回执](windows-native-graphics/rounds/win-seq-post-exit-readback-93818-20261121-01.result.json)

提交 ef790ec9 补充 opt-in KMD 提交返回状态。正式启动 93818 上，Windows 签名 ARM64 队列模型的原生退出码为 0，Inf2Cat 生成的目录签名和 12 个成员通过核验。该轮保留正式驱动、永久许可和同一桌面。事件 7/v2 仍待一次性候选实载校准。[目录和模型验证](windows-native-graphics/rounds/win-kmd-return-catalog-93818-20261121-02.result.json)

正式启动 93818 仍绑定实核回退包 oem30.inf，Driver Store 的目录和全部 13 个文件哈希吻合。隔离试用目录已准备，8 个组件中仅 KMD 的哈希不同，正式驱动和永久许可已回读。新 ETW 采集器待复核后进行一次性诊断。[隔离目录准备](windows-native-graphics/rounds/win-kmd-return-profile-stage-93818-20261121-01.result.json)

## 此前交接记录

工作目录为 `/Users/rujian/Documents/Codex/mesa-switch-woa-tgd3d-vulkan`，分支为 `tgd3d-vulkan`。原会话 `01a1ec52-4325-7bf2-b65d-3c934aadfad2` 保留。新会话可以自行创建进行中的长期目标，专门接手下述第四步。前三步从新会话的待办中移除，已有能力查询、游戏和 Pacecar 验收留档。[四组游戏及 ARM64EC 记录](DXVK_ZINK_GAME_MATRIX_20261118.json)

## 第四步 · 查清旧式窗口首次 Present 的重启

用户于 11 月 21 日要求先搁置《仙剑四》，后续会话按此状态暂缓其启动与复测。本轮已结束的早停记录显示，第七张纹理可以完成显存选择；之后一次最后记录在 `vkBindImageMemory` 入口，随即发生意外重启。该次绑定观察只有一轮，原因与重复性仍待恢复此用例时确认。游戏原 DLL、正式 GPU 组件已核对恢复，临时任务和目录已清理。[最新边界与暂缓状态](PAL4_IMAGE_BIND_BOUNDARY_20261121.json)

11 月 21 日，《仙剑四》原生窗口模式三次在第七张 512×512 托管纹理进入构造后重启。三份应用日志都保留了构造入口，均缺成功返回和输出指针标记。这版观察器没有独立记录异常返回。Windows 分配跟踪未正常封口，第七次调用中新建分配的归属仍为未知。第二轮只启动游戏一次，后来还有两段原因未知的恢复开机区间。三轮结束均已恢复原游戏 DLL，并核对已选 GPU 基线。[三轮逐项记录](PAL4_TEXTURE_CONSTRUCTOR_BOUNDARY_20261121.json) · [恢复期间的独立开机记录](wsl-dxgkrnl-contracts/rounds/pal4-texture7-allocation-owner-20261121-02.recovery.json)

微软 WSL2 `dxgkrnl` 源码注释提供待核验的问题。每轮 Switch 实机先登记一个注释编号、责任方、触发条件和计划哈希，结束后把该轮的实际证据与结论写回故障表。同一问题在不同进程和启动中分别记录。独立 x86 客户端已确认 `MakeResident` 的原始异步返回与 Windows 内核一致，游戏进程也有 16 次封口的内核返回记录。分页完成与重启根因还需要各自的证据。[原生驻留返回实测](PAL4_NATIVE_RESIDENCY_20261121.json) · [故障表](open-faults.json)

原游戏中两个执行上下文已关联到同进程创建的原生 GM20B 设备。主纹理的 Vulkan 图像创建前检查点已命中，随后独立读回相同启动与 DWM、游戏进程消失、原 DLL 恢复以及正式组件哈希。控制脚本漏写了该次游戏汇总，已保留这项限制。更早的一轮早停点误放在另一个函数，保持未知。未早停轮已记录到 `vkCreateImage` 与 `vkGetImageMemoryRequirements2` 返回成功，下一步沿后续显存分配、绑定与纹理构造继续定位。[游戏设备归属与早停记录](PAL4_PREIMAGE_CONTEXT_20261121.json)

11 月 21 日，实机使用 WDDM 2.6 基线 `c137ce75`。分层 GDI 源窗口和 DirectComposition 镜像显示了两帧不同图案，116,380 次像素比对全部匹配。源窗口收到 F8 按下和释放，程序正常退出。无丢失的 ETW 记录中，CDD 事件 287 和生产 UMD 事件 34 均为零。标准 GDI allocation-only 导入仍缺同一适配器上对应分配进入 `OpenResource` 的实证。[分层窗口回执](GDI_LAYERED_COMPOSITION_20261121.json)

《仙剑四》移至文档目录后，已补录 x86 Vulkan Loader 的游戏路径。原生 DXVK 随后识别出 GM20B，手持模式下三次开窗后都重启，单独移除桌面抓图也复现了重启。三次 x86 llvmpipe 对照都完成了游戏初始化。DXVK 在调用 Vulkan 创建交换链之前写下所选参数，日志未给出该调用的返回结果。第一次随后异常重启，后两次需要强制关闭游戏。第三次直接读取客户区和合成桌面，两个位置都只读到桌面图像。随后延长软件对照，手持模式下两次拍到主菜单，分别在约 355 秒和 316 秒。第二次向“离开游戏”发出点击，约 335 秒时菜单仍在，随后发生异常重启。正常退出和重启根因仍待核实。[游戏对照和启动记录](PAL4_DOCUMENTS_STARTUP_20261121.json)

《仙剑四》启动阶段的 17 个像素着色器已在同一次开机中单测。x86 DXVK 绑定原生 NVK GM20B 和软件 llvmpipe 时，每种都完成了 34 次游戏着色器离屏绘图和一次已知颜色对照。两端都正常退出。两端相差超过一个色阶的 1,621 个像素全部位于合成纹理的四条采样分界线上，其余 15,300 个像素最多相差一个色阶。[着色器实机回执](PAL4_SHADER_REPLAY_20261121.json)

整款游戏的原生复现再次发生异常重启。17 次创建像素着色器均有返回，第一次 Vulkan 队列提交及等待均成功。保留下来的 193 条记录中，上屏和绘图调用均为零。末尾仍在执行的操作是游戏线程第七次创建纹理和工作线程第十七个编译任务。游戏自身的日志停在玩家管理器初始化。最后一条记录到下一次 Windows 开机事件相隔约 141 秒，期间的故障起点仍待定位。开机后已核对游戏原 DLL 和正式驱动哈希。[游戏启动时间线和开放故障](PAL4_SHADER_REPLAY_20261121.json#game_startup_boundary)

第七张纹理与第六张参数相同，均为 512×512 的 X8R8G8B8 托管纹理，使用自动生成 mipmap。第一次暂停分配的测试被用户关机中断。复测时游戏在分配前停留 155.7 秒并持续存活，随后由测试程序结束。开机记录 90934、DWM 进程和 Code 0 显卡均连续。用原 DXVK 独立创建七张纹理，以及先登记 17 个真实着色器再创建七张纹理，两组都成功释放资源并正常退出。整款游戏的首次上屏和正常退出仍待验收。此前开机 90872 的异常结束另列在开放故障登记表。[纹理复测与独立对照](PAL4_TEXTURE_CHECKPOINT_20261121.json)

x86 DXVK 的候选设置可延后 D3D9 像素着色器转换。同一启动的对照中，绑定前生成的调试 SPIR-V 从 17 个降至 0 个，绘图后两组均为 17 个。两组各完成 35 次读回，17 份游戏着色器图像与原版逐字节匹配。整款游戏实际启用该设置后仍在主菜单前异常重启，开机记录由 90934 变为 91011。已恢复原游戏 DLL，候选未晋升。[编译时机与完整游戏结果](PAL4_DEFERRED_PIXEL_COMPILATION_20261121.json)

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
