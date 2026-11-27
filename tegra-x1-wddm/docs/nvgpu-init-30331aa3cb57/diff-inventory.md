# GM20B 初始化比对与下一批候选

原报告第 6 节的九张表共有 60 行，依次为 11、10、13、5、4、6、5、3、3 行。第 5 节的 PRIV station timeout 单列。按同一上游行为为一组，总计 61 组，包含已上机的 2 组。一次门控表或异常掩码可能包含多个寄存器。

剩余 59 组中，11 组需核实 Windows 适用条件，2 组缺历史固件来源绑定，46 组已有对应行为或属于不同配置。当前没有一组以未解决的本地测试失败为阻塞。优先次序衡量取证价值。

## 下一批

| 顺位 | 原表行 | 项目 | 卡点 | 下一项核对 |
|---:|---:|---|---|---|
| 1 | 130 | GR/FECS 异常掩码及 HWW 状态 | 适用条件存疑 | 先列出已读取的状态、NETB 写入的值和当前能留存的字段，再决定哪些错误在第一次提交后看不见。状态清除、掩码使能和 ISR 归属分别核对。 |
| 2 | 183 | PRI timeout 错误锁存 0x9084 / 0x9088 | 适用条件存疑 | 仅在 PBUS 报错时读取保存区，先建立不清现场的观测。上游的清零在 ISR 读完之后，不能放进冷启动。 |
| 3 | 113 | FIFO 引擎超时 0x2a0c | 适用条件存疑 | 核对 Windows 对引擎超时的所有者、调度错误上报与 TDR 的先后。已核实 PTIMER 软件换算，尚无两种超时同时启用的板端依据。 |
| 4 | 192 | PBDMA acquire / GR semaphore 错误处理 | 适用条件存疑 | 只追踪旧运行里的中止、队列 fence 和完成顺序。Windows 已有部分 W1C 和冷复位处理，Linux 的内存 fast-forward 不能证明 Windows 的 DMA 完成。 |
| 5 | 172 | MC stall/nonstall 中断路由 | 适用条件存疑 | 列清已有非阻塞中断和轮询覆盖，确认谁拥有 stall ISR，再评估遗漏。与 GR 错误来源同轮分析、实机观测分轮。 |
| 6 | 136 | 挂起 / 下电时 GR FIFO 和异常状态 | 适用条件存疑 | 先区分完整断电、局部 GR reset 和 WDDM TDR。原报告的下电等效不能外推到保留现场的恢复。 |
| 7 | 126 | GR 中断初始化及 NETB 非上下文表 | 来源未核实 | 本地 NETB 的哈希及 176 条非上下文写入已核。该历史 Linux 安装实际加载的固件版本和哈希未绑定，暂不把外部载荷差异转成写入。 |
| 8 | 129 | 纹理合并读取 0x419a3c | 适用条件存疑 | KMD NETB 的值是 0x181e。上游在重放前清 bit 2/4；缺的是中间顺序。先核对两边重放前后状态，不能当作最终值缺失。 |
| 9 | 98 | FB/GPC MMU debug 状态 | 适用条件存疑 | KMD 已把 FB 0x100cc4 镜像至 GPC 0x4188b0。先确认冷启和恢复时继承的 bit 16，再决定有无必要显式设置。 |
| 10 | 152 | PMU/ACR 安全启动和门控 | 来源未核实 | 宿主侧启动入口可查。历史固件内部是否做同等写入缺少匹配镜像证据。先补固件身份；Horizon 专用 FECS/winsys 规避项划掉。 |
| 11 | 164 | THERM 外部事件与渐进降频 | 适用条件存疑 | 先核对板上三路事件接线、当前温控所有者及触发状态。0x20798 已有暂时关闭再恢复的写入，禁止按整寄存器缺失处理。 |
| 12 | 166 | BUS/CHIPLET/PERF/PRIRING/THERM/XBAR 门控 | 适用条件存疑 | 上游表有 16 个已列出的 profile 地址，未确定本机启动后实际状态。保留现有强制运行策略，先查适用条件。 |
| 13 | 165 | ELCG 空闲滤波 0x20288 / 0x2028c 等 | 适用条件存疑 | Windows 对 GR 使用 RUN。先证实 FILTER 在实际模式里参与状态转换，再考虑设置 FECS/HUBMMU 默认滤波值。 |

## 已有实现和环境差异

