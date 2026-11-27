# WDDM2.0上下文、对齐和驻留审计

## 本轮结果

原生试验失败后先恢复，随后仅在本地审计和修正UMD。没有再操作Switch、重复安装失败组合，
也没有改KMD、ICD、功耗、双向Fence或显示时序。

源码`0b9b7044edaa26700cb8ba5ca78e5e65ff8828f9`首次声明WDDM2.0 UMD，仍只声明D3D10管线。
源码在协商为现代接口后会同时启用核心层CreateContextVirtual、现代资源入口、普通应用资源的runtime allocation、
共享数组、Trim和DXGI1.4路径；因此不是只改一个上下文回调的试验。接口协商、资源分配和原生
提交的集成验证不充分，先前CPU控制不能覆盖真实runtime/NVK/KMD配合。

## 三项静态核对

| 项目 | 已核对内容 | 结论与边界 |
|---|---|---|
| ContextTypeFlags | WDK10.0.26100.0定义的新核心层回调接收`D3D10DDI_HRTCORELAYER`及`D3DWDDM2_0DDICB_CREATECONTEXTVIRTUAL`。实现使用该类型，NodeOrdinal=0、EngineAffinity=1、Flags=0、ContextTypeFlags=3D\|COPY；NVK另有KMT context。 | 没有把旧KT回调强转成新回调。匹配runtime静态检查允许低位掩码，不能据此说完整同步语义已通过，也没有证据证明位组合5非法。保持原值，继续核对runtime context与NVK队列之间的实际契约。 |
| 对齐与GPU VA | runtime backing按4KiB取整；KMD普通allocation返回PAGE_SIZE对齐。NVK对每个非线性plane用NIL的plane_align_B分配独立GPU VA，KMT预留额外padding后取齐；offset/size/reservation检查和分页Fence仍在。 | 4KiB是WDDM页接口粒度，不等于把图像要求强制降为4KiB。当前静态路径未发现直接把CPU指针/物理地址当GPU VA传递的代码。本轮没有真实GPU布局／像素验证。 |
| 驻留预算 | KMD已有跨设备并发计费的512MiB总量上限，旧UMD也有Trim和成功分页等待。`runtime_make_resident`却只请求一次，忽略E_OUTOFMEMORY的NumBytesToTrim。 | 这是确定的缺口：适配器总上限不能替代动态进程预算。已补裁减和重试；没有本轮运行分支证据，不能认定它造成此次黑屏。 |

相关实现：`src/nouveau/tgd3d/tgd3d_runtime.cpp`、`tgd3d_offer.cpp`、
`src/nouveau/vulkan/nvk_image.c`、`nvkmd/windows/kmt_native.c`、
`tegra-x1-wddm/windows/context.c`、`allocation.c`。KMD上下文设置VirtualAddressing对应的
NoPatchingRequired，GDI上下文的allocation list为256；没有因本次猜测改动这些设置。

微软ContextTypeFlags页面带有笼统JPEG描述，不能据此把3D context解释为JPEG专用。本轮同时
对照头文件及匹配的d3d11.dll 10.0.22621.2715，SHA256
`ccc352f7097d0320368000dbe9ff92772d8fc909185d95c8a0753cb8f115f880`，PDB
`4DD64C1C-37D9-00CB-45AF-89C52B2A1534`／age1。
CreateContextVirtual2 RVA0x42a88读取结构+0xC，RVA0x42ac0检查0xffffffe0；
RVA0x42c3c把标志交给runtime上下文对象构造。静态分支存在不等于本轮已执行。

## 预算修正

- E_OUTOFMEMORY不增加驻留引用。释放当前owner锁后，回收已完成的缓存，并按NumBytesToTrim
  驱逐安全闲置对象；保持当前目标、未完成/未提交引用、应用映射及扫描primary。
- 使用真实GPU完成值，不提交新GPU工作、不等待渲染Fence、不伪造完成。Evict成功才计为释放，
  同时读取其更新后的NumBytesToTrim；预算再次收紧时重新裁减。
- 没有对象可再裁减时只进行一次CantTrimFurther请求。普通资源创建不设置MustSucceed；失败
  继续返回，提交路径不得使用未驻留backing。成功MakeResident后的分页等待重试不再增加引用。
- 默认GetSupportedVersions恢复原三个D3D10版本；现代实现、typed tables和控制覆盖保留。
  这不是关闭tgd3d或双向Fence，也不是撤销复制、同步、EMC等已有修正。

