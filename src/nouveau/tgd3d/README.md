# tgd3d：原生 D3D DDI → Vulkan 桌面候选

开发和推送目的地是 `tgd3d-vulkan`（原 `claude-vulkan`，现为仓库默认分支）。本分支的 Windows 构建默认开启
`tgd3d`，DLL 为 `TegraGpuD3DVk.dll`；Zink 的 `TegraGpuD3D.dll` 和注册入口
保持独立。构建开启不等于 Windows 已切换到这个 UMD。

**WDDM2.0 UMD／DXGI1.4 MPO1 候选本轮实机失败，已撤下默认接口声明。** 新实现保留供修正和
控制验证；KMD仍为WDDM2.6，不能将两个版本号混为一谈。`0b9b7044`候选在切换方向之前即黑屏、
鼠标时隐时现，DWM重建。06:31 UTC已核对恢复5b66bbec UMD、Code0和原ICD／KMD，清理候选工具。
双向GPU Fence、登录约5秒后启用及平台设置保持。恢复实屏尚未另行验收。
静态审计已修正MakeResident超预算后缺少裁减／重试的问题，ARM64/x64构建及本地CPU控制通过；
此补丁未安装，尚不能认定为黑屏根因。详见[上下文／驻留审计](../../../tegra-x1-wddm/docs/WDDM2_CONTEXT_RESIDENCY_AUDIT_20261116.md)。
这不是新Feature Level或完整D3D10一致性认证，MSAA、buffer RTV、UAV／视频仍有缺口。
Tegra DC列扫描和vkcube仍待验收，设备与产物的最新状态以
[交接记录](../../../tegra-x1-wddm/docs/TGD3D_COLD_BOOT_HANDOFF_20261113.md)为准。

实现路径是 **D3D runtime → WDDM UMD DDI → Vulkan → NVK → GM20B**，不链接 Gallium、
Zink 或软件光栅器。它不是应用侧 DXVK，也不是完整 D3D10/Vulkan 一致性认证。

## 早期诊断记录（2026-11-13，非当前设备状态）

已确认 DWM 使用本后端。此前测试组合为冻结 `.158`、`fcc53bf1` CPU同步UMD和
`7c677212` ICD，当时登录后自动启用。该组合通过了一轮五分钟实屏交互，用户随后多次
尝试天气与浏览器也正常；但之后文件夹卡住、Explorer自动重启，再次开文件夹仍
卡住，因此整体稳定性仍未通过。仓库默认仍开启GPU桥接；当前实机关闭桥接，不能
记为双向Fence已修复。已知像素、硬件旋转与vkcube尚未验收。
后续ICD补丁`d3423d26`修复meta操作漏恢复pipeline绑定标记，旧／新ARM64 CPU
控制分别退出1／0；第一次热切换失败后，从门控状态启动成功，用户确认“主文件目录正常”，
随后打开天气仍发生重启。bit14 KMD补丁`253bc813`已单独构建、签名并安装，
与修复ICD及`fcc53bf1` UMD组成用户指定的新测试基线；这一组合尚未通过稳定性验收。
原一次许可加启动任务的方案在用户重启后仍为Code43，已替换为KMD补丁`b916f531`
的持久`AllowNativeBoot`授权。17:26 UTC已真正安装`oem94.inf`，SYS哈希`c629328e…`；
无需登录或启动任务切换显卡。17:27重启后，用户报告在登录用户名出现前自动重启；
该次启动失败，尚无Code0或DWM加载证明。SD离线恢复的SYSTEM副本显示初始化各阶段
状态均为0，只说明对应初始化已完成，不能替代桌面验收。原生启动授权按用户要求保留。
这属于后续测试基线，不是稳定版本；当前实机UMD仍关闭双向桥接。
本轮修复标准主表面／影子表面的OpenResource：接受冻结KMD的48字节allocation描述，
不再强求不存在的72字节TGSR资源数据，沿用消费者句柄和现有backing。
同一CPU断言旧实现失败、新实现通过，ARM64/x64构建通过；实机重启问题是否改善待测。
后续顺序为修复启动与稳定性、着色器缓存和实测约60Hz垂直同步、已知像素、硬件旋转。
静止桌面按需提交，不以持续空转制造“60帧”。
详见[实机记录](../../../tegra-x1-wddm/docs/TGD3D_NATIVE_20261113.md)。