| 原表行 | 子系统 | 地址或行为组 | 处置依据 |
|---:|---|---|---|
| 93 | FB/MMU | `0x00100800` | 活动 LTC 数目已写入 FB。 |
| 94 | FB/MMU | `0x00100ce4` | 非安全分支按条件执行；不改变熔丝或零售机安全策略。 |
| 95 | FB/MMU | `0x00100c80` (bit 11) | 页大小字段已有对应写入。 |
| 96 | FB/MMU | `0x00100c80` (bit 12) | KMD 支持非压缩 PTE，压缩策略不同。 |
| 97 | FB/MMU | `0x00100c10` / `0x00100cc8` / `0x00100ccc` | FB 调试缓冲及 flush 页已有相应编程。 |
| 99 | FB/MMU | `0x00100cb8` / `0x00100cbc` | TLB 无效化与触发已有实现。 |
| 100 | FB/MMU | `0x00070000` / `0x00070004` / `0x00070010` | 显存可见性和 L2 刷新已有实现。 |
| 101 | FB/MMU | `0x0007000c` | 无压缩标签资源，此清理路径不适用。 |
| 102 | FB/MMU | `0x00100cd0` / `0x00100cd4` | WPR/VPR 的宿主侧已走受保护读取流程。 |
| 103 | FB/MMU | `0x00100d14`, `0x00100c9c`（SLCG）；`0x00100d10/d30/d3c/d48/c98`（BLCG） | FB 门控使用已选强制运行 profile。 |
| 109 | FIFO/PBDMA/CE2 | `0x0000200` (PFIFO bit8) | PFIFO 复位已有实现。 |
| 110 | FIFO/PBDMA/CE2 | `0x00000204` (`MC_ENABLE_PB`) | 这是 KMD 额外显式使能 PBDMA。 |
| 111 | FIFO/PBDMA/CE2 | `0x000026ac` / `0x000026a4` | FIFO 门控采用强制运行策略。 |
| 112 | FIFO/PBDMA/CE2 | `0x00002a04` / `0x0004012c` | FB 和 PBDMA0 的超时字段已有实现。 |
| 114 | FIFO/PBDMA/CE2 | `0x0004013c` / `0x00040108` / `0x00040148` / `0x0004010c` / `0x0004014c` | PBDMA0 中断掩码和状态清除在 core/execute.c。 |
| 115 | FIFO/PBDMA/CE2 | `0x00002100` / `0x00002140` / `0x00002528` / `0x00002a00` | FIFO 本地中断在 core/execute.c，系统 IRQ 路由另见 MC 项。 |
| 116 | FIFO/PBDMA/CE2 | `0x00002270` / `0x00002274` / `0x00002630` / `0x00002634` / `0x00800000+8*i` / `0x00800004+8*i` | 已有单通道 runlist/CCSR；不运行 PMU ELPG 调度器。 |
| 117 | FIFO/PBDMA/CE2 | `0x00002a30+4*i` (`FIFO_TRIGGER_MMU_FAULT`) | Linux 按 TSG 注入恢复故障；WDDM 使用自身复位契约。 |
| 118 | FIFO/PBDMA/CE2 | `0x00106f28`（CE2 SLCG）+ `0x00000200` (CE2 reset) | CE2 复位已有实现，门控策略不同。 |
| 124 | GR | `0x00000200` (`0x18001000`) & `0x00400500` | GR 复位与 FIFO/semaphore access 已编程。 |
| 125 | GR | `gm20b_slcg_gr` (58 项) + `gm20b_blcg_gr` (61 项) | GR 门控地址表存在，选择的是强制运行值。 |
| 127 | GR | FECS/GPCCS 启动、`0x00409b00`、`0x00409840`/`0x00409500`/`0x00409504` | 宿主侧 FECS/GPCCS 启动存在；不透明固件内容见 PMU/ACR 待查项。 |
| 128 | GR | `0x00418880` / `0x00418890` / `0x00418894` / `0x004188b0` / `0x004188b4` / `0x004188b8` / `0x004188ac` | FB→GPC MMU 同步已有源码及数组写入。 |
| 131 | GR | `0x00419e44` (`SM_HWW_WARP_ESR_REPORT_MASK`) | 已有与 NVK 对齐的 warp ESR 策略；NETB 默认值有记录。 |
| 132 | GR | `0x00419f78` (`SM_DISP_CTRL`) | KMD 已显式处理 helper invocation。 |
| 133 | GR | `0x00405804..0x00405824` (GR DS ZBC) | 当前 KMD 无压缩 ZBC 路径。 |
| 134 | GR | `0x00503018`, ZCULL (`0x418980`, `0x500910/14/18`, `0x41bfd4`), FS/ROP (`0x406028+`, `0x405870+`, `0x418bb8/b08+`, `0x41bfd0/fe4/f00+`, `0x4078bc/80c+`, `0x4064d0+`, `0x405b00`, `0x408850`, `0x408958`, `0x408908`, `0x4041c4`, `0x405b60/ba0+`, `0x500c10/c30+`, `0x504088`, `0x504698`), Golden Capture (`0x404170`, `0x409614`, `0x40802c`, `0x404154`, `0x400200/04/08`, `0x404488/8c`, CB/Pagepool/Timeslice) | ZCULL、拓扑及上下文构造已有实现。 |
| 135 | GR | `0x00409500/04` (methods `0x30/0x31/0x32` ELPG FECS bind) | 没有 PMU ELPG 运行环境，Linux 绑定方法不适用。 |
| 142 | LTC | `0x0017e27c` / `0x0017e000` / `0x0017e318` / `0x0017e20c` | LTC 枚举及 FS 状态已有相应写入。 |
| 143 | LTC | `0x0017e050/35c` (SLCG) & `0x0017e030/040/3e0/3c8` (BLCG) | LTC 门控选择强制运行 profile。 |
| 144 | LTC | `0x0017e278` (`cbc_base`), `0x0017e26c/70/74` (`cbc_ctrl1/2/3`) | 无 CBC backing，不能写入压缩缓冲基址。 |
| 145 | LTC | `0x0017e338` / `0x0017e33c+4*i` / `0x0017e34c` (LTC ZBC) | 未启用 ZBC 压缩资源。 |
| 146 | LTC | `0x0017e2b0` (`l2_bypass_mode`) | 两边均未在本次启动链修改此旁路位。 |
| 153 | PMU/ACR | `0x0010a004` (`IRQSCLR`), `0x0010a014` (`IRQMCLR`), `0x0010a130` (`CPUCTL_ALIAS`), `0x0010a4a0/4b0/4c8/4cc` (CMDQ/MSGQ head/tail), DMEM cmdline args | PMU 固件引导及同步轮询已实现。 |
| 154 | PMU/ACR | `0x0010a010` (`IRQMSET`) / `0x0010a01c` (`IRQDEST`) | PMU IRQ 目的地属于 Linux 异步 RTOS 模式。 |
| 155 | PMU/ACR | `0x0010a6c0/e8` (`pg_idlefilth/ppuidlefilth`) + `_pginitseq_gm20b` (57 项) + `0x0010a504..10a9ec` (Perfmon 计数器 0/1/2/3/4/6) + `0x0010a488/48c/580+` (PMU Mutex) | 不运行 Linux PMU ELPG/Perfmon 服务。 |
| 161 | 时钟/THERM/门控 | `0x00137000..0x00137340` (`TRIM_SYS_GPCPLL_*`, `SEL_VCO`, `GPC2CLK_OUT`, `BYPASSCTRL`), `0x00132820` (`BCAST_GPCPLL_DVFS2`), `0x00021944/48` (`FUSE_CTRL_OPT_RAM_SVOP_*`), `0x00020160` (`THERM_CLK_SLOWDOWN`), `0x00020798` (`THERM_USE_A` save/clear/restore) | 本行是 PLL 基础设置总览；条件分频单列。 |
| 163 | 时钟/THERM/门控 | `0x0013701c` / `0x001328a0` (`NDIV_SLOWDOWN` / `NDIV_SLOWDOWN_DEBUG`) | KMD 使用 bypass 重锁，未采用动态 NDIV slide。 |
| 173 | MC/IRQ/PRIV/BUS | `0x0000020c` (`MC_ELPG_ENABLE`) | FB/XBAR/HUB 使能在 frontend。 |
| 174 | MC/IRQ/PRIV/BUS | `0x00001100` / `0x00001140` (`BUS_INTR_0` / `BUS_INTR_EN_0`) | PBUS 的使能在 frontend；报错时的保存区另列。 |
| 175 | MC/IRQ/PRIV/BUS | `0x00001704` (`BUS_BAR1_BLOCK`) | BAR1 绑定已有对应代码。 |
| 176 | MC/IRQ/PRIV/BUS | `0x0012004c` / `0x00122204` (`PRI_RINGMASTER_COMMAND` / `SYS_DECODE_CONFIG`) | 已启用 PRIV ring，这与站点 timeout 是不同写入。 |
| 182 | PTIMER | `0x00009080` (`timer_pri_timeout_r`) | 没有要移植的 PTIMER 校准写入。 |
| 184 | PTIMER | `0x00009400` / `0x00009410` (`timer_time_0/1_r`) | 高低计数器是只读时钟采样；软件换算及 Windows DDI 是另外的契约。 |
| 190 | Semaphore | `0x00400500` (bit16 `semaphore_access`) | GR semaphore access 已使能。 |
| 191 | Semaphore | `0x00070000` (`flush_fb_flush_r`) | FB flush 可见性屏障已有实现。 |

