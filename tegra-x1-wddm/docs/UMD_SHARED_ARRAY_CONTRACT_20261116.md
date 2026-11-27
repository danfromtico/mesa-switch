# tgd3d 共享数组与真实 GPU 所有权

本轮接在 `9be5e9a0076e049bb9f17b8b4c72ad24cf1c8933` 后。旧72字节线性共享描述不变；
新增152字节、版本2的同NVK纹理布局描述，支持现代接口需要的双层2D共享纹理及八种颜色格式。
此时现代表仍未对系统启用，Switch继续使用5b66bbec UMD、e95287cb KMD和95b97df4 ICD。

## 实现边界

- NVK的线性图像不支持数组，不能只放宽ArraySize检查。本轮使用NVK原有optimal图像创建及
  私有runtime allocation导入，让NIL决定真实数组步长、PTE kind及对齐。
- 元数据不传进程指针、物理地址或跨设备复用的allocation句柄。每个消费者使用runtime交给自己的
  句柄和GPU VA，并核对LUID、device/driver UUID、driverVersion、完整创建参数与内存要求。
- 创建者首次使用从UNDEFINED开始；打开者从GENERAL接收既有内容。每次使用取得外部队列所有权，
  提交前释放回外部队列。提交及跨设备顺序继续依赖真实runtime/NVK双向GPU Fence。
- release屏障只涉及本次实际使用的图像，记录随存储而非D3D对象身份移动。失败不清空未提交所有权，
  不伪造完成；销毁仍等真实GPU完成后才释放runtime backing。
- 支持的8种格式为RGBA/BGRA/BGRX的UNORM和sRGB，以及R10G10B10A2_UNORM、
  R16G16B16A16_FLOAT。当前为单mip、1–2层、无CPU映射、非primary。
  这不是扫描主表面，不改变DC列扫描所需primary契约。

来源：本树NVK `nvk_image.c`、NIL线性/块布局与Windows runtime导入实现；
[微软Windows8共享数组要求](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/software-requirements)。
私有导入不是公共Vulkan external-memory handle ABI，必须在匹配的本支线ICD上验证。

## 验证

ARM64/x64构建通过，最终x64 Wine控制退出0。控制覆盖两次真实生产代码提交的屏障序列、
句柄及PTE映射隔离、21处元数据破坏、内存要求不匹配、绑定失败清理、生产者释放后消费者保有
backing，以及对象身份交换后按正确图像释放。payload只是CPU替代存储，不是NIL/GPU像素验证。
新增控制初次发现owner拒绝非零kind；修正只放开已验证的optimal共享所有者，线性primary限制不变。

ARM64 DLL SHA256：`813d638c00d915e85a89969cc0c6dffdbab4bb7b73ea7721ae39aca0f7f7f76a`。
ARM64控制程序SHA256：`bfa4b81ae3b26a50ad8e960d06df2a0e1a139b98d6adb96a139662d50ebc15b4`。
候选未安装。完整源码、构建日志、失败和成功控制日志、产物及哈希位于
OUT `shared-array-contract-20261116/`。Gemini额度恢复时间为2026-11-20T23:53:18Z，本轮独立审查待补。

下一步接能力查询、设备协商及现代接口必需操作，再用普通DWM呈现验证横向主表面。
