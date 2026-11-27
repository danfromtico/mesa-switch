# tgd3d 驻留缩减与能力查询

本轮接在共享数组提交 `d87e06e8` 后，现代接口仍未对系统声明。没有修改KMD、ICD、
电源设置或双向Fence协议，也未安装本轮DLL。

## 改动

1. 接入DXGI1.4的TrimResidencySet。周期缩减使用上次周期的真实完成序号，预算缩减只处理已完成
   且未映射的资源；扫描primary保持驻留，因为渲染完成不等于显示控制器停止读取。
2. Evict成功才更新驻留状态；下一次GPU使用前调用MakeResident并等分页完成。
   分页完成不改变渲染Fence。分页等待失败后的重试不重复增加MakeResident引用。
3. 用不分配内存的设备资源链表覆盖创建、打开、失败退出和销毁。提交前检查本批资源；
   重命名或销毁发生在提交前时，旧backing先恢复驻留，再按原来的真实序号退休。
4. Flush认识TRIM_MEMORY，回收已完成的临时上传、命令及描述符缓存；未知的新Flush位按契约
   保守提交，不把它当非法调用。保留运行所需的当前上传块和所有在途对象。
5. GetCaps改为按类型和精确长度输出。只报告既有D3D10管线，不新增FL10.1/11/12、计算、
   UAV、命令列表、自由线程、保守光栅或显式纹理布局能力。UMA与CPU/GPU缓存一致性分开；
   GPU虚拟地址40位与本支线KMD一致。GetSupportedVersions的短缓冲区和空参数也有边界检查。

这些是开发中驱动的接口实现，不是完整Feature Level认证。既有MSAA、buffer RTV等缺口未因此变成支持。
当前Trim覆盖runtime拥有的应用资源，以及UMD可释放的临时缓存；不宣称任意预算请求都能满足，
也不以强制丢弃活跃primary或在途数据凑预算。

参考：微软[驻留模型](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/residency-overview)、
[Flush标志](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/ne-d3d10umddi-d3d11_1_ddi_flush_flags)、
[GetCaps](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/nc-d3d10umddi-pfnd3d10_2ddi_getcaps)。
结构及位域使用SDK/WDK10.0.26100.0；没有照搬D3D11On12的空Trim实现，因为其D3D12内存所有权不同。

## 匹配运行时的补充核对

继续只读分析已核对身份的d3d11.dll 10.0.22621.2715，SHA256
`ccc352f7097d0320368000dbe9ff92772d8fc909185d95c8a0753cb8f115f880`。

- TrimNotificationCallback RVA0x526e0在调用DXGI表0x310槽前，调用RVA0x35000的
  CDDISync::CFreeThreadedLock；后者按设备+0x544进入锁。GetCaps type128读取的
  FREETHREADED bit会在RVA0x2cc28清掉该锁标志。本驱动不声明FREETHREADED。
  这是静态路径证据；单线程设备标志对锁的影响仍应在现代接口启用前核对，不能冒充实机已执行。
- CreateContextVirtual2 RVA0x42a88读取新结构+0xC的ContextTypeFlags，再转换内核请求。
  WDDM2.0核心层回调接收hRTCoreLayer及新结构，与当前旧KT回调不是同一个函数类型。
  下一步设备协商必须接入对应回调，不能只填长函数表后继续调用旧上下文入口。

相关片段和全量本地反汇编保存在OUT `modern-residency-20261116/`；没有修改系统二进制。

## 验证和设备状态

ARM64/x64 DLL及控制程序构建通过，最终x64控制退出0。
05:52:32–05:52:33 UTC，Switch运行ARM64控制程序
`588ef4e2d5e21bad8803222b3b107d2a5025e0f3333ff5bdfcae22084651e449`，退出0。
覆盖周期／预算缩减、在途保护、扫描primary排除、分页重试引用平衡、Evict失败保持原状态、
销毁前恢复及提交后退休。控制使用CPU替代Vulkan/runtime回调，没有真实GPU绘制。

之后仅放宽未定义使用pInfo的能力查询以忽略该输入，并补本地控制；纹理布局查询仍拒绝
未声明的布局输入。最终ARM64 DLL为
`1f1712c484a9d5aec0b3337a7e06b9644fd6c2603d93274edcc7a06b64a84cae`，
最终ARM64控制程序为`43b69b4bfc18563fa7a06d13deeebc41909db76c5a03381489a07bc905e9cf1e`。
最终控制二进制未重复上机，前述原生结果仅对应其明确哈希。

05:53:39 UTC已清理临时控制目录、SPIR-V及上传包。启动时间仍为04:26:20.1954500Z，
DWM仍为PID1416／04:26:53.0211410Z，设备Code0，加载UMD仍为
`5b66bbec86af299941812e8376cc1ac4a5630385828c895b7a52ec96d5c5ad7a`。
本轮没有安装、切换方向或重启，亦没有新增横屏／实屏验收结论。

完整初版与终版源码、diff、构建、控制结果和产物哈希在OUT同目录。Gemini Carbon High
仍受额度限制，2026-11-20T23:53:18Z前不重试，冻结差异待独立审查。

后续接现代上下文及设备协商、剩余必需资源操作，再启用MPO1，通过普通DWM路径验证列扫描。
