# dxgkrnl普通桌面旋转交接核对

## 后续匹配运行时 — 2026-11-16 05:06 UTC

已继续核对实机 dwmcore/d3d11/dxgi。DWM 的单平面 Present→MPO 转换另检查 DXGI1.4
`PresentMultiplaneOverlay1` 入口；这与旧 UMD 已经到达 KMD MPO3 并不矛盾。
版本、RVA、契约差异、代码和原生 ARM64 控制结果见
[UMD MPO1 核对](UMD_MPO1_CONTRACT_20261116.md)。现代入口仍未协商，不宣称列扫描通过。

## 后续结论修正 — 2026-11-16 UMD 工作轮

PresentId=241 已可追溯到 `panel_scanout.c::set_primary_mpo`：其他普通主表面入口将
PresentId置零，锁存读回只搬运已有值。因此，旧D3D10 UMD已经可以经runtime到达
KMD MPO3，现代UMD DDI未注册不能解释成“系统到不了MPO3”。旧 `runtime_primary_create`
也已有1280×720/pitch5120创建路径。本轮实际被锁存的却是720×1280/pitch2880/identity；
下一步需要核对横向primary的请求、创建/拒绝、CheckMPO及plane0绑定，而不是强写寄存器。
未证明“标准SHADOWSURFACE”就是本轮来源，也未证明null-context Present分支在本轮执行。

用户取消物理强开试验，要求先补完UMD复制、失败清理和生命周期；之后授权自主实机测试。
现代typed table完整性属于独立实现工作，仍未协商。保留Noble的硬件能力证据与用户本轮
短时物理横屏照片，但不将二者作为Windows列扫描闭环验收。

本轮针对用户要求的一次真实列扫描四色测试，继续查普通DWM路径。没有修改系统内核，
也没有将尚未接入的现代UMD函数表整批启用。源码仍在tgd3d-vulkan，当前设备已经恢复
E95/4c8/95b与原电源、风扇、同步组合。

## 对应版本

从实机再次读回 `dxgkrnl.sys` 为10.0.22621.2792，SHA256：
`99d425e1c997eae32909fa4f37d6c8e2e846a5374c602295546c8be2547aa5e6`。
与本地取回的ARM64二进制一致。PE CodeView GUID为
`797F457A-6488-C67C-8E55-B0F3E771DC32`、age1；公开PDB同GUID、DBI age1。
公开PDB的Info stream age为2，单独记录这一差异，没有把Info age当作DBI age。
PDB SHA256为 `7f0a811d1c74569f9dbe428cc2d73355beb4e0864b7b77c81e1bbf3540fa74e9`。

反汇编与身份清单保存在OUT `column-scan-history-audit-20261116/kernel-audit/`。
函数入口按匹配的公开符号标记；不采用objdump在缺少私有符号时显示的相邻导出名作为真实函数名。
拆分的冷分支需另行跟进，单个相邻符号区间不等于全部控制流。

## 已核对的分支

| 入口 | RVA | 实际静态行为及限制 |
|---|---:|---|
| DXGDEVICE::SetDisplayMode | 0x1493C8 | 0x1494C8调用DescribeAllocation；先比较模式宽高，再核对要求的Rotation。当前候选已区分模式与存储描述。本轮没有该拒绝分支的到达证据。 |
| DMMVIDPNPRESENTPATH::UpdateActiveVidPnPresentPath | 0x3AF958 | 序列化路径后，在0x3AFA58调用GetContentRotationHw，再把结果写进传给KMD的路径结构。逻辑桌面角度不能直接当作KMD实际收到的角度。 |
| DMMVIDPNPRESENTPATH::GetContentRotationHw | 0x130CB0 | 常规有效角度下检查目标对象的字节标志；标志置位则返回identity。不能只看到这个分支就认定实机已经走到这里。 |
| ADAPTER_DISPLAY::DdiSetVidPnSourceVisibility | 0xF5200 | 持有DdiSync调用miniport；成功后更新系统侧可见性记录。意外返回值走内部诊断分支；该分支本身不能证明本轮发生了bugcheck。当前驱动不可把锁存超时伪装成功。 |
| DdiSetVidPnSourceAddress / MPO3 | 0x4B9E8 / 0x1DCB0 | 均有独立miniport调用入口与同步边界。仅注册MPO KMD入口不证明DWM实际交来横向主表面。 |
| DdiCommitVidPn / DdiSetTimingsFromVidPn | 0x2A6F48 / 0x170B20 | 分别保留，不能把恢复方向中未返回的一个SDK调用直接归因给其中某一个DDI。 |
| DxgNotifyInterruptCB | 0x1D4D0 | 通知入口存在；应继续使用真实锁存/中断完成，不能添加无条件HsyncFlipCompletion。 |

