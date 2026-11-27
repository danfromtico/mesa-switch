# Deferred Init 冷启动对照

**状态：实机冷启动仍自动重启，用户要求回退；以下保留实验设计与验证历史，
此诊断开关、辅助程序和相应代码已经从当前分支撤回。**

这是按用户要求增加的诊断构建，`-Dtgd3d-deferred-init=true`，默认关闭此诊断选项。
普通构建仍在创建设备时启用双向 GPU Fence；不能同时定义
`TGD3D_CPU_SYNC_CONTROL` 与 `TGD3D_DEFERRED_INIT`。

## 实际延迟的工作

* CreateDevice 先建立 NVK 设备、命令池和本地 Vulkan completion timeline。
  不在此时创建、共享或导入两只 runtime monitored fence，也不创建绘制布局。
* CreateShader 仍进行 DXBC 校验、资源反射及 SPIR-V 翻译。资源掩码必须在
  `prepare_reads` 和数值类型 variant 选择前有效，不能直接跳过反射。
  NVK/NAK 编译、descriptor/pipeline layout 和默认 sampler 延到首次实际使用。
  未使用的 shader 不调用 NVK；登录画面必需的 shader 仍会在登录前按需编译。
* 登录辅助程序 `tgd3d-desktop-ready.exe` 看到本会话可见的 Explorer 桌面与任务栏，
  设置 `Local\SwitchWOA.Tgd3d.DesktopReady.v1` 手动复位事件，并持有它到注销。
  通过一个专用 HKLM Run 值启动；重启后没有持久的“已就绪”标志。
  该事件表示 shell 已出现，不代表物理画面验收通过。
* 下一次允许等待的 Flush/Present 在原 DDI 调用线程完成旧提交，再事务式创建
  两只 runtime fence。辅助程序不调用 DDI，不在后台线程调用 runtime callback。
  `DONOTWAIT`、QueryGetData 和设备销毁不启动切换。
* 保留原本地 timeline；切换后 NVK 同时 signal 本地完成值与 runtime 输出 fence。
  后续提交仍 wait 原 timeline，保留提交间内存依赖。新 runtime fence 的初值只取
  已确认完成的 N，下一次 signal 为 N+1，不重置序号、不由 CPU 冒充在途 GPU 完成。
  第二只 fence 创建/导入失败时释放临时对象，原 timeline 保持有效，错误上报。
  两只输出信号不假设完成顺序；完成值取二者较小值，CPU 等待覆盖二者后才能退休
  对象或销毁设备。控制测试刻意让本地 Fence 先完成，确认对象仍保留。

桌面就绪前使用已有 CPU 同步对照的 Flush/Present 等待行为。
该启动对照未建立 runtime→NVK 的 GPU ingress；它不构成共享资源异步生产/消费
同步正确性的完整证明。桌面后恢复双向桥接，仍须分别验收，不能称作已经修复。

## 范围和检查

上机候选以冻结 9de42c82 + 7f5f416b + 42ad5d1b 为底，只加入本轮延迟初始化。
同步 callback 的两处失败返回同时统一为记录的 device-removal 状态。
不引入 HEAD 的着色器磁盘缓存。Device/Shader 布局变化，因此重编全部 tgd3d 对象，
不混用旧头文件编译的对象。保持原 .158/bit14 KMD、d3423d26 ICD、原生启动设置及
CPU/风扇/睡眠/GPU 预算。不会自动回退 .59 或切 BasicDisplay。

CPU 控制覆盖未就绪/已就绪、非阻塞调用、双向 signal/wait、旧 timeline 的连续性、
两次导入的分别失败、GPU 超时不得退休对象、首次编译及资源反射、布局部分失败清理。
这些不是 GPU 执行、冷启动、像素或实屏稳定性通过。

ETW 数字事件19：arg0=0 启动对照；1=桌面后桥接切换（E_PENDING/完成状态配对）；
2=首次布局建立。arg1=Device 地址。shader phase 事件仍区分前端与后端。
已安装的冷启动 ETW/WER 配置保留，待用户确认修复后移除。

## 参考契约

核对日期 2026-11-13；适用范围为本驱动的非 free-threaded D3D10 DDI、Windows
monitored fence 与会话内事件，不把 D3D12 应用层行为直接套作 UMD 契约：

* [微软 D3D callback 线程规则](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/changes-from-direct3d-10)，页面更新2021-12-15。
* [D3DDDI_SYNCHRONIZATIONOBJECTINFO2](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dukmdt/ns-d3dukmdt-_d3dddi_synchronizationobjectinfo2)，页面更新2025-03-19；使用 InitialFenceValue 与 NT handle 共享。
* [Windows kernel object namespaces](https://learn.microsoft.com/en-us/windows/win32/termserv/kernel-object-namespaces)，页面更新2024-05-07；显式使用本会话 Local 事件。

* [Vulkan synchronization — Implicit Synchronization Guarantees](https://docs.vulkan.org/spec/latest/chapters/synchronization.html#synchronization-implicit)，查阅2026-11-13：同一 VkSubmitInfo2 内的多个 semaphore signal 没有相互完成顺序保证。
