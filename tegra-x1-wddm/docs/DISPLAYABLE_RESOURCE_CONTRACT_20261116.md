# Displayable资源、提交device与WrittenPrimaries

最初后端基于`0f1760d79ade28daaa8c8a7d01651f83c0e9d75a`，已提交为`f01448d0`。
后续接通UMD写入跟踪、NVK引用和队列列表，构建及CPU控制通过；现代接口仍关闭。
尚未证明真实runtime所有呈现资源可导出，没有安装新UMD/ICD/KMD或改变显示方向。
列扫描仍是目标路线，Noble的物理布局证据沿用前轮，不采用GPU预旋转或强开寄存器替代。

## 已确认的契约缺口

旧记录的32×32、BindFlags=0xa8进入了legacy `create_resource`内部的事件8，因此已越过现代
描述符和TextureLayout适配器。它在VkCreateImage之前被拒绝。记录不含完整format/misc/sample描述，
不能仅凭尺寸认定哪一个具体拒绝条件在本次执行。

静态核对发现：所有普通、线性共享及tiled共享路径的misc白名单都缺少
`D3DWDDM2_0DDI_RESOURCE_MISC_DISPLAYABLE_SURFACE`。它是WDDM2的呈现表面标志；
对应写入还必须列入`WrittenPrimaries`，并不等价于有pPrimaryDesc或当前正在扫描。
NVK现有`tg_kmt_submit`未填此列表。因此只去掉白名单检查仍不满足完整契约。

匹配d3d11.dll 10.0.22621.2715（SHA256
`ccc352f7097d0320368000dbe9ff92772d8fc909185d95c8a0753cb8f115f880`）的
CSafeDDIResource Texture2D构造在RVA0x139b0–0x139d0检查共享创建描述和primary指针，
并将0x20000写入DDI MiscFlags。这说明普通呈现表面可以得到该位，不是驱动可以忽略的私有输入。
此外，API的KEYEDMUTEX/NTHANDLE数值与DDI MiscFlags不相同，不能直接套API位值判定或放行。

## 匹配内核确认device归属

dxgkrnl.sys 10.0.22621.2792 SHA256
`99d425e1c997eae32909fa4f37d6c8e2e846a5374c602295546c8be2547aa5e6`：
DxgkSubmitCommandInternal在RVA0x11a9b4读取NumPrimaries并限制为16；
DXGCONTEXT::SubmitCommand在0x11c224读取数量、0x11c258取得WrittenPrimaries地址，调用VidMm。

同机dxgmms2.sys 10.0.22621.2792 SHA256
`94fa72e5cee778b7cd12bedf13550a78414231df8612547c74f0bc3ed11a62c5`，PDB
`AF63A8A4-9D01-FA37-1A26-6959E133EB0B`／age1：
VIDMM_GLOBAL::VidMmReferenceWrittenPrimaries在RVA0x76834读取allocation所属DXGDEVICE，
0x76838与传入提交device比较；不匹配跳到0xbd1a4，经0xbd1dc返回0x76824的
STATUS_INVALID_PARAMETER路径。不是简单的同进程GPU VA相等就允许跨device提交句柄。
这些静态分支已核对，不宣称该拒绝路径已在57a动态发生，因为57a根本没有提交WrittenPrimaries。

## 本轮后端实现

- `tg_kmt_open_runtime_alias`接收真正的kernel resource handle，导出临时NT引用，在NVK提交device
  上重新打开同一个资源。检查单allocation、私有数据长度上限/返回指针范围及Tegra存储头、确切字节数。
  不把hRTResource/HANDLE强转KMT句柄，不分配替代像素buffer，不回退到原device句柄。
- 为新引用调用真实目标device的MakeResident并等待其分页Fence；超预算/导出拒绝直接报告，
  不强制MustSucceed。该引用没有新建physical backing，不重复修改本地allocator字节计数。
- 每个alias只关闭自己的device/resource。失败路径释放NT handle和查询缓存；若关闭失败，输出
  中仍保留待清理的句柄，调用者必须持有并清理，禁止把它当成功对象使用。
- `tg_kmt_submit_primaries`把列表放入真实D3DKMT_SUBMITCOMMAND，与GPU命令同一次提交；
  检查16上限、空句柄、重复句柄和命令长度。原无列表提交包装保留，不添加空写入标记或伪造完成。
