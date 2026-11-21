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

沿用 91476→91557 两份 ETW。首轮只问 Windows 调度器的 DMA 152586 是否有同序号的 KMD 进入回执；先对已有覆盖和事件字段做校验，预抢占 20571 与已完成 fence 152584 均不可顶替。GR 或 PRI 采集只用已归属的快照和非破坏性读取，并标明数据来自哪个阶段。读取 PRI 保存区与 W1C/清零分轮评估。既有采集不能量出状态时记录为未知。

后续每轮只验证表内一个问题，保留相同工作负载、设备身份和已加载组件哈希。对 PL=1 过渡、长时间高热、整机重启的结论均需独立实机证据。

原报告 SHA-256 为 `24df8481d07cc2b2b340cc1821814470ccffb320f03a9ce95719de51c6c02925`。原表行号对应保留的 Gemini 返回正文。核验了地址和调用点的寄存器在[源码对照清单](../windows-native-graphics/gm20b-init-diff-9ef86d7e.json)中。