## 早期修正

最新诊断改动补齐设备创建的五个阶段、DXBC/SPIR-V前端与NVK/NAK后端、三段
跨队列提交以及CPU Fence等待的进入记录。进入状态为`E_PENDING`，不能当作
返回失败或成功；旧的成功CreateDevice/Present判定保持独立。等待结束保留原始
VkResult，销毁明确标记为DESTROY，避免此前统一标记READBACK的误判。
ARM64构建、12项CPU控制和14条实时/离线ETW传输通过；超时/设备丢失控制确认
未完成对象仍保留且没有CPU伪造完成。该诊断UMD已完成第三轮静止桌面观察，
窗口工具和日志问题使开窗未验收；这些结果不表示开窗重启已修复。

- 修复 Windows 头文件顺序、DDI `ClearRenderTargetView` 签名和错误的资源标志名。
- 接通 NVK 对私有 instance 请求的读取；只针对 Windows GM20B，普通 NVK/Zink
  instance 保持原来的选择规则。不设置 DWM 进程环境变量，也不改变 NVK 的公开能力。
- 将 42 个 D3D 常量缓冲绑定改为普通描述符集快照，消除原实现与 NVK 32 个
  push descriptor 上限的冲突。每次更新重建完整有效绑定，旧快照跟随提交退休。
- 每次提交执行 runtime signal → Vulkan wait/render/signal → runtime wait。
  Vulkan 提交还等待上次完成值，提供跨提交内存依赖。传输、独立渲染段和读回
  显式设置 barrier；失败的提交不推进已提交值、不排入完成等待、不用 CPU 伪造完成。
- `DONOTWAIT` Map 和 QueryGetData 可提交待处理命令，但不进入帧数回压等待。
  正常 Flush/Present 保留最多两个未完成提交的回压。查询跨越 128 个渲染段时继续
  分配查询池，不再静默丢弃后续统计；事件/时间戳包含之前延迟记录的 clear。
- 共享资源沿用 TGSR v1 的 72 字节元数据、runtime 自己的 allocation handle、
  paging fence 和 owner 回调。创建后对 NVK 返回的实际 rowPitch/offset 也做核对，
  ICD 必须明确确认导入了 runtime backing。GPU 映射、CPU 锁、runtime allocation
  按顺序释放；生产者销毁不借用或销毁消费者句柄。
- 交换链 identity rotation 后，跨逻辑资源缓存的 image view 以及已经延迟销毁的
  view 均在其 backing image/memory 之前退休。部分初始上传失败和设备丢失路径
  也先等在途工作退出，再返回 runtime backing。
- Present 保留 opaque DXGI token、runtime context 和返回状态，interval 0 请求
  覆盖为 interval 1；设备丢失状态锁存。QPC 回压时长按频率换算为纳秒。
  **interval 请求与 CPU 控制不证明实际 60 Hz**，仍需 KMD VBlank 和物理画面证据。

动态 512 MiB GPU 预算、GPU PMIC 桥、睡眠策略均未更改。CPU、风扇、SD 不在本轮
实现或发布范围内。Direct Flip、DX12 和具体游戏适配不在此次交付范围。

## 构建

