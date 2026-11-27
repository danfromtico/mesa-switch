# tgd3d ARM64 可还原实机测试包

本包由 tgd3d-vulkan 支线打包。UMD/ICD 使用该支线构建；KMD 固定复用
支线 NATIVE_DESKTOP_158.md 记录的、曾完整任务栏上屏的签名 .158：
源提交 6c854e9f394c3d761fab85885bead4bbd4588581，SYS SHA256
46d1757674ff156028ce5b729ba53671981ffab09948fcaf125751c46bb10088。
所有组件的独立源码提交、长度和 SHA256 见 trial-components.json。
不包含主线 .161 KMD、Zink UMD 或 CPU/风扇/SD 驱动更新。

## 准备与启动

1. 在 Switch Windows ARM64 的管理员终端运行 PREPARE.cmd。
   基线为冻结 .158 的关闭启动门控状态（Code 43、BasicDisplay可操作）；首次
   迁移也接受已知 .59 / Code 0。CPU保持1497600 kHz / 风扇13。
   工具会复制包到受保护的 ProgramData 目录，导出当前签名驱动包，
   保存 ACPI、交互桌面显示设置和电源设置，并校验备份。此步不装候选驱动。
2. 进入输出的 PREPARED_DIRECTORY，再运行 START-TEST.cmd。
   已在冻结 .158 / Code43 且ACPI匹配时，直接在当前Windows启动中观察并启用
   显卡设备，不重装驱动或重启Windows；首次从.59迁移才需要初始重启。
   原生启用后进行约30秒静止桌面观察。使用
   `Start-NativeStage.ps1 -Stage Window -PreviousPhysicalObservation "实际观察"`
   运行约60秒开窗、移动、缩放、关闭，再以同样方式指定 `-Stage Interaction`
   进行五分钟交互（包含遮挡、最小化/恢复、菜单、快捷设置和性能程序回归）。
   每段在同一次原生启动中继续。已经启用的 .158 直接使用阶段入口，
   不要再次安装或运行初始启动。阶段结束和失败均不自动回退或重启。
3. 静止、开窗、退出和恢复检查可自动执行，无需每段人工确认。缺少实屏反馈时
   只能记自动检查通过。Code 0、ETW Present、截图均不能单独证明
   面板上屏。evidence/stage-*.result.json 的 API 结果与物理反馈分开记录。

## 还原

- 仅在明确需要恢复时，在准备后的目录运行 RESTORE.cmd（管理员）。
  会撤销本次原生启动许可及临时UMD/ICD注册，保留冻结的
  .158驱动与匹配ACPI。重启后预期Code 43（启动门控关闭），BasicDisplay提供
  可操作桌面，再恢复并校验显示设置及CPU/风扇/电源状态。不会自动安装.59。
- 不注册自动回退定时任务，正常阶段结束后保留当前原生 .158。
  运行旧包前应清除其遗留的自动恢复任务，避免旧计时器导致额外重启。
- 如果内核完全卡死，软件定时任务无法执行。重启后一次性许可和挥发性门控会
  阻止再次原生启动；需要恢复时再显式运行手动工具。
- recovery-verified.json 的 Verified=true 才代表重启后校验完成。
  如果失败，保留 restore-error.txt 和还原目录，不能把 restore-staged 当作还原成功。

## 证据与范围

保留整个准备目录的 JSON/JSONL、ETL、日志、evidence/ 和 recovery.json。
收集前停止本包 ETW 会话；离线可用 app/tgd3d-trace-export.exe 解码 ETL。
解码器也支持 --follow，使用 tgd3d 独立 provider，不使用 Zink loader 日志判断成功。
UMD provider现在使用启动门控打开前创建的独立`Tgd3dTrialUmd`会话，输出
`umd.etl`；内核/DXGI/DWM继续保留在`Tgd3dTrialDxg`。两者分别记录丢失计数，
不会把内核会话丢失混作UMD会话丢失，也不忽略UMD自身的任何丢失。
按用户最新要求，恢复前的Dump抓取已停用，不继续查找新日志/Dump定位故障。
正常启动也不再要求内核autologger或trace-flush辅助进程；只启动确认当前DWM
身份所需的小型UMD会话。手动取证工具保留，不作为启用/开窗前置条件。
当前主线工作是静态源码对比和单变量实机对照；已有工具保留供明确需要时使用。
事件8仅记录资源创建入口；事件17按资源地址和PID/TID区分DDI、VkImage创建、
内存分配、绑定及runtime导入，E_PENDING为进入，返回状态才表示该调用结束。
窗口工具在Shown时显式ShowWindow，并记录Win32可见状态；它仍不替代物理观察。
已复用的工具来源记录在 tool-provenance.json；打包不会构建主线或合并 main。

回退后的低开销SD/内存排查使用主机端 `../collect_recovery_health.py`，指定
`--host`、已有密钥/HostKeyAlias和新的`--out`目录。它只读固定的SD设备、
注册表、文件哈希、内存及限条数的事件记录，不安装任何组件。
2026-11-13临时`sd-dump-state.ps1`曾在SSH超时后持续占用内存，禁止复用；
客户端超时不代表远端进程终止。新入口遇到错误即停止并保留原始输出，
不能把命令退出0当作SD转储或桌面实屏验收通过。
