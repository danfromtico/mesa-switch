# 普通 DWM 列扫描路径：UMD 呈现接口门槛

## 本轮结论与边界

旧 D3D10 UMD 已经经 runtime 到达 KMD MPO3，PresentId=241 仍是有效证据。
本轮进一步定位到另一条独立路径：DWM 是否将自己的普通 Present 转成带平面属性的 MPO。
在实机匹配的 D3D11 runtime 中，这要求 KMT 的 MPO3 查询成功，且 DXGI1.4 的
`pfnPresentMultiplaneOverlay1` 非空。已准备的 DXGI1.3 表只有旧 MPO 入口，不满足这个检查。

这是匹配二进制中的静态条件，不证明全部黑屏/重启由此造成，也不说明旧 UMD 到不了 KMD MPO3。
本轮补了该入口和 WDDM2.0 类型适配，**GetSupportedVersions/CreateDevice 仍未启用现代表**。
不得将准备代码、控制测试或短时逐行横屏照片改记为 Windows 列扫描验收。

当前 Switch 继续运行上一轮的 UMD `5b66bbec86af299941812e8376cc1ac4a5630385828c895b7a52ec96d5c5ad7a`、
KMD `e95287cbeb1d2c325baacb9685affd1d24783b22ecadc1801ba1dffcfe7cc25e`、
ICD `95b97df4e83a2a90315a8784d83e75771021093dec34f98362b584d1f546bdff`。
本轮只在设备运行自包含 ARM64 控制程序，没有替换驱动、改变显示方向或重启。

## 版本和可追溯来源

| 实机文件 | 版本 | SHA256 |
|---|---|---|
| dwmcore.dll | 10.0.22621.2715 | `72e4a0545913eed25cc75d59894b6963a44ece1338c9d89658caaf5e433ae301` |
| d3d11.dll | 10.0.22621.2715 | `ccc352f7097d0320368000dbe9ff92772d8fc909185d95c8a0753cb8f115f880` |
| dxgi.dll | 10.0.22621.2506 | `c0a20e80493b245d95d64185c363b090dd00d33b2fa8e690dca59ae1d2b4cac0` |
| dxgkrnl.sys | 10.0.22621.2792 | `99d425e1c997eae32909fa4f37d6c8e2e846a5374c602295546c8be2547aa5e6` |

新取回 dwmcore/d3d11 的 PE CodeView GUID 分别为
`5FB7B4CB-8038-332C-65C6-2532D96BC365`、`4DD64C1C-37D9-00CB-45AF-89C52B2A1534`。
微软公开 PDB 的 GUID 与 DBI age=1 均匹配；Info stream age=2 单独记录，不冒充 PE age。
PDB SHA256 分别为 `cbf44d19292fe1a31bbdf6be3310ee60dcac91d7b007653a4a65e69d93e3ee91`、
`da4d2c31a30feb29311d9545198379374c1c545182bd3a4bc02e7d6dc49f30ab`。
只读分析系统文件，没有修改系统内核、运行时或私有开关。公共符号折叠别名不重复计为调用路径。
完整身份、拆分反汇编与源文件冻结清单在 OUT `primary-column-contract-20261116/`。

| 匹配入口（RVA） | 核对结果 |
|---|---|
| d3d11 `CDevice::CreateDriverInstance` 0x29840 | 0x29948 将对象+0x280 作为 DXGI 函数表传给 UMD CreateDevice。 |
| d3d11 `CDevice::MPO3DDIsEnabled` 0x70D0 | 查询 KMT adapter info type43（MPO3DDI_SUPPORT），不是只看 UMD 版本号。 |
| d3d11 `CDevice::GetMultiplaneOverlayCaps` 0x943F0 | 0x946B4 调上项；0x946C0 检查接口指针+0x310，即完整对象+0x320；非空才置内部能力 bit18。表起点+0x280 到+0x320 是第20项，精确对应 DXGI1.4 MPO1。 |
| dwmcore `CLegacySwapChain::GetMultiplaneOverlayCaps` 0x36600 | bit18 设置后内部状态=2；否则需要真实多平面数量才进入旧的状态1。当前硬件只提供一平面，不能谎报两平面绕过。 |
| dwmcore `CheckSupportsConvertPresentToMPO` 0x261FE0 | 检查上述状态是否至少为2。 |
| d3d11 `PresentMultiplaneOverlay1CB_PreWDDM2_2` 0x95C20 / 实现 0x8FBC8 | 消费每平面的 ContextCount、上下文列表、allocation 列表、私有数据长度/指针；禁用全部平面时仍从首平面取得有效上下文。较新 SyncIntervalOverride 尾部不属于该回调版本。 |

KMT overlay caps 的 bit11 是 Version3DDISupport，**KMD 同名位域的 bit11 是保留位**。
两者不能互抄；本轮没有修改保留位、MaxPlanes、NO_SCANOUT 或显隐门控来绕过条件。

## 主表面契约

