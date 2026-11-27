# 冷启动 LogonUI / DWM 并发捕获

当前冻结组合：`.158 + bit14 + b916f531` KMD、`d3423d26` ICD、冻结
`9de42c82 + 7f5f416b + 42ad5d1b` CPU 同步对照 UMD。仅新增捕获设置，
不改变 AllowNativeBoot、驱动文件、DDI 注册、CPU/风扇/电源设置。

`prepare.py <输出目录>` 生成 WinPE 离线注册表导入、配置与恢复文件。
本包针对已核对的 ControlSet001、Windows 系统盘 C:。配置前必须核对
Select、组件 SHA256、分区身份；目标键必须原先不存在，遇已存在项拒绝覆盖。
用 `x86_64-w64-mingw32-clang -O2 -Wall -Wextra -Werror -static -municode
setacl.c -o <输出目录>/setacl.exe -ladvapi32` 构建维护工具，将该目录放入
WinPE 的 `X:\tgd3d-update`。该工具按 SID 构造 DACL，避免 WinPE 无法解析
DWM 虚拟账号名称的问题。
在新建的 WinPE COW 中执行 `configure.cmd <WINARM 在 WinPE 的盘符>`，
成功卸载 hive 后审查差异；保存原扇区及文件/注册表备份，再写回并读回验证。
不把 Mac 本地模拟恢复的 hive 写回设备。

- GlobalLogger：进程/线程开始退出、DLL image load、CSwitch；QPC 时钟。
- 三个 AutoLogger：tgd3d API/提交/CPU 等待，DXG/DWM/DXGI 图形同步，
  User-Loader/Winlogon/CodeIntegrity。使用现有已验证 provider 与目标系统登记的 GUID。
- 每秒刷 ETW 缓冲；循环 ETL 分别 64/32/64/32 MiB，每种最多保留四轮。
  每会话 16–64 个 64 KiB 缓冲；没有启用高频 CPU 采样或全盘文件跟踪。
- WER 仅对 `dwm.exe` 和 `LogonUI.exe` 配置完整用户态 dump，各保留两个。
  DWM 虚拟账号组只能写 DWM 的 dump 目录；LogonUI 的 dump 仅 SYSTEM/管理员可读写。
  不安装 IFEO Debugger、不打开 loader snaps、不修改超时或错误恢复行为。

文件位于 `C:\ProgramData\SwitchWOA\ColdBootTrace-20261113`。
用户确认修复后，由本窗口在线以管理员运行 `Restore.ps1`：停止会话，删除本包新增键，保留证据和
原生启动；随后核对重启不再自动采集。修复确认前不因单轮日志收集结束而自动移除配置。
离线恢复时先把系统 hive 加载为 `TgdBootSYSTEM` / `TgdBootSOFTWARE`，
使用 `Restore.ps1 -Offline`，随后卸载 hive。Windows 再次启动后不能盲目写回旧扇区。
不含 PowerShell 的 WinPE 可在加载上述 hive 后，先核对目标键的 FileName/DumpFolder
仍指向本轮目录，再用 `reg import disable-offline.reg` 删除本轮六个键，最后卸载 hive。

分析时先看 session/provider Status、ETL 丢事件/截止时间及启动轮次，按 PID/TID、
进程开始时间和 QPC 对齐。成功 image load 不能代替加载失败返回码；等待开始无结束
也不能单独证明死锁。用实际 Loader/DXG 返回码、tgd3d 等待 sequence/完成值、
CSwitch 和可得 dump 调用栈互证。硬复位仍可能丢失末尾缓冲或没有 dump，
不能据此虚构最后指令栈。当前实机 UMD 是 CPU 同步对照，不能声称在测试双向 GPU Fence。

参考：Microsoft Learn 的 [AutoLogger](https://learn.microsoft.com/en-us/windows/win32/etw/configuring-and-starting-an-autologger-session)、
[GlobalLogger](https://learn.microsoft.com/en-us/windows/win32/etw/configuring-and-starting-the-global-logger-session)、
[WER LocalDumps](https://learn.microsoft.com/en-us/windows/win32/wer/collecting-user-mode-dumps)，
SDK 10.0.26100.0 `evntrace.h`；目标 Windows 22621/22631 系列。
