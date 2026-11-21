# Switch 实机与 WSL dxgkrnl 契约

本目录保留固定源码审阅和既有 Switch 回执。当前 Windows ARM64 本机验收、首次 Present 与 GPU 围栏进度见独立索引。[Windows 本机验收](../windows-native-graphics/index.json)

Gemini 4 Neon XHigh 审阅了 15 个文件的 452 条注释。主会话核对了源码位置、适用场景和实机回执。当前结论为 **11 条已满足、0 条已违反、441 条未知**。未知项中，19 条有原生 Windows 对照，298 条需要 WSL 来宾或宿主观测，124 条标记源码元信息。[完整契约表](contracts.json) · [覆盖与校验记录](audit.json) · [微软固定版本的源码目录](https://github.com/microsoft/WSL2-Linux-Kernel/tree/a989171929bed4ea681be8d77333845ce1c1732a/drivers/hv/dxgkrnl)

微软源码版本为 `a989171929bed4ea681be8d77333845ce1c1732a`。每个 C 注释以编译器识别的起始行和字节列编号，Kconfig 和 Makefile 按行首注释计数。许可证、标题和分隔行也保留编号，实机状态记为未知。WSL 来宾内部的链表、VMBus 消息和宿主回包需要对应平台上的观测。本版注释正文中 `TDR`、`reset`、`watchdog` 和 `timeout` 的独立词命中数依次为 0, 0, 0, 0。

对原生可比项，状态限定到条目所列的 Windows 用例。来宾锁、引用计数和协议字段需要同层记录。当前实机为 WDDM 2.6，原生围栏的 NT 句柄打开接口要求 WDDM 3.2。[微软接口要求](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmthk/nf-d3dkmthk-d3dkmtopennativefencefromnthandle)

## 原始 449 条缺口索引

索引按 `a73e3b76` 的原始顺序保留 124 条源码元信息、27 条原生可比项和 298 条 WSL 专属项。每条分列当时的缺口和最新处置，静态核对与 GPU 判定分别保存。已登记的 27 个 Switch 轮次直接引用。[449 条逐项缺口索引](gap-index.json)

## 27 条原生可比项实机验收

原清单中 27 条原生可比未知项已逐条预登记并在 GM20B 上处置，新增 8 条已满足，19 条保留未知。逐项回执给出公开调用、能力结果、所缺观测和对首次 Present 的意义。三次历史重启前，每次四个 UMD 映射回调都有后续成功分页检查，调用标记之后的故障阶段仍然未知。[27 项实机结果与首次呈现边界](native-27.md) · [原始身份、QPC 与归档哈希](native-27.json)

## 先前实机结果

- **已满足**。独立 x86 调用方与 Windows 内核在同一请求上都记录了 `STATUS_PENDING`（`0x103`），分页 fence 均为 7001。原游戏另有一份完整记录，其中 16 次驻留请求在内核返回该值。[本轮实机回执](rounds/pal4-residency-x86-20261121-01.result.json) · [仙剑四日志与源码核对](../PAL4_NATIVE_RESIDENCY_20261121.json)
- **未知**。原游戏三轮均记录到第七个纹理进入构造，随后重启。分配事件的完整时间区间缺失，这次调用中新建分配的归属仍然未知。[三轮游戏回执](../PAL4_TEXTURE_CONSTRUCTOR_BOUNDARY_20261121.json)
- **未知**。原游戏未命中预定检查点，所保留的 2 个上下文中有 2 个能关联到同进程的设备。[该轮上下文与检查点回执](rounds/pal4-preimage-context-20261121-01.result.json)
- **已满足**。原游戏命中主纹理的 Vulkan 图像创建前检查点，所保留的 2 个上下文中有 2 个能关联到同进程的设备。[该轮上下文与检查点回执](rounds/pal4-preimage-context-20261121-02.result.json)

- **已满足**。独立 x86 程序创建了一份普通分配和一份共享资源分配，各为 4 MiB。同一调用线程的设备、分配及资源句柄都与内核事件吻合。此前游戏第七张纹理的归属仍为未知。[原生创建计划](rounds/native-x86-allocation-owner-20261121-01.plan.json) · [用户态与内核逐次配对](rounds/native-x86-allocation-owner-20261121-01.result.json)

## 按源码文件查阅

契约表逐条给出微软源码链接、注释字节哈希、中文断言、判定和证据。下表链接指向相应文件的首条记录。

| 源文件 | 注释 | 已满足 | 已违反 | 未知 |
| --- | ---: | ---: | ---: | ---: |
| [Kconfig](contracts.json#L401) | 3 | 0 | 0 | 3 |
| [Makefile](contracts.json#L449) | 2 | 0 | 0 | 2 |
| [dxgadapter.c](contracts.json#L481) | 11 | 0 | 0 | 11 |
| [dxgkrnl.h](contracts.json#L657) | 99 | 5 | 0 | 94 |
| [dxgmodule.c](contracts.json#L2336) | 40 | 0 | 0 | 40 |
| [dxgprocess.c](contracts.json#L2976) | 6 | 0 | 0 | 6 |
| [dxgsyncfile.c](contracts.json#L3072) | 7 | 2 | 0 | 5 |
| [dxgsyncfile.h](contracts.json#L3200) | 3 | 0 | 0 | 3 |
| [dxgvmbus.c](contracts.json#L3248) | 70 | 0 | 0 | 70 |
| [dxgvmbus.h](contracts.json#L4394) | 62 | 0 | 0 | 62 |
| [hmgr.c](contracts.json#L5386) | 9 | 0 | 0 | 9 |
| [hmgr.h](contracts.json#L5530) | 4 | 0 | 0 | 4 |
| [ioctl.c](contracts.json#L5594) | 127 | 4 | 0 | 123 |
| [misc.c](contracts.json#L7664) | 2 | 0 | 0 | 2 |
| [misc.h](contracts.json#L7696) | 7 | 0 | 0 | 7 |

## 旧式窗口首次呈现前的围栏

重读原生窗口三次异常启动和一次正常翻转对照的原始 ETW，绘图线程在首次 `Present` 前发出了值为 1 的 GPU 围栏信号。三次异常启动中，Windows 调度器分别在调用前 360.492、1101.095 和 400.943 毫秒完成了该信号包。早期交接把这条回执解释为等待完成。Neon XHigh 和主会话复核后，`dxgkrnl.h:145:1` 和 `dxgsyncfile.c:406:2` 均保留“未知”。判断对象归属、销毁时 CPU 与 GPU 等待的行为，以及之后的重启起因，都还缺对应记录。[原始事件、匹配字段与审阅回执](../legacy-sequential-step4/historical-first-present-fence.json)

## 继续单项实机验收

开始前在故障表的 `dxgkrnl_contracts.rounds` 中登记一个 `contract_id`，保存本轮 `.plan.json` 的 SHA-256。计划写明责任方、触发条件、唯一待裁决的问题与判定所需的回执。启动器读回登记和哈希后才运行。[故障表](../open-faults.json)

结束后填写同一轮的 `.result.json`，把文件、SHA-256 和判定写回故障表。缺少目标事件、身份、时间配对或必要记录时填写未知。发生未解释重启时登记对应开放故障，保留中断前的事实。按不同进程、组件和启动分别说明证据能覆盖到哪一层。

在仓库根目录执行以下命令可核对目录覆盖、来源、固定版本证据指针和每轮的单条登记。

```sh
python3 tegra-x1-wddm/tools/check_dxgkrnl_contracts.py
```

另加 `--source-directory /path/to/drivers/hv/dxgkrnl` 可逐字节核对上述微软固定版本中的所有注释。

仓库的 Git 推送检查会读取故障表，并对契约表和关联实机记录执行同一校验。表内的未知项保留适用范围，候选晋升继续依据原有开放故障规则。
