# Displayable资源、提交device与WrittenPrimaries

本轮基于`0f1760d79ade28daaa8c8a7d01651f83c0e9d75a`，继续57a原生失败后的源码排查。
新增的是提交端基础能力，尚未接通UMD，未启用现代接口，未安装新ICD/KMD或改变显示方向。
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

**未接通的部分：** UMD保存hKMResource、importer持有alias、提交前按实际写入形成列表、
16项边界处按完整操作分批、列表跨异步提交的存活、失败回收和真正GPU完成后的释放。
还要验证runtime是否允许导出各类呈现资源；导出失败不得通过虚构句柄/布局绕过。
这些完成前，不放开DISPLAYABLE_SURFACE接纳，不启用现代默认协商，不把本轮作为黑屏修复版。

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