GetContentRotationHw使用的目标标志来自监视器连接处理；后者会调用
`IsVirtualizationDisabledForTarget`。该函数同时考虑适配器开关与监视器虚拟模式禁用状态。
适配器初始化在所审二进制中读取GraphicsDrivers下的 `ForceEnableDWMClone`；
另一兼容分支涉及 `EnableVirtualTopologySupport`。实机只读查询显示这两个值都不存在。
本轮没有新增、删除或修改这些注册表值。

微软[DriverRotation定义](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmdt/ns-d3dkmdt-_d3dkmdt_vidpn_hw_capability)
明确区分图形引擎预旋转与显示管线直接旋转；当前DC路线使用0，不能把它盲目改成1。
微软的[活动路径旋转要求](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/optimized-screen-rotation-support)
要求miniport在相应回调中落实旋转。当前仅记录PathRotation、等待未来primary的设计仍需补齐实际交接。

## 本机只读CCD结果

新只读工具 `tools/display_virtualization.cpp` 使用QDC_VIRTUAL_MODE_AWARE及官方
DISPLAYCONFIG_DEVICE_INFO_GET_SUPPORT_VIRTUAL_RESOLUTION。ARM64构建通过，原生执行于
01:53:35 UTC，查询返回ERROR_NOT_SUPPORTED(50)，没有改变任何显示或虚拟化设置。
原始路径flags=1，source/target索引字段需要依照该次返回的路径标志解释；没有用越界索引读取模式数组。
本轮没有由此取得可用于切换的虚拟模式状态，因此不调用setter，不采用私有注册表绕过。

