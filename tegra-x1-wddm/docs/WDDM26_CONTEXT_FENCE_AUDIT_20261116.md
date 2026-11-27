# 匹配内核、同机Noble与WDDM 2.6核对

本轮接57a0845c失败后的恢复状态，局部修正tgd3d的完成值处理。未改KMD、ICD、显示时序、
列扫描布局或平台设置，没有再次安装、切方向或重启。UMD的WDDM2.0接口版本与KMD的
WDDM2.6版本分别核对；不把新版网页中的WDDM3.2字段套到本驱动。

## 匹配版本和上下文

本机dxgkrnl.sys 10.0.22621.2792，SHA256
`99d425e1c997eae32909fa4f37d6c8e2e846a5374c602295546c8be2547aa5e6`；
PDB GUID `797F457A-6488-C67C-8E55-B0F3E771DC32`，PE/DBI age1，Info age2差异保留记录。
此次再次对本地两份系统文件计算哈希，均匹配原实机取回记录。未重新下载不匹配内核替代。

`DxgkCreateContextVirtualImpl` RVA0xfcde0读取KMT结构：0xfcf40检查EngineAffinity高位，
0xfcf70检查NodeOrdinal范围，0xfcf78调用掩码计数辅助函数；0xfcfc0在对应版本条件下检查
Flags保留位。0xfd110进入`DXGDEVICE::CreateContext`（0xfe030）。这些是匹配静态路径，
不是对本轮每条内核分支都已有动态到达证明。

该KMT结构没有UMD的ContextTypeFlags。匹配d3d11.dll 10.0.22621.2715的
CreateContextVirtual2在RVA0x42ab0读取UMD结构+0xc，0x42ac0检查掩码0xffffffe0；
0x42b34起构造KMT描述，0x42c3c再将ContextTypeFlags传给runtime上下文对象。
SDK/WDK10.0.26100.0头文件中的核心层回调类型与实现一致：hRTCoreLayer、新结构、
NodeOrdinal=0、EngineAffinity=1、Flags=0、ContextTypeFlags=3D|COPY。不能把该值5直接
解释为KMD Flags=5。未发现这一位组合或句柄类型传错的证据，保留原值。

57a原生记录已成功创建现代设备，但不足以证明所有跨上下文同步正确。详见
[原生结果](UMD_LINEAR_RESOURCE_ADMISSION_20261116.md)和[前一轮预算审计](WDDM2_CONTEXT_RESIDENCY_AUDIT_20261116.md)。

## 对齐、预算及Noble差异

- runtime allocation按4KiB页映射，地址、offset、size及预留范围检查均在；NVK图像另按
  NIL要求预留/对齐GPU VA。KMD上下文私有内存由WDDM连续内存回调取得，GR实例校验CPU/物理
  页对齐和2–4GiB范围。本轮未发现把普通runtime句柄当物理地址传递的证据，未证明全部布局正确。
- 512MiB总量限制与VidMm动态进程预算不是一件事。已有补丁处理NumBytesToTrim、成功分页与引用
  平衡；NVK内部KMT分配仍有独立管理缺口。旧文档ApplicationTarget已被头文件标为deprecated，
  当前table/scanout使用LocalBudgetGroup，paging aperture与GpuMmu隐式系统内存需分别分析，
  不因网页旧位描述而盲改全部segment。
- 同机Noble24.04.5/#149交接报告WIN_OPTIONS=0x40010011、pitch5120、H_OFFSET5119、
  V_OFFSET0、物理720×1280。它与较早#5逐行扫描记录不同，不能混作一个已验收基线。
  参考的维护者源码固定为7d95822acda1f6dab3f3d1d099b43ff9e0d0e626；window.c约770–827行
  把scan_column、独立stride及末字节反向offset写入DC，ext/dev.c另管理dma-buf pin/unpin。
  Windows必须通过自己的allocation、GPU VA、驻留和锁存生命周期完成交接，不能照搬Linux地址或句柄。

## 完成值缺口与修正

失败轮DWM PID500的首个提交事件为sequence1、completed=18446744073709551615，状态成功。
当前NVK `win_sync_value`直接读取monitored fence并返回VK_SUCCESS，因此UMD不能只检查函数返回值。
微软D3D12设备移除文档描述全1唤醒值；这只是相邻契约证据，本轮尚未定位写入此值的具体内核分支，
也未证明设备移除原因。无论来源，这个从0开始、只由私有提交序号递增的Fence都不能据此确认seq1完成。

1. `Device::completed()`拒绝UINT64_MAX并报告DDI设备移除，不允许据此回收command buffer、view、
   allocation或上传内存。保留双向队列协议，不新增CPU伪造信号或每帧阻塞。
2. `wait_paging()`在快速判断前和成功CPU等待后分别检查全1值；失败不得成为驻留或页表更新完成。
3. CPU控制覆盖提交后第一次采样、等待前采样、成功等待后采样，以及分页前/等待中异常与失败分配清理。
   ARM64/x64构建和x64完整控制退出0；没有GPU执行，不属于原生修复验收。新DLL未安装。

当前修正作用于tgd3d私有协议；未修改Vulkan任意应用timeline对UINT64_MAX的通用语义。
WDDM2.x FromGpu同步可由OS调度器挂起依赖工作，不等于WDDM3.2新增的native GPU fence机制。
后者的MonitoredValue与当前读取的完成值不是同一个字段。

后续先继续审计32×32呈现纹理请求、NVK设备丢失传播及分页等待，再恢复现代接口原生验证；
列扫描和vkcube没有新增通过结论。Gemini独立审核仍待额度恢复，VPS Claude未登录未提供结果。

反汇编、冻结失败候选、恢复清单、新构建及控制结果保存在OUT `modern-resource-audit-20261116/`。

参考（读取2026-11-16）：微软[KMT上下文](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmthk/ns-d3dkmthk-_d3dkmt_createcontextvirtual)、
[GpuMmu](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/gpummu-model)、
[进程预算](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/process-residency-budgets)、
[Segment flags](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/ns-d3dkmddi-_dxgk_segmentflags)、
[设备移除与Fence](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device5-removedevice)、
[WDDM2.x与3.2 Fence差别](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/native-gpu-fence-objects)。