`--red-primary-control`是独立的短时红屏诊断构建，默认不启用。它将旋转Blt替换为
720×1280 BGRA目标的纯红clear，并在SetDisplayMode交出同一个runtime allocation
前执行clear、提交及真实Fence等待；不创建旋转shader或采样源图。继续使用GPU
bridge，不能与`--cpu-sync-control`合用。测试后使用原有恢复流程还原正常UMD。
ETW事件19的phase0只表示clear录制；phase1表示其等待结果，后续SetDisplayMode、
Present及DC锁存要分别核对。红屏成功只证明本轮简化路径可用；黑屏时若尚未清屏
完成或交出地址，不能直接认定KMD失败。它不构成旋转、60Hz或稳定性验收。

需要 Meson/Ninja、LLVM-MinGW、Windows SDK/WDK 10.0.26100.0。
`build_tgd3d.py` 将所需 WDK 头复制到独立输出目录，生成五个文件：

- `TegraGpuD3DVk.dll`
- `tgd3d-contracts.exe`：生产代码加 Vulkan/runtime 替身，仅 CPU 控制
- `tgd3d-runtime-check.exe`：实际 Windows D3D11 runtime / FL10_0 / LUID 检查
- `tgd3d-trace-export.exe`：tgd3d ETW JSONL 导出；live 使用原始 QPC，离线兼容 v1/v2
- `tgd3d-trace-control.exe`：仅生成合成 ETW，验证记录传输，不创建 D3D/Vulkan 设备

```sh
python tegra-x1-wddm/tools/build_tgd3d.py \
  --out /absolute/path/tgd3d-arm64 \
  --cross-file /absolute/path/windows-arm64.ini \
  --sdk-include /absolute/path/sdk/Include/10.0.26100.0 \
  --wdk-include /absolute/path/wdk/Include/10.0.26100.0
```

该入口明确使用 `-Dtgd3d=enabled`，关闭 Gallium、OpenGL 和 Vulkan 驱动构建。
Meson 选项是 feature 类型：`enabled` / `disabled` / `auto`。旧 boolean 配置目录
需要重新配置到新目录。非 Windows 的 `auto` 不构建本后端。
Zink UMD 和独立 ICD 构建入口明确禁用 tgd3d，各自只构建对应组件。

必须配套包含本轮 `nvk_embedded_instance_info` 消费端的 NVK ICD，放在 UMD 同目录。
只换 tgd3d DLL，不能让旧 ICD 自动理解这个私有请求。ICD 继续使用现有
`src/nouveau/vulkan/nvkmd/windows/build_icd.py`、相同 SDK/WDK 与 ARM64 工具链，
单独输出。源码、部署、实机验证和恢复由执行当前目标的窗口统一负责；KMD 先冻结于已追溯的 `.158`，只有故障证据支持时才单独修改。
具体已构建文件、提交和 SHA256 记录在交付清单，不用包号替代 UMD/ICD 身份。

## 着色器二进制缓存

tgd3d使用Shader Object，因此单纯启用NVK的pipeline cache不会覆盖此入口。
现在通过`vkGetShaderBinaryDataEXT`保存编译后的二进制，命中后以BINARY类型在当前
VkDevice重新创建Shader。DXBC反射和SPIR-V生成仍执行，缓存主要省去重复NVK/NAK编译；
不缓存或跨设备复用VkShader句柄。

复用本支线`16a0c400`已有的Win32 blob cache（该后端源码本轮不变）：后台低优先级写入、
原子发布、内容校验、跨进程协调和淘汰。tgd3d独立缓存目录的磁盘上限为128MiB，
与512MiB GPU预算无关。默认启用，沿用`MESA_SHADER_CACHE_DIR`和
`MESA_SHADER_CACHE_DISABLE`；目录不可用或缓存损坏时正常编译。
key包含SPIR-V、stage/nextStage、创建flags、描述符布局版本/槽位及shaderBinaryUUID/Version。
只有INCOMPATIBLE_SHADER_BINARY会删除旧条目并重新编译；创建中的OOM/设备丢失保持原错误。
导出缓存本身的OOM不使已创建Shader失败，若导出发现设备丢失则销毁尚未提交的新对象并报错。