- KMT分页等待同步补入全1完成值拒绝，与tgd3d私有分页防护一致；未修改任意Vulkan应用timeline的合法值域。

## UMD至NVK连接（基于f01448d0）

- owner v1保留原ABI前缀，v2另携带真正的kernel resource及导入确认；保护页控制验证新ICD
  不读取旧UMD不存在的尾部。旧ICD拒绝v2，不能假装接受新提交契约。
- 线性／tiled共享元数据分别保留v1／v2，并增加带DISPLAYABLE标志的v3／v4；OpenResource
  恢复相同属性，拒绝错误版本、未知保留位、布局和设备／驱动身份不匹配。
- 写引用跟随ImageStorage实际backing，在clear、draw、upload、copy／Blt及GenMips写入时收集；
  排除只读访问、去重，并在整次操作取得命令／描述符前分批。33项延迟clear形成16／16／1，
  分批保留原双向Fence协议，不用CPU信号模拟完成。
- 私有提交链进入Mesa公共队列时复制16项以内的内联列表，NVK实际命令提交携带该列表。
  通用提交合并遇到写引用就保留边界，防止元数据丢失或越过16上限；普通提交仍可合并。
- alias关闭失败保留句柄和owner引用；销毁设备成功后才释放剩余引用。内核设备关闭也失败时，
  保留对象到进程清理，不声称已完成退休。tg_kmt_close_device不再无条件清零失败的句柄。
- 带显示引用的资源不参与当前Offer／Trim路径，避免只释放原device的一份驻留引用就声称回收预算。

## 新确认的适用限制：不能据控制测试重新开放现代接口

微软D3DKMTShareObjects明确要求原对象创建时具有NtSecuritySharing。DISPLAYABLE_SURFACE
只描述呈现用途，不足以证明可导出NT共享引用。匹配d3d11.dll的原生
`NDXGI::CDevice::AllocateCB`（RVA0xc080–0xc6e0）在0xc388读取内部共享描述+0x38，
0xc38c按零值跳过，0xc390才把0x40写入KMT CreateAllocation Flags（NtSecuritySharing位）。
这是有条件的分支，不能由本轮CPU替身的ShareObjects成功推断普通DWM资源都能经过它。

同一原生函数未发现对调用者D3DDDICB_ALLOCATE.hKMResource（+0x18）的回写；它更新内部
resource对象的kernel handle（0xc53c–0xc540）。这项静态观察仍需核对完整callback适配路径，
不能解引用hRTResource私有布局获取内部句柄，也不能把控制中人为返回的0x90当作真实输出证明。
本实现缺少合法kernel resource或NT导出失败时直接拒绝，不使用foreign allocation兜底。

下一步需完成**不依赖所有资源都可NT导出的真实提交所有权路径**：核对通过runtime创建第二个
受管理context、在原DDI线程提交真实NVK命令的可行性，连同GPU等待／信号、错误和销毁一起处理。
这只是下一步设计方向，尚未实现；不能提交空命令或伪造WrittenPrimaries来补账。
现代GetSupportedVersions继续只公布原三项接口，KMD仍为WDDM2.6。列扫描几何和当前电源修正保留。

## 验证

SDK/WDK10.0.26100.0，ARM64/x64桥接对象与三项控制程序构建通过（/W4 /WX）。
新控制和同步控制在x64/Wine退出0。旧共享控制在PAGE_WRITECOMBINE读回处退出122；
用0f1760d7原桥接源码及同一控制程序重建也退出122，定位为该环境下既有缓存属性检查不满足。
没有删除检查或把失败改成通过。

07:56:07 UTC，Switch原生ARM64三项CPU控制全部退出0：

| 控制 | SHA256 |
|---|---|
| displayable | `348b70cea2238f0725dffad737777125e5a10f3dd4c5a23f1b8068f60b273040` |
| shared | `6d3d7d92585fb79c5c42e442a0d7d93eecd160d5abb7d3f2c8266ba587338120` |
| sync | `9d4769bdc4a521467102015d8e60dcdccf843ed94ca9a8c28ad82ed5ec45b644` |

