# .158 + tgd3d 启动包

当前实机使用已验收组件的永久原生开机许可。新组件先使用一次性许可。已知故障、日常绕行范围和当前组件哈希由仓库保存。[开放故障登记表](../../docs/open-faults.json) · [已验收组件](../../docs/graphics-baseline.json)

## 基线选择与永久许可

在仓库根目录运行 `pwsh -File tegra-x1-wddm/tools/Select-GraphicsBaseline.ps1 -Action CheckLedger` 可检查故障记录、历史和证据链接。用 `python3 tegra-x1-wddm/tools/check_graphics_baseline_push.py --install` 安装本地推送检查。它检查即将推送的提交，普通源码和实测记录仍可独立提交。GitHub 的 Switch 专用工作流会在拉取请求和推送时复核。

使用 `python3 tegra-x1-wddm/tools/package_graphics_policy.py --out <输出目录>` 从当前仓库打包策略。将包传到 Switch 后，用管理员 PowerShell 运行包内的 `Install-GraphicsBootPolicy.ps1 -ManifestSha256 <打包返回的哈希>`。安装器在 Program Files 保存按哈希验证的只读快照并切换活动指针。日常入口为 `C:\Program Files\SwitchWOA-GpuBoot\Native-Boot.ps1`。`Disarm` 撤销许可，`Trial` 只准下一次启动，`Restore` 按设备上先前验收的目录和完整哈希恢复。

在已有永久许可的设备上，首次安装会核对原许可、组件、Code 0 和正在运行的 DWM。用新入口恢复当前已验收包并读回许可后，再运行同一安装命令并加 `-RetireLegacy`。已核对哈希的旧入口保存在 `retired-policy-source`，原命令路径退出使用。后续试用与恢复脚本指向新入口。

新基线的验收 JSON 须列出 `base_commit`、`source_commit`、该 Git 范围的 `changed_paths`、`change_kind`、组件目录和完整哈希。`trial` 记录一次性许可、Code 0、外接电源、开机记录号和实际加载组件。`system_log` 记录有覆盖的系统日志区间、启动记录、异常事件和每次重启的解释。`cases` 记录专项实机结果和前后开机记录。每条用例和重启解释附上相对于该验收文件的证据文件名及 SHA-256。格式样例和拒绝用例见[门槛测试](tests/Test-BaselinePromotion.ps1)。

校验新候选用 `Select-GraphicsBaseline.ps1 -Action Check -Qualification <仓库内验收文件>`。有开放故障时，独立修复还需在登记表的 `independent_qualifications` 中绑定该验收文件的 SHA-256，并逐项给出与开放故障路径独立的理由和实机用例哈希。实机上给新组件永久许可用 `-Mode Accept -Directory <目录> -VerifiedKmd <哈希> -VerifiedUmd <分发 DLL 哈希> -VerifiedIcd <哈希> -Qualification <验收文件> -LedgerSha256 <仓库故障表哈希>`。入口会再核对活动策略、完整组件、实际 DWM、一次性启动记录和系统事件，检查通过后才写许可。

在仓库用 `-Action Select` 和同一验收文件更新基线记录。提交选择结果和实机证据后，`-Action Publish` 会再次校验将要推送的源码和数据。已确认根因的呈现修复可沿用旧基线已有的开放故障。用 `verified-fix` 绑定该修复的销案证据、同次候选启动的三次原场景验收和一个独立的正常路径对照。在故障表的 `baseline_comparisons` 中登记本次验收哈希、修复前的仓库提交、当时已开放的故障编号，以及旧基线验收文件和原有正常路径用例的哈希。设备端将旧基线验收文件作为 `previous-qualification.json` 随新回执传入。已在仓库选定的同一份组件可在之后的一次性启动上激活。设备核对原验收哈希、当前实际加载的组件、外接电源、Code 0 和本次许可后的完整启动记录，分别保存功能验收和实际激活的启动号。新增故障或候选验收期间的异常关机阻止该次晋升。其他呈现候选继续按其对应的开放故障和独立验收检查。涉及 x64 或 x86 用户态组件更新，在本次候选启动的相应用例中用 `loaded_components` 记录进程实际加载的模块哈希。已验收包可按原始哈希恢复。

## 当前缓存更新