## 已上机

- PLL 分频过渡。PL=1 条件过渡已编入通过桌面验收的 KMD。高频分支由模型覆盖；手持实机本轮未越过 PL=1 门槛。
- SYS/GPC station timeout。原报告第 5 节单列。在 GR 设置阶段分别写 0x122354、0x128328 为 0x800。

## 下一次本机取证

91476→91557 的两份 ETW 仍缺 DMA 152586 的同序号 KMD 进入、返回及完成。预抢占 20571 的快照覆盖 DMA 152584。现行基线在启动 92083 复放同一签名空窗口，取得正常提交的 5063 进入、返回和实际完成，历史故障仍开放。[历史同包缺口](../windows-native-graphics/rounds/win-board-cdd-dwm-third-submit-kmd-20261121-01.result.json) · [现行基线同包回执](../windows-native-graphics/rounds/win-seq-new-baseline-profiler-20261121-01.result.json)

| 原表行 | 已核对的可观测范围 | 下一次单项取证 |
|---:|---|---|
| 130 | 现行工作线程在有效 MC 采样且 GR 供电位为真时，采集 GR 中断、FECS 错误邮箱和主机中断状态。ExecutionState 的 `gr_valid` 区分实读零与未采样。91476 的 DMA 152558 与 92083 的 DMA 9651 各有一份有效快照，三个字段均为零。两张快照只覆盖各自提交；152584 的翻页态快照没有 `gr_valid`。NETB 的 0x400130/134/138 初值为零，0x419e44/4c 有各自的掩码写入。Linux 另有 FECS 状态处理后 W1C、SM global ESR 回写和 warp ESR 清零。 | 用现有 ETW 同时收集故障来源、快照有效位、阶段和 DMA 身份。逐 SM 的 HWW ESR、PC 及当时生效的掩码仍须在持有设备上下文且供电有效时由驱动留下观测。保留现有中断路由和清除行为。 |
| 183 | Windows 前端快照保存 PBUS 0x1100、PRI 汇总，并按 SYS 状态位取 0x122120/0x12212c。Linux 在 PBUS 的 PRI 条件命中后先读 0x9084/0x9088，再清锁存。当前 Windows ExecutionState 没有这两个 PTIMER 保存字，已保存的 DXG 同包回执也没有。 | 只在同包出现 PBUS/PRI 相应标志时评估由现有设备所有者增加一次只读锁存快照。分别保留触发、保存字有效位和取样时序。 |
| 113 | Linux GM20B 走 FIFO init 的 PTIMER 缩放分支，并将 FIFO 调度错误与 engine/TSG 或 channel 归属结合。现行 Windows 运行路径能记录 FIFO 汇总，诊断快照另有 0x254c 调度错误值；两者还需与 Windows 调度器的 preemption 和 TDR 边界配对。现有两轮缺同包的 0x2a0c、0x254c 及关联 TDR 回执。 | 先观察现有事件中硬件调度错误、Windows preemption 与 `DxgkDdiResetFromTimeout` 的顺序和同一包归属。暂无并行使能硬件超时与修改 TDR 的实机依据。 |