参考Vulkan 1.4头文件revision354及2026-11-13核查的
[Shader Object二进制获取](https://docs.vulkan.org/refpages/latest/refpages/source/vkGetShaderBinaryDataEXT.html)、
[创建约束](https://docs.vulkan.org/refpages/latest/refpages/source/VkShaderCreateInfoEXT.html)和
[UUID/版本兼容约定](https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceShaderObjectPropertiesEXT.html)。
二进制输入/输出均按16字节对齐。ETW事件18的arg0为0未命中、1加载成功、2排入缓存写入；
arg1为二进制字节数。写入事件不代表落盘，实机须另测新进程/重启命中和实际编译耗时。
当前仅完成CPU控制与交叉构建，尚无Switch上的缓存收益和画面正确性验收。

## 垂直同步取样

`tgd3d-display-timing.exe --luid <本轮16位原生LUID> 180`在该适配器唯一的活动输出上
调用`IDXGIOutput::WaitForVBlank`，保存原始QPC和间隔中位数/p95/最大值。第一段不完整
刷新只用于对齐，不参与频率统计；没有Sleep制造的60Hz。工具不创建窗口、提交绘制、
Present或改变显示模式。取样数量30–600，超时只终止本工具。

两次`DwmGetCompositionTimingInfo(NULL,...)`记录合成/提交/显示计数；只有第二次查询
输出文档规定的区间计数。应在物理控制台会话执行，输出同时记录进程session与console
session，不能把SSH服务会话的DWM数据直接归给实屏。CPU时间属于取样线程，
wait调用耗时包含API本身的CPU成本；两者不等于GPU工作时间。

退出0仅表示取样完成。原生同LUID/DWM模块、真实约60Hz、扫描地址锁存、交互期间的呈现
和物理画面仍须结合本轮证据判断。静止桌面可以有60Hz扫描而没有60次新合成，不能为了
计数持续空转。该工具尚未在Switch运行。
参考微软[WaitForVBlank](https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgioutput-waitforvblank)
（2024-02-22）、[DwmGetCompositionTimingInfo](https://learn.microsoft.com/en-us/windows/win32/api/dwmapi/nf-dwmapi-dwmgetcompositiontiminginfo)
（2024-02-22）和[DWM_TIMING_INFO](https://learn.microsoft.com/en-us/windows/win32/api/dwmapi/ns-dwmapi-dwm_timing_info)
（2021-04-02），核查日期2026-11-13；不是用文档声明代替硬件测量。

## 验证边界

控制程序覆盖逐提交双向 Fence 和三处故障、DONOTWAIT 映射、42 个常量缓冲槽位、
绑定快照、交换链 view 生命周期、查询池扩展、真实 SM4 转译、非整齐尺寸的共享布局、
E_PENDING paging、独立 owner 销毁、忽略导入的 ICD、Present token/status。
这些测试不执行 GPU，也不模拟“物理上屏成功”。生成的 `tgd3d-vs.spv` / `tgd3d-ps.spv`
及带纹理采样/常量缓冲 13 的 `tgd3d-sample.spv`、含动态循环/CB 索引/裁剪的 `tgd3d-loop.spv`
另用 `spirv-val --target-env vulkan1.3` 检查。

也可把本地应用的独立 SM4.0 VS/PS 容器送入相同生产 DDI，导出转换结果：

```text
tgd3d-contracts.exe --export-dxbc input.dxbc output.spv
spirv-val --target-env vulkan1.3 output.spv
```

此入口检查容器、token 长度和 ISGN/OSGN 签名范围，仅导出前端结果，不运行合成
故障注入用例或加载 GPU 驱动。库函数须先由应用链接为完整程序；ISG1/OSG1、其他
shader model 及阶段不在该入口范围内。应用着色器及其派生产物留在本地，不随仓库发布。

Windows NVK 构建开启 `build-tests` 后，可构建 `nvk-tgd3d-shader-check`，将这些
SPIR-V 送进实际 NVK 描述符 lowering 和 NAK SM53 编码器。例如在控制文件目录运行：

```text
nvk-tgd3d-shader-check.exe vs tgd3d-vs.spv vs
nvk-tgd3d-shader-check.exe ps tgd3d-loop.spv loop
```

输出 `.bin` 和 `.nak.txt`。工具使用 GM20B 编译参数和 tgd3d 的三个普通描述符集合，
通过 NVK 已有的无 nvkmd 分支跳过上传；不枚举适配器、不创建 KMT 设备或提交 GPU 工作。
这是编译接受性检查，不证明指令在硬件上正确执行，也不能排除实际桌面着色器的问题。

ARM64 控制程序需要另行在 Windows 执行；本机 x64/Wine CPU 控制也不能替代它。
2026-11-12（洛杉矶时间）本机 x64/Wine 11.21 执行的 11 项控制输出全部通过，
三个生成的 SPIR-V 均通过 SPIRV-Tools v2026.3 的 Vulkan 1.3 校验。
控制中的 Vulkan dispatch 全部是替身；Wine 初始化时的 MoltenVK 适配器枚举不计作 GPU 测试。
最终文件身份见交付清单。历史 `.87` 离屏、`.158` 短时桌面及后续失败均只作背景，
不计入本后端验收；Code 0、成功链接、mock 通过均不计入原生通过。

现有功能边界：单样本、D3D10.0 DDI。MSAA/resolve、stream output/DrawAuto、
非空 predication、buffer RTV、旋转 DXGI Blt、packed depth/stencil 的 CPU 平面转换
尚未实现。对应入口返回不支持；A8 可通过 R8 swizzle 采样，当前不支持 A8 render target。
这些限制不允许描述为完整 FL10_0 兼容性通过。

## 实机验收顺序

1. 使用统一核对过 hash 的 KMD/UMD/ICD，单独注册 `TegraGpuD3DVk.dll`。
   先确认 DWM 加载了这个模块、NVK LUID 与真实 Tegra 适配器一致，再确认物理屏幕。
   不使用旧的 Zink UMD 注册记录作为 tgd3d 加载证据。
2. 五分钟正确桌面：开窗、移动、缩放、菜单。物理观察、截图和 ETL 分开记录时间。
   任意花屏、冻结或黑屏即本轮实屏失败，停止扩大验收并保留现场证据。
3. 在同一组件组合下验证着色器缓存冷/热启动、重启后命中及画面一致性；用真实VBlank、
   扫描地址锁存和队列回压确认约60Hz呈现，区分CPU/GPU工作和等待。静止桌面按需提交。
4. 在同一原生 LUID 上运行已知像素/共享采样读回；例如
   `tgd3d-runtime-check.exe --luid <16位LUID>`，再使用 `--shared-contract`、
   `--shared-layout`、`--sample`、`--load`。每次保存退出码、输出与组件 hash。
   这不是上屏证明，不能跳过前两步。
5. 确认硬件 1280×720 旋转及物理方向。
6. 用版本匹配的 Vulkan-Tools Win32 `vkcube` 持续呈现，同时记录实际 VBlank/Present
   节拍和丢帧；请求 interval 1 不等于已测得 60 Hz。

ETW provider 为 `{5f0c4c6e-7a0b-4b52-9d3a-2f6d1e8b4a11}`，当前事件version 2，解码保留version 1兼容。
主聊天可用 `logman` 记录后运行 `tgd3d-trace-export.exe trace.etl > trace.jsonl`。
TIMING 事件含 QPC 频率和提交值；导出程序对事件丢失、无匹配事件或格式错误返回失败。
物理帧节拍还需与 KMD 的 latched scanout/VBlank 证据对齐。

## 后续进入 main

本轮没有修改或推送任何 main 分支，也没有把 main 的 Zink 修正留在本工作树。
`2dca8a30` 的误合并已由 `c31c10a1` 撤销，保留了历史，不做强推。
因此后续**不要直接将包含该撤销的整条历史合并进 main**，否则会撤销主线工作。
在主线审阅仅针对 tgd3d 的实现提交/补丁，并将本实验分支的默认开启提交单独处理；
主线保留 Zink 默认路径。若届时选择整分支合并，应先撤销 `c31c10a1`，恢复当时被移除的
main 变更，再正常同步最新 main 并解决冲突；这一步只在获准合并时进行。合并前后应确认 Zink 源码、当前修正、驱动注册及 KMD 均保留。
这个说明不是现在执行合并或部署的指令。

## 来源与许可证

- `mesa-switch` 固定基线 `0ff52079ca9e9e837ed27eb95a8d239f439a1864`；复用现有
  NVK/NIL/NAK。NIL 的显式线性 row stride 与按 layer 的偏移由 ICD 负责，tgd3d
  不自行实现另一套 GM20B tiling。原有文件的许可证和版权保留。
- `dxbc-spirv` 固定 `bf14419e5fa7eacb817b7b632f03cb61d61bbad7`，SPIRV-Headers
  gitlink 为 `c8ad050fcb29e42a2f57d9f59e97488f465c436d`。
  [上游](https://github.com/doitsujin/dxbc-spirv/tree/bf14419e5fa7eacb817b7b632f03cb61d61bbad7)
  · [保留的 MIT 许可证](third_party/dxbc-spirv.LICENSE)。测试三角形继续使用仓库已有的
  Mesa/apitrace SM4 fixture，没有复制新的外部驱动实现。
- 微软 [GPU signal callback](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dumddi/nc-d3dumddi-pfnd3dddi_signalsynchronizationobjectfromgpucb)、
  [GPU wait callback](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dumddi/nc-d3dumddi-pfnd3dddi_waitforsynchronizationobjectfromgpucb)、
  [DXGI Present contract](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/dxgiddi/ns-dxgiddi-dxgiddicb_present)
  和 [Windows Driver Samples](https://github.com/microsoft/Windows-driver-samples/tree/main/video)
  用于核对 runtime/KMT 句柄、资源和呈现；KMDOD 是 display-only 参考，不提供本 UMD 的 GPU 实现。
- [Vulkan synchronization](https://docs.vulkan.org/spec/latest/chapters/synchronization.html)
  明确区分提交顺序、执行依赖和内存依赖。本实现请求 Vulkan 1.3；仓库 Vulkan-Headers
  为 1.4.354。匹配的 Vulkan-Tools `v1.4.354` 是
  `ec6931a360d89d08f1c7578fa43958fb3829b10a`；参考 Vulkan CTS `vulkan-cts-1.4.7.1`
  是 `0d18b347943e5e843529a05c95220e27fe30c538`。本轮未构建或运行它们，交主线按
  驱动实际声明的 API/扩展选择用例；不能把工具启动作为一致性通过。
- `lfrb/mesa wddm2` 参考固定 `801c9763c6043f0de8408e905a5324eea06d81d7`：
  [移植差异说明](../../../tegra-x1-wddm/docs/PORTING.md#radv-windows-reference)。本轮 GitLab
  原文访问不可用；不把 AMD KMD 的分配/提交私有数据搬进 Tegra 后端。
- [deko3D queue](https://github.com/devkitPro/deko3d/blob/373bfd43d8b2d805fa2eb7d406dbb823032df958/source/dk_queue.cpp)
  用于对照 GM20B 队列/同步方向；其 Horizon nvservices 生命周期不等于 WDDM callback。
- Switchroot 显示参考 `3c40adf1081096bc61a7c1280434c163e7e79b48` 的 `window.c`、
  `dc.c` 用于对照 pitch、更新/激活和 VBlank 读回。GPL-2.0 代码只作硬件参考，未移植。