官方[虚拟模式状态结构](https://learn.microsoft.com/en-us/windows/win32/api/wingdi/ns-wingdi-displayconfig_support_virtual_resolution)
与[CCD索引规则](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/relationship-of-mode-information-to-path-information)
是工具依据。它们描述的是公共接口，不保证所有当前驱动/目标都支持该查询。

## 对下一次试验的约束

01:34 UTC用户确认的短时横屏，状态记录仍为720×1280/pitch2880/rotation0的逐行扫描。
图案退出后开始恢复原竖屏，恢复调用未返回即重启；不能说真实列扫描已测试或失败。

真正的下一轮需由普通DWM交来可追溯的1280×720/pitch5120主表面，并由对应VidPN/MPO
角度完成DC列扫描锁存。先解决入口与表面交接，再显示同样四色图案；保留当前电源、
面板时序和双向GPU Fence。检查新旧backing退休与返回原方向，不重放原逐行组合代替修复。
现代UMD类型桥仍是本地未协商工作，其完整性与实际需要须分别验证，不因有一个MPO函数就虚报接口。

再次核对了[PRIMARY_NONPREROTATED定义](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/dxgiddi/ns-dxgiddi-dxgi_ddi_primary_desc)：
它确实表示identity存储，应用负责方向。当前该位的解释没有反转，不能通过颠倒含义强制列扫描。
微软的[窗口模式说明](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/windowed-mode-behavior)
也区分应用窗口与DWM/miniport负责的旋转；不能把单个窗口的资源描述直接当作最终扫描表面。

尝试按用户授权调用VPS Claude作独立契约复核，SSH成功但Claude返回未登录，审查没有执行。
未修改其配置；回复保存在 `vps-column-review-response.json`。本报告为主窗口核对，未宣称独立审核通过。

## 普通DWM接口接入进度 — 2026-11-16 02:35 UTC

用户要求再做一次真实列扫描四色测试。该次重测尚未发起：已安装的D3D10.0接口不能调用
本地尚未注册的Present1/MPO原型；直接重放上轮包仍不能证明DC列扫描。本轮没有安装候选、
改变方向或重启设备。02:21:39 UTC只读预检为Code0、720×1280、DWM PID1392，
启动时间仍约01:39:17 UTC；上一轮完整组件恢复校验继续保留，未用本次简要预检代替。

本地补入 `tgd3d_clear.cpp::ClearView11`，处理当前2D RTV的整面/矩形清除、数组各层、
裁剪和空矩形、整数NaN/INF限幅及XRGB/A8映射。矩形路径以LOAD/STORE保留区域外数据，
通过既有命令屏障排序，并沿真实提交序列退休image/view；不新增Present前CPU等待或完成信号。
不支持的view类型明确失败，尚未因此扩大格式或Feature Level声明。
依据为微软[ClearView DDI](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/nc-d3d10umddi-pfnd3d11_1ddi_clearview)
（页面2022-01-04版；读取2026-11-16）。CPU替身验证的是命令、参数、顺序及生命周期，
不是实际像素或面板显示。

同时新增真正的 `D3DWDDM1_3DDI_DEVICEFUNCS` 类型桥。与11.1表的公共成员逐项按类型赋值；
Relocate和新增Flags参数的多采样查询使用各自签名，不把短表强转成长表，不忽略tiled请求标志。
当前WDK/SDK10.0.26100.0头文件显示：Win8 DXGI1.2也有MPO接口，但其plane属性没有1.3
新增的ClipRect，且过滤器字段与DirtyRects占用同一位置。不能因为11.1表有MPO字段，就把
当前1.3属性直接交给旧ABI。未声明的tiled、marker等可选功能保持未启用。

两个架构构建通过；x64/Wine完整CPU控制套件通过，包括函数表边界、各层清屏、65个矩形分批、
错误路径和在途资源退休。ARM64本轮仅编译，没有GPU离屏或原生桌面执行。
完整结果及冻结二进制在OUT `ordinary-mpo-ddi-20261116/`。

**接入尚未完成。** `GetSupportedVersions`/`CreateDevice`仍只协商原D3D10接口。
启用前还需闭合Offer/Reclaim对runtime/NVK两种allocation的所有权及驻留、
其余必需现代操作与能力声明；不能将Offer/Reclaim写成无条件成功，也不能把同资源重叠复制
直接变成非法的Vulkan重叠copy。MPO原型的CPU通过不能省略这些调用边界。
该路线是补齐已存在的呈现原型，不是已经证明旧接口绝无其他合法列扫描路径。
Gemini独立审核仍待既定额度恢复；本轮没有重复请求或声称审查通过。

### 后续本地修正 — 02:44 UTC

`Blt1`现已按[微软源子矩形定义](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/dxgiddi/ns-dxgiddi-dxgi_ddi_arg_blt1)
接到已有复制路径：identity使用准确源坐标；明确要求旋转时，使用独立blit pipeline layout
和16字节UV参数映射源子区域，旧Blt默认全源。顶点/片段shader object与绑定layout的push范围
一致，符合[Vulkan shader-object布局兼容规则](https://docs.vulkan.org/refpages/latest/refpages/source/VkShaderCreateInfoEXT.html)。
这只处理runtime要求的复制，MPO列扫描呈现不走该shader。没有把GPU预旋转改成默认路线。
新SPIR-V经glslang和spirv-val通过；CPU替身覆盖四向、非零mip/layer、源子矩形及越界拒绝。
尚无新shader的NAK实机执行或像素验收。

另发现 `CreateDevice` 原先用最新SDK的 `sizeof(D3DDDI_DEVICECALLBACKS)` 整表读取runtime指针。
源表按较早接口提供时，这会读到不属于该ABI的尾部；本轮没有证明历史实机在该处发生异常。
已改为逐项读取实际使用的20个WDDM2及更早回调，未使用的新尾部保持0；core-layer只保留实际
使用的SetError回调。保护页控制将源表截在最后一个必需字段之后，验证没有读过边界。
后续接Offer/Reclaim时须同时明确新增callback的版本/边界，不重新改成整表复制。

包含上述改动的ARM64/x64构建和x64/Wine控制套件于02:44:37 UTC通过。
最新ARM64 UMD SHA256为
`1586bdf4a0b348abcfd7185b0c391dfecaafefb4cabda066331fa31c21eead85`。
结果见 `ordinary-mpo-ddi-20261116/callback-cpu-controls.json`，二进制为
`callback-validated-arm64/`。仍未安装、未启用现代DDI协商，真正列扫描重测继续标记为未执行。

## 资源驻留与 MMIO Present 边界 — 2026-11-16 03:20 UTC

本地现代 DDI 资源路径增加 runtime 所有的私有 image/buffer allocation，复用既有
NVK 导入而不修改 ICD；每个可让出的缓冲区独占 allocation，Offer 前提交并排入 runtime
GPU wait，Reclaim 使用真实 paging fence，CPU 地址在回收后重新映射。分页完成不推进
render 完成值，在途 backing 仍按 render 序列退休。旧 D3D10 路径不切换至该分配方式。
ARM64/x64 构建、x64/Wine 控制套件通过；没有现代 DDI 原生执行证据。
冻结结果为 OUT `runtime-offer-reclaim-20261116/checkpoint.json`，其 UMD 未部署。
现代接口仍未协商，尚有其余必需操作与能力工作；不能据此宣称 DX11 功能完整。

依据匹配实机 22621.2792 的内核，新增确认：

- `ADAPTER_DISPLAY::Initialize` RVA 0x186BC0 在 0x186C40 查询 type16，即
  `DXGKQAITYPE_DISPLAY_DRIVERCAPS_EXTENSION`，输出位1正是 VirtualModeSupport。
  0x187080 取该位作为后续默认值。`OnMonitorConnectionChanged` 0x19FEEC 将
  IsVirtualizationDisabled 的结果取反写入 target+0x197。因此“未声明 VirtualModeSupport”
  本身不能证明当前旋转被强制 identity；仍需区分监视器分支和实际调用。
- `DXGCONTEXT::PresentMultiPlaneOverlay` RVA 0x309218 将旧 MPO 参数转换后，在
  0x3094E8 调用 MPO3；属性和调度仍由系统接管。
- `PresentMultiPlaneOverlay3` 0x114124–0x11414C 构造普通或 MPO 的 Flip/NoWait 标志。
  `SubmitPresentMultiPlaneOverlays3` 0x10E294 传 NULL context，0x10E2AC 调用
  `ADAPTER_RENDER::DdiPresent`；后者在 0x10D5E8 把同一 context 原样交 miniport。
  原 `TgGpuDdiPresent` 在所有操作前拒绝 NULL，故该合法 MMIO 分支会被误拒绝。

修正只将空 context 校验移至无 DMA MMIO 翻转之后；Blt/Rotate、未知标志、带 DMA 或
Multipass 的请求没有放宽。无 DMA 返回不写寄存器、不报完成；后续 panel DDI 继续验证
allocation、驻留、布局以及实际锁存。Native ARM64 CPU fixture 验证 NULL context、
MPO 与普通 Flip/NoWait，以及非法 Rotate/空参数边界。该缺陷是静态证据，**尚未证明
上一轮实际到达它，也不认定它就是之前自动重启根因**。

相关公开契约：[DxgkDdiPresent 的 MMIO 翻转](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/nc-d3dkmddi-dxgkddi_present)、
[显示扩展能力](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/ns-d3dkmddi-_dxgk_display_drivercaps_extension)。
空 context 的具体调用证据来自上述匹配二进制，不将文档未明确说明的部分伪称为文档原文。
反汇编新增输出在 OUT `runtime-offer-reclaim-20261116/kernel-path/`。

本轮最小实机候选 KMD SHA256：
`102218b0d69af15e1259a9f4047dec049eb8a61228b10eaf3551898cea8172b7`。
沿用上次候选 UMD `379e521d1d6d82c2eb6d8ef0be179c1bd1270d07965b70ffcf7a4d204585649d`
及 ICD `95b97df4e83a2a90315a8784d83e75771021093dec34f98362b584d1f546bdff`。
签名/catalog 13 项原生检查通过。03:19:31 UTC 安装前完整11组件基线一致，启动时间仍
01:39:17 UTC。初次 schema 包装器漏传 Directory 参数，未安装；补传参数后检查通过。
候选与恢复清单在 OUT `mpo-null-context-20261116/candidate-package.json`。

此次不先假定必须启用整套现代 UMD 才可能进入列扫描：用定位到的最小 KMD 修正作一次
普通 DWM 入口验证，入横屏后先读取真实布局；若还是逐行，不再用逐行图案作列扫描证据。
入口／停留和恢复分开，避免上次恢复请求后重启掩盖具体阶段。未提升日常基线。
Gemini审核待已记录额度恢复，未重试或声称通过；用户此前已授权审核返回前安装测试。

### 本轮实机入口结果 — 03:25:55–03:26:02 UTC

测试已实际执行一次，没有复测。候选启用完成于03:24:57，DWM PID1416实际加载上述379e/95b组合，
KMD为102218b0，GPU桥接开启。启动时间03:21:31，LUID `00000000000c30ed`。
普通显示配置切换返回0，逻辑1280×720/orientation1；严格验证1610后采用工具原有允许调整配置，
source mode与physical signal均720×1280。这些API结果不等于硬件列扫描通过。

切换前后面板读回均成功，关键值：

| 字段 | 切换前 | 横屏入口 |
|---|---:|---:|
| VidPN path rotation | 1（identity） | 2（90°） |
| 实际存储宽×高/pitch | 720×1280 / 2880 | 720×1280 / 2880 |
| DC rotation / column bit | 0 / 0 | 0 / 0 |
| source_visible / target_active | 1 / 1 | 1 / 1 |
| WIN_OPTIONS | 0x40000000 | 0x40000000 |
| 地址/已显示地址 | 0xFC276000 | 0xFBB6E000 |
| 待锁存地址 | 0 | 0 |
| displayed_present | 209 | 241 |
| underflow寄存器读值 | 11 | 11 |

地址与已显示地址在各次采样中相等，displayed_present序号由209推进到241；不能由此换算为实际显示32帧，
也不把underflow=11解释为新增11次故障。
本轮确实把90°请求交到KMD，显隐开启且新地址完成锁存，但依然是竖向backing和逐行扫描；本轮未做预旋转内容的像素正确性验证。
因此入口失败，**四色窗口没有启动、像素/实屏列扫描均未通过**。截至03:27:15恢复前，
启动时间没有改变，测试前后DWM PID相同，没有观测到自行重启。

此结果不能证明所有同步都正确，也不能证明MPO空上下文分支本轮被调用；它证实该最小修正
不足以使普通DWM交来列扫描主表面。下一步继续解决普通DWM的呈现接口与横向primary交接，
不能强行把当前720×1280 backing按5120 pitch扫描，也不通过翻转私有内核开关绕过契约。
恢复单独于03:27:23启动，目标是本轮安装前e952/4c8/95b及原电源设置，恢复结果另记。
原始记录已经取回OUT `mpo-null-context-20261116/device-results/`，入口摘要为`trial-result.json`。

### 用户实屏补充

用户随后发来本轮实机照片并确认“看着挺正常的”。照片中手持横屏的桌面、图标、任务栏
和Pacecar正常显示，任务栏为11:26，与本轮UTC03:26窗口相符。此证据将**短时横屏实屏
显示**记为成功；它没有改变寄存器证明的逐行扫描路径，也不证明长时间交互稳定。
四色专用窗口仍未运行，列扫描阶段仍未通过。不能把“列扫描入口未命中”泛化为“横屏没上屏”。
照片完整SHA256及用户原话见 OUT `mpo-null-context-20261116/physical-observation.json`。
恢复任务于用户照片到达前已启动，本轮候选和实屏证据完整保留，未推广为列扫描基线。

恢复最终核对于 2026-11-16T03:33:51.0965520Z 通过：11文件、Code0、原UMD/ICD、原方向/60Hz、
Handheld/85%/风扇策略及默认登录任务一致。无列扫描基线提升，活动目标继续。

清理于2026-11-16T03:35:40.7254571Z完成：6个任务、21个临时路径，
探针已移除。首次SSH客户端超时，之后只读结果确认完成，未重复清理或重启。
