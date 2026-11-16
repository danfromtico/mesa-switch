# Runtime 拥有的 NVK 提交：真实前提确认，候选撤回

本轮仅处理现代 UMD 上下文调整引入的回归；旧 D3D10 UMD 已可经 runtime 到达 KMD MPO3。
现代接口不是 DXVK 的前提，也未证明是 DC 列扫描必需。当前默认协商已恢复原三项 D3D10
接口，不继续扩建现代接口。横屏、四方向及 vkcube 均没有新增验收。

## 组件和源码身份

| 状态/组件 | 源码身份 | 完整 SHA256 |
|---|---|---|
| 当前/恢复 UMD | c1f25986eee51c0ee01088c3ecb3c9508dea2479；冻结文件与提交一致 | `5b66bbec86af299941812e8376cc1ac4a5630385828c895b7a52ec96d5c5ad7a` |
| 保留 KMD | c0c6647cec87fcc8d40951d8040ced5d1bbd5f2f + emc-6942cee5eb41547e40ebada89a5a8e976caa09985fda3fee509d2f2451bd0852；见原组件清单 | `e95287cbeb1d2c325baacb9685affd1d24783b22ecadc1801ba1dffcfe7cc25e` |
| 当前/恢复 ICD | c0c6647cec87fcc8d40951d8040ced5d1bbd5f2f；见原组件清单 | `95b97df4e83a2a90315a8784d83e75771021093dec34f98362b584d1f546bdff` |
| 已撤回候选 UMD | a8bf5de59268f95423375058c251544eb7130ca1 + 本轮冻结 diff | `f8ed1d8938f8d490fac84762234e3e0a0671d79ba018fa245190ad60939f9016` |
| 已撤回候选 ICD | 同一冻结源树，含继承的未提交改动 | `a488ff8f6f3b5bb22c86715d8c2ca1d0265fab48542956104ba46c2d406a5ed6` |

候选源码 diff SHA256：`4862b3bb748ad958744c21bd3f7d557221beccf1351bd681a338e33be71c784f`。
逐文件源码清单 SHA256：`1fe51ac66096daec56a5d97cb95b63a1f9126d25d02c819d10c2e0875613dbc6`。
源码、构建配置/日志、ARM64产物、控制、ETL/JSONL和完整恢复记录在 OUT
`runtime-submit-owner-20261116/`。候选仍含现代声明；最终保留源码撤下该声明，二者不能混作
同一个已测试产物。没有重建、更换KMD或提升显示基线。原组件来源清单为OUT
`modern-ddi-resources-20261116/active-trial-components.json`；其旧UMD字段由c1f/5b记录补正。

## 范围及契约

普通displayable allocation不能假定具备NtSecuritySharing或AllocateCB的kernel resource回写。
保留v1/v2内存owner兼容性，增加v3同runtime device引用：WrittenPrimaries使用原allocation，
真实NVK命令经runtime拥有的context和SubmitCommandCb提交，不使用空写标记、私有对象布局
或虚构句柄。ICD必须明确接受配套私有传输；提交保持原DDI线程，同context调用串行。
Fence导入保留同device token引用，成功提交不改完成值；超时、设备移除及销毁失败向上传播。

原NVK allocator device的销毁不再能证明runtime context已停止。因此，释放GPU存储前须
成功销毁全部runtime提交context；失败保留存储及句柄到进程清理，不提前回收在途资源。
正常析构和构造失败清理均接入此边界。依据微软
[DestroyContext callback](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dumddi/nc-d3dumddi-pfnd3dddi_destroycontextcb)、
[callback线程要求](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/changes-from-direct3d-10)及
[SubmitCommand/WrittenPrimaries](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dumddi/ns-d3dumddi-_d3dddicb_submitcommand)
（读取2026-11-16）。这不声明完整D3D、可选WDDM或更高Feature Level。

实际系统文件09:32读取：dxgkrnl.sys/dxgmms2.sys均10.0.22621.2792，分别为
`99d425e1c997eae32909fa4f37d6c8e2e846a5374c602295546c8be2547aa5e6`、
`94fa72e5cee778b7cd12bedf13550a78414231df8612547c74f0bc3ed11a62c5`；
d3d11.dll 10.0.22621.2715为
`ccc352f7097d0320368000dbe9ff92772d8fc909185d95c8a0753cb8f115f880`。
PDB身份及age差异沿用[匹配内核审计](WDDM26_CONTEXT_FENCE_AUDIT_20261116.md)与
[资源契约](DISPLAYABLE_RESOURCE_CONTRACT_20261116.md)，本轮系统哈希匹配。
SubmitCommandCB、GPU wait/signal适配反汇编另存本轮OUT。Noble列扫描证据仅证明同机硬件
可行，未作为Windows验收；没有改面板/DSI时序、显隐、列扫描布局或电源策略。

## 实际结果（UTC）

- 09:32:39，三项原生ARM64 CPU控制退出0；x64控制和配套ARM64构建通过。
  控制验证owner/上下文/Fence/清理/真实队列函数的参数传递，底层runtime/KMT仍为替身，未执行GPU。
  新NVK控制初版的空pdev与内部map flags错误属于测试夹具问题，修正后通过，原日志保留。
- 09:33仅派发任务后设备离线。用户确认候选未执行、因电量耗尽重启；此事件不计驱动失败。
  09:46核对原11文件/Code0；09:49:22明确外接电源Online/Charging后继续。
- 09:50直接Stage完成并保存收据，发出一次候选加载重启。没有切方向。
- 实际ETW的DWM PID1388成功创建24个runtime提交context（copy=4、graphics=7）；
  12次7508字节NVK初始化命令由真实SubmitCommandCb接受，WrittenPrimaries均为0。
  这证明真实callback/context入口可用，**不证明GPU完成、显示写列表或呈现通过**。
- 同一DWM进程记录12次成功设备创建，随后11次在冻结版`tgd3d_cmd.cpp:54`的完成查询路径
  报`0x887a0005`，继而资源清理报`0x88760870`。20次context销毁返回成功。
  没有获得成功Present证据；事件10是SetDisplayMode，不能当Present。
  用户提出黑屏后立即停止扩大测试并恢复；属于切方向前的新接口回归，不属于列扫描。
- 采集到的DWM事件跨度25.08秒，不是成功运行时长。JSONL末行截断，后段ETW曾报告至少
  92145个事件丢失，不能据缺失事件作全程否定或完整清理结论。原29,097,984字节ETL与JSONL均已取回。
- 09:55:06发出一次恢复重启；09:57首读Code43/无UMD处于登录启用过程中，未据此再次启用。
  10:00:04确认Code0、DWM1352加载原5b/95b、11项文件完整匹配、默认任务Ready；
  Handheld、85%、fan1、EMC204/1331.2/1600/2500及当前电源设置保留。
- 10:05:40清理临时trace任务、输入目录、失败安装目录和上传包，DWM1352/Code0不变。
  原活动包及恢复备份保留，无清理重启。恢复后的物理画面未另行验收。

## 后续

现代入口保持关闭，保留此次可复用修正；不继续扩建或重复启动相同失败组合。
回到当前5b/E95/95b普通DWM路径，直接追踪1280×720主表面的请求/选择、VidPN/MPO方向
和DC列扫描锁存。尚缺横屏、四方向、必要交互和Win32 vkcube验收。
Gemini4 Carbon High待2026-11-20T23:53:18Z额度恢复，本轮未重试、未声称独立审核通过。
Gemini的DXVK游戏适配/SC7与DeepSeek的NVDEC/D3D11VA仍是外部未合入工作，未重复开发。