2026-11-14已在活动`.158`组合上接入shader-object磁盘缓存，双向GPU Fence开启。
完整当前组件和验证范围见[交接记录](../../docs/TGD3D_COLD_BOOT_HANDOFF_20261113.md)。

`Update-ShaderCache.ps1 -CandidateDirectory <目录> -Restart`仅用于该次缓存专用UMD
更新。候选目录需包含`update.json`、ARM64 UMD、配套控制程序、同候选的原生CPU控制
结果`controls-state.json`，以及两个更新/恢复脚本。清单固定更新前的目录和组件哈希，
不接受任意版本。更新器复制已有受保护包到新目录，替换UMD及配套控制程序，保留KMD、
ICD、恢复文件和平台设置；切换注册及登录任务，下一次重启生效，不覆盖已映射DLL。

缓存使用当前令牌的LocalAppData下`mesa_shader_cache_win32-*`目录，每个缓存目录
上限128MiB；这是磁盘容量，不占用512MiB GPU预算。缓存命中仍在当前VkDevice上
重建shader object，不跨设备复用句柄。DXBC→SPIR-V转换仍会执行；缓存主要节省NVK/NAK
编译。源码已有缓存不代表旧冻结UMD已启用，构建必须同时包含缓存源码及
`ENABLE_SHADER_CACHE`后端。

本轮需要撤回时，在**新包目录**以管理员身份运行`Restore-ShaderCache.ps1 -Restart`。
它恢复保存的旧UMD/ICD注册、包目录和登录任务，重启后加载旧UMD；保留原`.158` KMD、
ICD二进制和频率，不安装`.59`。恢复入口与更新后的实际路径写入交接记录。
脚本及哈希核对通过；未为验证恢复工具而额外回退、重启或重复安装。

## 历史冷启动诊断包

本包使用清单中指定的 `.158` KMD 和配套 ARM64 NVK ICD。
`shader-output-bit14-native-boot` 变体在冻结 `.158` 上只移植 bit14 修复及显式
持久原生启动门控；附带原签名 KMD 的恢复文件。UMD为CPU同步对照版，双向GPU
Fence桥接仍屏蔽。该组合用于用户测试，稳定性尚未验收通过。

1. 当前应为已安装的 `.158 / Code43` 门控桌面，以管理员身份运行 `INSTALL.cmd`。
2. 清单 `startup_mode: native-boot` 通过下一次受控重启试用候选。后续启动按上方基线门槛授予永久许可。
3. 不会自动开测试窗口、采集Dump、回退`.59`或重启Windows。CPU、风扇和睡眠设置保持。

旧 `logon-default` 包仍在登录后约5秒启用，不属于直接原生开机模式。
此前使用一次性许可加启动任务续写的方案已被替换；不能用于持久原生启动验收。

需要恢复时，以管理员身份运行 `RESTORE.cmd`。它先关闭自动任务、清除
`AllowNativeBoot` 与测试许可；如原生 GPU 正在运行，会提示手动重启。
重启后再运行一次，恢复原签名 `.158` 并保持启动门控关闭，不热切换活动GPU，
不安装`.59`。完整组件来源和SHA256见 `trial-components.json`。

`shader-output-bit14-gpu460` 是GPU460.8MHz的电源对照，初始使用登录后启用。
它保留800mV固定下限，CPU及UMD/ICD不由KMD修改；内存1600MHz通过WinX/Hekate
启动配置设置。安装前必须关闭当前自动启用任务并重启到`.158 / Code43`，不在
原生桌面上热切换KMD。启动参数中的`BootGpuMeasuredKhz`来自启动时硬件计数器，
`BootGpuRailUv`为启动时PMIC设定回读，不代表负载瞬态电压。
按用户指定顺序，桌面测试通过后才将同一组合切换为`native-boot`，进行冷启动测试。
`Native-Boot.ps1`可在匹配的活动`.158`上配置下一次启动并停用登录任务，不重启显卡。

该对照包的`RESTORE.cmd`恢复带bit14的729.6MHz基线SYS（SHA256以`c629328e`开头），
不是更早的原始`.158`。恢复后先保持门控桌面；需要恢复原来的登录后tgd3d启动时，
从原基线包运行`Install-Default.ps1 -Prepared`，再按其提示启用。
