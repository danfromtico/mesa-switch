# 27 条原生可比契约与首次 Present

从提交 `a73e3b76` 的 27 条原生可比未知项出发，逐项预登记并执行了 27 轮实机验证。**8 条已满足，0 条已违反，19 条未知。** Switch Lite GM20B 使用 WDDM 2.6 基线 `c137ce75`。全程交流供电、手持模式、GPU Code 0，启动记录 91476 和 DWM 1400 连续。[逐项记录、设备哈希与清理回执](native-27.json)

## 三次旧式双缓冲窗口的证据边界

历史窗口使用 `step4-present.exe` 进程级 DDI 枚举候选，实际记录的旧式 DDI 为 `0x000b000f`。本轮独立 KMT 客户端使用已验收的 WDDM 2.6 基线。旧式 SEQUENTIAL 窗口在三次异常启动中都成功建立交换链、取得后缓冲并创建渲染目标视图。绘图线程在首次 `Present(1,0)` 前查询的设备移除原因均为 `S_OK`。重新解码同一线程的 tgd3d 事件 6，并按各自启动内的分配句柄配对，三次均可见两份分配、四次映射回调和四次对应的成功分页检查。映射回调返回的是 HRESULT `E_PENDING`，每次记录的映射长度为 983040 字节。独立 KMT 用例另在调用点读到 NTSTATUS `0x103`。[原始四份归档的回执、哈希与 QPC](native-27.json) · [tgd3d 固定版回调记录](https://github.com/ruje0504-2/mesa-switch-woa/blob/86b11bff561ea8107aadd060249eb14a226bd1c0/src/nouveau/tgd3d/tgd3d_runtime.cpp#L217)

| 启动 | 旧式窗口效果 | 发值 1 的信号包退役距调用 | 最后一次成功分页检查距调用 | 调用之后的应用回执 |
| ---: | --- | ---: | ---: | --- |
| 82058 | FlipSequential | 590.047 ms | 430.848 ms | Present 返回 S_OK |
| 82058 | Sequential | 360.492 ms | 259.940 ms | 未留存返回 |
| 82221 | Sequential | 1101.095 ms | 280.943 ms | 未留存返回 |
| 82382 | Sequential | 400.943 ms | 307.703 ms | 未留存返回 |

三次异常启动的四个映射后续分页目标均依次为 7002、7003、7005、7006。目标 7006 的成功检查晚于各自序号 2 的信号包退役。分页检查含已完成的快速路径。三次调用后的图形记录均缺失，最终呈现源句柄、重定向目标、首次 KMD 进入与重启发起阶段仍未定位。[围栏信号匹配](../legacy-sequential-step4/historical-first-present-fence.json) · [三次异常启动边界](../legacy-sequential-step4/historical-restart-order.json)

后续独立的诊断启动在首次调用区间内记录了 640×360 staging 事件、普通上下文 Blt 的 KMD 进入和 `STATUS_NOT_SUPPORTED` 返回。随后 KMT 返回 `STATUS_GRAPHICS_PRESENT_OCCLUDED`，公开接口返回 `DXGI_STATUS_OCCLUDED`。诊断启动与功能候选的三次正常 SEQUENTIAL 对照，KMT flags 均为 `0x04011081`。四次都用同次 KMT 的源分配句柄配对到调用前的 921600 字节映射和成功的分页目标 7002 检查。三次功能候选随后都记录了 52 字节复制包及其完成，KMT 返回成功。WARP 对照的软件 Present 成功，输入判据未通过。这几组记录各有自己的进程、组件和启动范围。[诊断 DDI 回执](../legacy-sequential-step4/staging-ddi-result-13286e3e.json) · [功能候选验收](../legacy-sequential-step4/window-copy-c09c3c86/qualification.json) · [WARP 对照](../legacy-sequential-step4/warp-control-5.json)

## 逐项结果

每个编号链接到本轮回执。相邻计划文件及其 SHA-256 保存在故障表中。原生围栏能力查询取自相同 GM20B LUID 的 `KMTQAITYPE_WDDM_3_1_CAPS`。系统导出创建和打开入口，设备返回 `NativeGpuFenceSupported=0`。[故障表](../open-faults.json) · [微软原生围栏能力接口](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/native-gpu-fence-objects)

| 契约 | 判定 | 实机观测或缺口 | 对首次 Present 的意义 |
| --- | --- | --- | --- |
| [ioctl.c:2581:2](rounds/c27-gpuva-map-20261121-01.result.json) | 已满足 | 64 KiB、16 页的合法映射直接返回 0x103，输出 VA 65536 与保留地址一致。对应分页围栏目标为 7002，同步后实读 7002。 | 区分调用前 GPUVA 映射获准和分页完成。历史信号包退役无法代替具体映射调用的返回。 |
| [dxgkrnl.h:197:2](rounds/c27-monitored-cpu-va-20261121-01.result.json) | 已满足 | 同一受监控围栏的 CPU 映射先读到 0。事件 30、1069 和 294 关联到同一设备、上下文、局部围栏和 GPU 信号值 1。成功等待后该映射读到 1。本轮没有可配对的调度包退役事件。 | 为旧回执补充独立的 CPU 读值对照。历史启动只有 GPU 信号退役，仍缺当时绘图进程的 CPU 读取。 |
| [dxgkrnl.h:723:2](rounds/c27-shared-resource-owner-20261121-01.result.json) | 已满足 | 两个不同进程在各自设备取得资源和分配句柄。两份 DeviceAllocation 事件的非零共享资源身份和全局分配身份分别相同，能配对到每次合法创建或打开。 | 测试可用于配对绘图端与合成端的共享后备。需另将首次 Present 的重定向表面标识绑定到相应设备和调用区间。 |
| [ioctl.c:1710:4](rounds/c27-shared-private-data-20261121-01.result.json) | 已满足 | 独立打开方查询到 1 份分配、16 字节逐分配私有数据。OpenResourceFromNtHandle 返回的分配描述符指向该输出缓冲，版本、标志和 65536 字节大小逐字段匹配创建输入。 | 覆盖共享分配数据交给打开端的阶段。重定向表面的专用布局及历史首次 Present 中实际传递的描述符仍须当次记录。 |
| [ioctl.c:1630:4](rounds/c27-append-allocation-20261121-01.result.json) | 已满足 | 同一未共享资源接受了第二份 64 KiB 分配。两次创建的分配句柄不同，调用方资源句柄相同，各自对应的事件 33/36 均关联到同一设备和资源。 | 区分新建表面与向既有资源添加成员。历史崩溃场景需能识别发生在重定向表面上的那一次调用。 |
| [dxgkrnl.h:684:4](rounds/c27-sealed-members-20261121-01.result.json) | 未知 | 共享后在创建方、打开方、打开结束时查询的成员数为 1、1、1。现有同层记录没有成员集合被固定或禁止追加的直接观测。 | 区分共享后成员稳定的观测与禁止追加的内部条件。历史异常需要绑定到具体交换链与成员集合。 |
| [dxgkrnl.h:759:2](rounds/c27-allocation-cache-20261121-01.result.json) | 未知 | 正常 Lock2 返回可读写的 64 KiB 页，VirtualQuery 的 Protect 和 AllocationProtect 均为 0x404（PAGE_READWRITE 或 PAGE_WRITECOMBINE）。该轮没有可缓存映射，事件 33 仅给出未具名的原始 Flags=0x81，缺少可缓存映射与明确定义的 Cached 位配对。 | 可缩小 CPU 锁定与 staging 读回的观测范围。只凭可访问页或窗口出现不能判定缓存一致性。 |
| [dxgvmbus.c:3651:2](rounds/c27-allocation-property-20261121-01.result.json) | 未知 | 本次自有分配的 Unmoveable 更新返回 STATUS_NOT_IMPLEMENTED（0xc0000002），输出分页围栏为 0。未取得这次 API 的异步成功回执。 | 单独检查属性更新的分页路径。映射或 MakeResident 返回 0x103 都不能说明首次 Present 调用了这个 API。 |
| [dxgsyncfile.c:266:2](rounds/c27-open-monitor-wait-only-20261121-01.result.json) | 已满足 | 创建方以 NT 安全共享创建监控围栏，子进程合法继承其导出对象，以 NoSignal 打开，得到非零本地句柄并成功等待值 1。两端均正常释放。 | 显示等待专用的打开与同步。旧首帧的信号包自身没有显示任何等待方或跨进程打开。 |
| [dxgsyncfile.c:381:2](rounds/c27-open-monitor-local-20261121-01.result.json) | 已满足 | 两个不同 PID 在各自设备持有同一 NT 共享监控围栏的本地 KMT 句柄。打开方等待值 1 后发出值 2，创建方读到应答，两端分别正常释放。 | 区分同步对象共享与纹理共享。历史 Present 的对象打开方仍需实际进程和对象关联。 |
| [dxgkrnl.h:199:2](rounds/c27-sync-process-handle-20261121-01.result.json) | 已满足 | 两个不同 PID 在各自设备持有同一 NT 共享监控围栏的本地 KMT 句柄。打开方等待值 1 后发出值 2，创建方读到应答，两端分别正常释放。 | 帮助把绘图进程的局部同步句柄与 DWM 的局部句柄分开。历史判定需要同时确认进程和共享对象。 |
| [dxgkrnl.h:231:1](rounds/c27-shared-parent-20261121-01.result.json) | 未知 | 传统全局围栏创建返回 00000000，有效打开回执数为 1。合法全局等待之后的监控围栏读到 1（等待返回 0x00000000）。NT 共享路径打开返回 00000000。本次公开句柄及 ReportSyncObject 模板不提供全局表项指向的父对象。 事件 1069 把两个进程的局部句柄配对到同一非零 pSyncObject。 | 帮助辨认同一个信号对象。只见值传播或共享句柄不足以辨认历史首次 Present 使用的内核父对象。 |
| [dxgkrnl.h:615:1](rounds/c27-hardware-queue-20261121-01.result.json) | 未知 | GM20B 报告 WDDM 2.6。WDDM_2_7_CAPS 返回 0x00000000，HwSchSupported=0、HwSchEnabled=0。已选 KMD 的普通上下文路径不接受 HwQueueSupported 标志，缺硬件队列创建与设备归属回执。 本轮实机采集到能力查询。HwQueueSupported 的入口限制来自已选 KMD 源码。 | 区分普通上下文调度包与硬件执行队列。历史序号 2 的 QueuePacket 不能单独证明硬件队列。 |
| [dxgkrnl.h:145:1](rounds/c27-sync-object-scope-20261121-01.result.json) | 未知 | GM20B 的 DRIVERVERSION=2600。WDDM_3_1_CAPS 返回 0x00000000，NativeGpuFenceSupported=0。系统创建/打开原生围栏入口导出分别为 1、1，本基线缺所需原生类型的实际调用回执。 普通监控围栏 CPU 读值依次为 0、1。在活动 GM20B 目标上创建周期型围栏返回 0x00000000。整条的原生围栏及来宾链表/FD 语义仍缺对应回执。 本轮周期型围栏在同一 CPU 映射的两次采样值为 0、218533。 | 旧三次启动有围栏信号退役，整条还涉及类型、归属、跨进程与不同同步方式，需分别观测。 |
| [dxgkrnl.h:218:2](rounds/c27-native-type-20261121-01.result.json) | 未知 | 已查询当前适配器，NativeGpuFenceSupported=0。缺少同一受支持的原生围栏对象与调用区间内的创建类型回执。 | 涉及原生围栏的创建类型。按本机能力决定是否能进入这条路径；与已观察到的普通监控围栏 GPU 信号分开。 |
| [dxgkrnl.h:256:2](rounds/c27-native-shared-type-20261121-01.result.json) | 未知 | 已查询当前适配器，NativeGpuFenceSupported=0。缺少同一受支持的原生围栏对象与调用区间内的共享子类型回执。 | 涉及原生围栏的共享子类型。按本机能力决定是否能进入这条路径；与已观察到的普通监控围栏 GPU 信号分开。 |
| [dxgvmbus.c:825:2](rounds/c27-native-open-type-20261121-01.result.json) | 未知 | 已查询当前适配器，NativeGpuFenceSupported=0。缺少同一受支持的原生围栏对象与调用区间内的NT 对象类型回执。 | 涉及原生围栏的NT 对象类型。按本机能力决定是否能进入这条路径；与已观察到的普通监控围栏 GPU 信号分开。 |
| [dxgvmbus.c:889:2](rounds/c27-native-open-permissions-20261121-01.result.json) | 未知 | 已查询当前适配器，NativeGpuFenceSupported=0。缺少同一受支持的原生围栏对象与调用区间内的打开映射权限回执。 | 涉及原生围栏的打开映射权限。按本机能力决定是否能进入这条路径；与已观察到的普通监控围栏 GPU 信号分开。 |
| [dxgvmbus.c:896:3](rounds/c27-native-open-intra-20261121-01.result.json) | 未知 | 已查询当前适配器，NativeGpuFenceSupported=0。缺少同一受支持的原生围栏对象与调用区间内的INTRA_GPU 打开输出回执。 | 涉及原生围栏的INTRA_GPU 打开输出。按本机能力决定是否能进入这条路径；与已观察到的普通监控围栏 GPU 信号分开。 |
| [dxgvmbus.c:900:3](rounds/c27-native-open-default-20261121-01.result.json) | 未知 | 已查询当前适配器，NativeGpuFenceSupported=0。缺少同一受支持的原生围栏对象与调用区间内的DEFAULT 打开输出回执。 | 涉及原生围栏的DEFAULT 打开输出。按本机能力决定是否能进入这条路径；与已观察到的普通监控围栏 GPU 信号分开。 |
| [dxgvmbus.c:3147:2](rounds/c27-native-create-permissions-20261121-01.result.json) | 未知 | 已查询当前适配器，NativeGpuFenceSupported=0。缺少同一受支持的原生围栏对象与调用区间内的创建映射权限回执。 | 涉及原生围栏的创建映射权限。按本机能力决定是否能进入这条路径；与已观察到的普通监控围栏 GPU 信号分开。 |
| [dxgvmbus.c:3152:3](rounds/c27-native-create-intra-20261121-01.result.json) | 未知 | 已查询当前适配器，NativeGpuFenceSupported=0。缺少同一受支持的原生围栏对象与调用区间内的INTRA_GPU 创建输出回执。 | 涉及原生围栏的INTRA_GPU 创建输出。按本机能力决定是否能进入这条路径；与已观察到的普通监控围栏 GPU 信号分开。 |
| [dxgvmbus.c:3156:3](rounds/c27-native-create-default-20261121-01.result.json) | 未知 | 已查询当前适配器，NativeGpuFenceSupported=0。缺少同一受支持的原生围栏对象与调用区间内的DEFAULT 创建输出回执。 | 涉及原生围栏的DEFAULT 创建输出。按本机能力决定是否能进入这条路径；与已观察到的普通监控围栏 GPU 信号分开。 |
| [ioctl.c:2888:2](rounds/c27-native-create-enum-20261121-01.result.json) | 未知 | 已查询当前适配器，NativeGpuFenceSupported=0。缺少同一受支持的原生围栏对象与调用区间内的创建子类型与共享回执。 | 涉及原生围栏的创建子类型与共享。按本机能力决定是否能进入这条路径；与已观察到的普通监控围栏 GPU 信号分开。 |
| [ioctl.c:3243:2](rounds/c27-native-open-input-20261121-01.result.json) | 未知 | 已查询当前适配器，NativeGpuFenceSupported=0。缺少同一受支持的原生围栏对象与调用区间内的NT 打开参数回执。 | 涉及原生围栏的NT 打开参数。按本机能力决定是否能进入这条路径；与已观察到的普通监控围栏 GPU 信号分开。 |
| [ioctl.c:3302:2](rounds/c27-native-open-object-20261121-01.result.json) | 未知 | 已查询当前适配器，NativeGpuFenceSupported=0。缺少同一受支持的原生围栏对象与调用区间内的打开时的类型检查回执。 | 涉及原生围栏的打开时的类型检查。按本机能力决定是否能进入这条路径；与已观察到的普通监控围栏 GPU 信号分开。 |
| [ioctl.c:3310:2](rounds/c27-native-adapter-20261121-01.result.json) | 未知 | 已查询当前适配器，NativeGpuFenceSupported=0。缺少同一受支持的原生围栏对象与调用区间内的跨适配器约束回执。 | 涉及原生围栏的跨适配器约束。按本机能力决定是否能进入这条路径；与已观察到的普通监控围栏 GPU 信号分开。 |

写合并页的观测保留其实际页面保护。KMD 的 Cached 属性描述分配后备是否应按可缓存内存建立。[微软分配属性说明](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/ns-d3dkmddi-_dxgk_allocationinfoflags_wddm2_0)

本轮两个临时工作目录均已按归档哈希清理，任务和 ETW 会话均为 0。《仙剑四》保持暂缓，九份既有报告已逐字节核对。[完整记录](native-27.json)