边界：这轮只修runtime拥有的分配。NVK内部KMT分配仍有独立上限及失败清理，不能宣称本补丁
已经把两个device的所有缓存、预算和Trim通知统一管理。跨device驻留、失败传播、现代资源描述符
以及必要资源操作仍需审计。KMD的GpuMmu隐式系统内存路径不能照旧文档直接盲加ApplicationTarget；
WDK中该旧位已注明由LocalBudgetGroup/NonLocalBudgetGroup替代，须结合实际segment模型另行确认。

ARM64/x64 DLL和控制程序构建通过，本地x64控制退出0。新增控制注入连续两次超预算、零裁减估计、
预算更新、无法再裁减、分页等待失败后重试，检查受保护对象和引用计数。它们没有执行GPU或真实
WDDM runtime。构建只出现已有dxbc-spirv未使用字段警告。源码、diff、产物及完整哈希保存在OUT
`wddm2-contract-audit-20261116/`，本补丁没有安装；Gemini审核因额度限制待恢复。

## 原生失败与恢复

- 失败UMD：`d17ff16d431cc93ac5a51d3d4384fba681540b43d197c2347b36a87200c4ed14`。
  06:13:10 UTC ARM64 CPU控制退出0；06:15:33.486Z计划加载启动。用户报告黑屏、只有鼠标且
  无法操作、鼠标时隐时现。06:20:05读取Code0，但DWM已由1416变1496，加载该失败UMD。
- 没有切换显示方向，没有执行四色窗口或列扫描。Code0不能推翻物理画面失败。
- 收集器缺少`Start-NativeUmdTrace.ps1`及`app/tgd3d-trace-export.exe`，未取得本轮UMD事件；
  JSON读取另有WinPS默认编码问题，后续用ReadAllText读取成功。这两项是工具失败，不能冒充
  驱动调用链证据。下轮打包必须自包含并提前核对依赖，不能直接借用基线目录中的假定工具。
  DLL已加载不能单独证明现代接口协商及新回调实际到达；本轮未取得这项事件证据。
- 06:20:16恢复旧注册和登录任务，执行一次恢复重启；新启动06:20:54.419Z。
  06:31:18核对DWM1416／06:21:26.779Z、Code0及11项完整文件哈希。
- 当前UMD：`5b66bbec86af299941812e8376cc1ac4a5630385828c895b7a52ec96d5c5ad7a`。
  KMD：`e95287cbeb1d2c325baacb9685affd1d24783b22ecadc1801ba1dffcfe7cc25e`。
  ICD：`95b97df4e83a2a90315a8784d83e75771021093dec34f98362b584d1f546bdff`。
  目录`C:\ProgramData\SwitchWOA\GpuUpdate\tgd3d-copy-lifetime-5b66bbec-20261116-122524`。
  Handheld、85%充电限制、风扇模式1、动态EMC和登录后启用保留；恢复实屏未另行验收。
- 两个临时任务、失败候选、测试目录、SPIR-V和上传包已清理；旧包及原恢复备份保留。
  原始结果在OUT `modern-device-admission-20261116/`，包括`exact-recovery-verification.json`。

## 实际参考

查阅日期2026-11-16。使用WDDM2.x适用契约，KMD目标仍为2.6；没有按WDDM3.1扩展声明。

- [MakeResident回调](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dumddi/nc-d3dumddi-pfnd3dddi_makeresidentcb)：E_PENDING、原子失败、裁减重试。
- [进程预算](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/process-residency-budgets)与[驻留概述](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/residency-overview)：预算动态变化及每device驻留集合。
- [MakeResident标志](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dukmdt/ns-d3dukmdt-d3dddi_makeresident_flags)：CantTrimFurther与MustSucceed的不同条件。
- [ContextTypeFlags](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/ne-d3d10umddi-d3dwddm2_0ddi_contexttype_flag)及[ContextInfo](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/ns-d3dkmddi-_dxgk_contextinfo)：结合上述WDK和匹配runtime，而非仅凭页面短描述。
- [GpuMmu模型](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/gpummu-model)、[GPU segments](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/gpu-segments)、[SegmentFlags](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/ns-d3dkmddi-_dxgk_segmentflags)：4KiB逻辑接口、隐式system segment与预算分组的适用差异。
