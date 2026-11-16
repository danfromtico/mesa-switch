# 现代资源入口：合法线性缓冲区与错误边界

## 原生结果：失败，已恢复并清理

源码`57a0845c4bfeca384a5181b366ca923113ce543b`，ARM64 UMD SHA256
`f9bec71061d2f6ed0c1fdb207beffa480cb048d2937e54c6b41d17a930094c44`。
07:09 UTC原生ARM64 CPU控制退出0；07:10 UTC计划加载候选。真实DWM PID500和4772均完成
WDDM2.0设备创建，并各创建15个buffer，说明线性buffer修正已推进真实调用链。

本轮仍失败：Explorer/TextInputHost等进程拒绝32×32、BindFlags=0xa8的纹理请求；DWM发生重建，
PID1540在VkDevice创建阶段失败，PID4772最后停在MakeResident E_PENDING后。最后一条事件
不能单独证明永久死锁或给出超时根因。旧跟踪未保存该纹理完整描述，不据此猜定TextureLayout。
PID500的首个提交记录sequence=1却completed=UINT64_MAX；当前getter直接透传该值，后续修正见
[Fence核对](WDDM26_CONTEXT_FENCE_AUDIT_20261116.md)。不能把它写成GPU正常完成。

**计数更正：事件10是SetDisplayMode，不是Present。** 两次成功属于模式设置；事件30/kind2的
Present计数为0。该轮未启动横屏、四色窗口或列扫描，没有Present/实屏通过结论。

07:16:53 UTC恢复原注册，单次恢复重启后的启动时间为07:17:34.328844Z，DWM PID1176。
07:21 UTC确认DWM加载原5b UMD/95b ICD，07:25:01 UTC完整核对11项文件哈希、Code0、Handheld、
85%限制、fan1及EMC频点204/1331.2/1600/2500 MHz。保留E95 KMD；失败候选目录、两项临时任务、
ETW会话、测试目录和上传包已清理。未新增物理恢复验收。测试前后电源读数不同，未宣称全程插电。
现代接口已再次撤下默认声明，保留资源修正和类型化实现；没有重复安装本失败候选。

以下记录为该候选构建前的分析及修改过程。

本轮接`770edde7`，继续处理WDDM2.0候选在切换方向之前的黑屏。没有把猜测当作上轮实机调用记录。

## 已定位的两项错误

1. `CreateResource20`拒绝所有非UNDEFINED布局，包含ROW_MAJOR缓冲区。匹配实机的d3d11.dll
   10.0.22621.2715中，`CDevice::CreateLayeredChild`的buffer分支RVA0x1df48构造DDI描述符于
   `x26+0x260`。RVA0x1dfc0–0x1dfd4将API CPUAccessFlags转为MapFlags，RVA0x1dff0/0x1e000
   计算MapFlags是否非零，RVA0x1e018将0或1写入`x26+0x2a8`，即DDI TextureLayout的+0x48。
   因而合法的CPU可访问buffer会收到ROW_MAJOR，不依赖可选default-texture-mapping能力。
2. 资源创建/打开失败把DXGI_ERROR_UNSUPPORTED等API状态交给SetErrorCb。普通Create/Open允许
   OOM和DDI设备移除；主表面opt-out另有DXGI_DDI_ERR_UNSUPPORTED，不能拿API同名错误代替。
   同一runtime的UMSetError_ RVA0x1f3be0按调用类别分派；普通资源类别2的分支RVA0x1f3d24不接受
   API unsupported，primary类别8另识别DDI unsupported。非法返回进入RVA0x1f3f40的critical分支。

二进制SHA256为`ccc352f7097d0320368000dbe9ff92772d8fc909185d95c8a0753cb8f115f880`，
PDB `4DD64C1C-37D9-00CB-45AF-89C52B2A1534`／age1，与前轮匹配源一致。字段偏移同时核对
WDK10.0.26100.0。普通texture2D的CSafeDDIResource构造RVA0x13618在RVA0x138c4按调用者布局
写+0x48，与buffer的自动ROW_MAJOR逻辑不同；不能把D3D12 buffer规范直接代入D3D11。

## 修正范围

- ROW_MAJOR只放行已有线性Slice实现的buffer和非共享、非primary的staging纹理；已知行填充的
  初始数据按现有row pitch复制。DEFAULT/optimal纹理和显式标准swizzle仍不可冒充线性布局。
- Create/Open失败先释放部分对象，OOM保持可恢复。普通对象不能执行时按DDI报告设备移除，
  保留原始错误跟踪；不把它伪装成OOM、成功或已完成GPU工作。
- 只有不支持的primary创建请求返回DXGI_DDI_ERR_UNSUPPORTED并设置NO_SCANOUT；成功创建的
  1280×720等primary仍清除NO_SCANOUT，不改变列扫描目标或强制走GPU预旋转。
- 该次候选重新声明WDDM2.0，仍只声明既有D3D10管线级别，失败后已撤下默认协商。预算修正和双向Fence保留，KMD/ICD及
  平台配置不变。前轮失败UMD没有重复安装；本候选的原生验证单独记录。

线性输入和错误边界的ARM64/x64构建及本地CPU控制通过，包括实际资源入口、缓冲区初值、带填充
staging行、失败清理、primary返回值及错误后禁止继续分配。接口重新声明后的最终候选验证和
实机结果以OUT `modern-resource-audit-20261116/checkpoint.json`为准；控制不等于GPU通过。

本轮按用户授权尝试VPS Claude只读补充复核，返回`Not logged in`，未修改登录或配置，未得到审计。
Gemini Carbon High仍待2026-11-20T23:53:18Z额度恢复；没有将主窗口检查称为独立审计通过。

## 已排除的猜测与剩余验证

微软AllocateCb允许非共享分配以hResource=NULL关联device，当前私有分配不能仅凭该值判错。
GpuMmu地址空间按process管理；跨device映射不能仅凭两个device不同判为非法，但必须保留各自
allocation handle、分页和引用生命周期。本轮没有为此更换物理地址、关闭双向Fence或修改KMD。

两项源码缺陷已证实，真实buffer创建已推进，但本轮新增结果仍未证明黑屏根因已修复。
尚未完成列扫描、GPU已知像素或vkcube验收。

参考（2026-11-16）：微软[CreateResource](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/nc-d3d10umddi-pfnd3d11ddi_createresource)、
[OpenResource](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/nc-d3d10umddi-pfnd3d10ddi_openresource)、
[primary回退](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/passing-dxgi-information-at-resource-creation-time)、
[Allocate关联](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dumddi/ns-d3dumddi-_d3dddicb_allocate)、
[GpuMmu](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/gpummu-model)。公开页面部分成员描述
不完整/仍标reserved，采用匹配runtime与WDK补充；没有修改系统二进制。