- DXGI `BuildBackBufferPrimaryDescs`（0x603A0）将输出 rotation 交给 primary descriptor 构造；
  90/270°交换模式尺寸，NONPREROTATED 分支再设置相应 primary flag 并调整尺寸。
  现有 `runtime_primary_create` 将 NONPREROTATED 解释为 identity 存储，与契约一致，不能倒置。
- 标准 shared primary 使用未旋转的尺寸。不能直接将其强改成 1280×720，或把普通非连续共享纹理当成
  已获物理扫描 backing 的 primary。横向存储必须从实际 primary 请求/分配沿完整路径传下来。
- MPO 的旋转为顺时针；VidPN 的方向约定另行转换。SrcRect 相对资源，DstRect 相对监视器。
  当前 DC 路径仍要求完整横向源、物理720×1280目标、已知 pitch 和实际锁存后完成。

参考 [primary descriptor](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/dxgiddi/ns-dxgiddi-dxgi_ddi_primary_desc)、
[显示 miniport 旋转](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/supporting-rotation-in-a-display-miniport-driver)、
[MPO3 attributes](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/ns-d3dkmddi-_dxgk_multiplane_overlay_attributes3)。
文档按接口适用条件使用；运行时实际分支以以上匹配版本核对，不因当前网页较新便假设实机全部支持。

## 本轮代码

1. 添加具名赋值的 DXGI1.4 表与 PresentMultiplaneOverlay1，校验真实一平面 SDR/BGRA/XRGB 布局；
   继续真实 runtime signal → NVK wait/submit → runtime GPU wait → 原线程呈现回调。
   opaque token 不重建，禁用平面不读取资源，保留有效上下文；不伪造完成或释放在途对象。
2. WDDM2.0 的 SRV/RTV PlaneSlice、query ContextType、Flush、rasterizer、TextureLayout 单独适配。
   不把旧结构强转成长结构；未实现的颜色平面、保守光栅、标准 swizzle、CPU 可见默认纹理不静默接受。
3. DXGI 同表面 Blt/Blt1 不再直接拒绝：先 GPU 快照源区域，再按原请求复制/缩放/旋转，最后按真实
   完成序号退休临时图像。它服务明确的 Blt 请求，不改变 DC 列扫描为默认显示旋转路线。
4. MPO dirty rectangles 只作优化提示。完整平面扫描不访问该数组，也不因提示而拒绝原本有效的平面。

参考 [MPO UMD 契约](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/multiplane-overlay-support)、
[DXGI1.3 呈现及线程规则](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/dxgiddi/ns-dxgiddi-dxgi1_3_ddi_base_functions)、
[MPO1 回调数据](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/dxgiddi/ns-dxgiddi-dxgiddicb_present_multiplane_overlay1)、
[Windows8 UMD 必需操作](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/software-requirements)。
头文件固定 SDK/WDK 10.0.26100.0；旧回调消费的前缀另外与实机反汇编核对。

## 验证与未完成项

ARM64/x64 DLL 和控制程序构建通过。x64 Wine 控制退出0；Switch 原生 ARM64 控制
05:06:07–05:06:08 UTC退出0。程序静态包含本轮实现并替代 Vulkan/runtime 回调，**未执行真实 GPU**。
覆盖两种 MPO 入口四方向、六种失败路径、禁用平面、颜色空间/布局拒绝、当前线程与 opaque token；
同表面双向重叠像素和区域外字节；快照已记录后管线分配失败、完成前不得退休、完成后准确清理。

新增故障注入首次失败属于测试预期错误：主动 OOM 正确产生一次错误回调，但后续执行模型仍要求
错误计数为零。已明确断言单次 OOM 和设备未丢失，再清除已消费的测试计数。原失败结果保留，
没有据此改驱动为忽略错误；该失败控制包只上传、未在设备执行，随后替换为验证通过的包。

候选 ARM64 DLL SHA256 `de7c6085ee7f84f40314bcd81ced3546f041419237e90a2717af0f07e61c4d3b`；
ARM64 控制程序 `4bfb81999a5d67eaa41eee81d3e47988e6ae881e869534580f6649d9e0ddeec9`。
候选 DLL **未安装**。当前驱动与正常登录启用任务继续保留，没有新增测试启动任务。
05:06:57 UTC确认临时控制目录、程序、生成SPIR-V和上传包已删除；前后启动时间仍为04:26:20，
DWM仍为PID1416/04:26:53，已加载UMD完整哈希仍为5b，显示设备Code0。结果已取回本地。

下一步必须补齐真实能力查询/设备协商、现代资源共享与必需操作覆盖，包括纹理数组共享、剩余 ClearView
资源类型及所选驻留模型对应的 Trim/布局契约，再启用现代呈现表做普通 DWM 横向主表面测试。
GetSupportedVersions/CreateDevice 继续保留原三种 D3D10 版本，不虚报新 Feature Level、MSAA 或计算能力。
Gemini Carbon High 仍处额度等待，恢复时间2026-11-20T23:53:18Z；本轮冻结 diff 待独立审核，未伪称已审。