源码锚定在已选的 `7f626083` 和 Switchroot `1ae0167d`。前三项仍待观测。其余十项沿原队列。 [Windows 提交轮询](https://github.com/ruje0504-2/mesa-switch-woa/blob/7f626083c90f73a6aa9279fafdf40e2777c0e6c6/tegra-x1-wddm/windows/submit_worker.c#L27) · [Windows 前端快照](https://github.com/ruje0504-2/mesa-switch-woa/blob/7f626083c90f73a6aa9279fafdf40e2777c0e6c6/tegra-x1-wddm/platform/t214_frontend.c#L41) · [Switchroot PRI 锁存](https://github.com/CTCaer/switch-l4t-kernel-nvgpu/blob/1ae0167d360287ca78f5a2572f0de42594140312/drivers/gpu/nvgpu/common/ptimer/ptimer_gk20a.c#L31) · [Windows TDR 契约](https://learn.microsoft.com/windows-hardware/drivers/display/timeout-detection-and-recovery)

后续每轮只验证表内一个问题，保留相同工作负载、设备身份和已加载组件哈希。对 PL=1 过渡、长时间高热、整机重启的结论均需独立实机证据。

原报告 SHA-256 为 `24df8481d07cc2b2b340cc1821814470ccffb320f03a9ce95719de51c6c02925`。原表行号对应保留的 Gemini 返回正文。核验了地址和调用点的寄存器在[源码对照清单](../windows-native-graphics/gm20b-init-diff-9ef86d7e.json)中。