控制覆盖same-backing引用的目标device/句柄、长度和数据损坏、分页失败/全1值、关闭失败保留所有权、
临时NT句柄关闭、0/8/16项列表和非法请求、底层提交错误传播。KMT调用由替身接管，没有打开适配器
或执行GPU指令；Windows section/event API为真实调用，不能称为真实跨device共享或上屏验收。
DWM前后及07:57清理后均PID1176；仅上传/运行/删除三项CPU控制及结果文件，未安装驱动或重启。
临时目录`C:\Users\ruje\tgd3d-kmt-controls-20261116-0755`已确认删除。

源码快照、反汇编、构建命令、完整哈希和原生结果在OUT `modern-present-resource-20261116/`。
Gemini独立审计仍待2026-11-20T23:53:18Z额度恢复。现有5b UMD/E95 KMD/95b ICD及电源设置保留。

参考：微软[SubmitCommand及WrittenPrimaries](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dumddi/ns-d3dumddi-_d3dddicb_submitcommand)
（2023-10-06页面；本轮读取2026-11-16）、
[DDI MiscFlags](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/ne-d3d10umddi-d3d10_ddi_resource_misc_flag)、
[primary与呈现资源区别](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/passing-dxgi-information-at-resource-creation-time)。

## 连接轮控制和构建结果

2026-11-16 08:42:49 UTC，Switch上四项ARM64控制全部退出0、无超时：

| 控制 | SHA256 |
|---|---|
| KMT引用／关闭失败／WrittenPrimaries | `db654af68025724c61fcfea69ebc103aac12b0ec904767200f4698560bd4d93a` |
| 实际NVK owner导入／清理 | `e29fb4646a61c70645b710384f423338d98bfc6be71c6c2fa4231ea318849399` |
| 实际公共队列、后台提交列表寿命及合并 | `4057d3299fe55df646f8f0cd46f041d6977759390e5c981a059c09e8a17289d0` |
| tgd3d完整CPU契约 | `baca64cdfd490911d295b21a63de5b703bcf3bf27f8bad2ef4c99bab25b2af68` |

相同逻辑x64控制在本地Wine通过，包括原同步控制。owner控制初版误调用backend内部函数，绕过
公共导入器的allocation链表注册，随后unref导致控制崩溃；已改用真实公共入口后通过，原失败日志保留。
新队列控制执行真实公共QueueSubmit2及工作线程，但driver_submit由替身接管；KMT运输也由替身接管，
未执行任何GPU命令。08:43:33 UTC临时目录已删除，启动07:17:34和DWM1176保持，Code0，
UMD5b66bbec／ICD95b97df4／KMDe95287cb完整哈希和加载路径均与恢复状态相同。

本地未部署产物：

| 产物 | SHA256 |
|---|---|
| ARM64 UMD | `10b718abf70b6f4b74178eed873c8115dc886430ee2cf3a1033e40c4ba3e414c` |
| ARM64 ICD | `ec908de545538914f2c623a876de0c71678aa6d1d90e778835ea719a3214b7d3` |

ICD build-id `588733c530697c06c24ff17cea73b11caa39407cb7253f6071319eadfc446d5e`。
源码diff、原有未提交工作、附加测试、交叉配置和构建日志独立冻结于OUT `displayable-bridge-20261116/`。
没有重建／替换KMD，没有上屏或列扫描新增通过结论。Gemini独立审计仍待额度恢复。

新增参考（读取2026-11-16）：
[ShareObjects与NT共享前提](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmthk/nf-d3dkmthk-d3dkmtshareobjects)、
[Allocate callback结构](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dumddi/ns-d3dumddi-_d3dddicb_allocate)、
[callback线程和上下文规则](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/changes-from-direct3d-10)。

最后补入QueueSubmit2后续批次验证失败时释放此前尚未提交的记录和临时同步引用。分配计数控制确认
无泄漏，x64通过；08:47:55 UTC仅重测新ARM64队列控制，退出0、DWM1176不变、临时目录已删除。
该最终控制SHA256 `2636bb322610cabc3dfce055d66ceedaf2640e8a3111f27e78d62313d24638bd`；上表保留08:42原四项的实际哈希，不混写测试轮次。
