# Switch Lite 与 OLED 的 WDDM 查询复审

Switch OLED（HEG-001 / Aula）的 Windows ARM64 适配、移植源码和四项问题反馈来自 [Tsunami714!](https://github.com/Tsunami714a)。

对方的测试环境为 Windows 11 ARM64 23H2（22631）。源码包 `switch-woa-gpu-src-20261127.zip` 的基点为 `68c4dd319f`，SHA-256 为 `6ffe6e7ce7bb959c2354d11ec144380e6cdc663fbe4bc0e7d3ea3f439ecf373e`。本仓库的共用候选基于 `29707703bfe4`。

| 项目 | OLED 源码复核 | Lite / OLED 共用候选 |
| --- | --- | --- |
| ① 子设备枚举 | 移植记录提到一次 28 字节请求。包内既有上报一个面板的路径，也有上报零个子设备的诊断路径。缺少同次启动的原始返回值。 | 一个面板使用 56 字节，前 28 字节描述面板，末尾 28 字节全零。显示路径收到单槽即返回 `STATUS_BUFFER_TOO_SMALL`，缓冲保持原样。纯计算路径上报零个子设备，一个全零槽即足够。 |
| ② 面板查询 | 去掉 `Panel.Dc.owned` 后可以查询已释放显示控制器的面板，但移植版同时去掉了子设备状态与描述符的 UID 校验。 | 记录成功启动时上报的面板及其 POST 标识。枚举、连接状态、描述符和静态模式查询独立于当前 DC 所有权。只向已上报的内屏返回已连接，拒绝未知 UID。 |
| ③ 早期能力查询 | `windows/query.c:88` 仍无条件描述表段，先于 `display=TRUE` 分支。两个宏配置在表段为空时均返回 `0xC000000D`。随包 `tools/build.py` 只为 miniport 相关编译单元传面板宏，实机出包命令未附。 | 显示和纯计算包分别注册静态能力入口。无表段时两者均能返回 `DRIVERCAPS`，`ComputeOnly` 依次为 0 和 1。段、页表等依赖资源的查询仍等待资源就绪。 |
| ④ 能力结构大小 | WDDM 2.6 模型中三个输出分别为 4、584、24 字节。各少一字节时，移植版对 `WDDMDEVICECAPS` 报错，对 `DRIVERCAPS` 和 `PHYSICALADAPTERCAPS` 写出部分结构并报成功。后者还会在检查输入长度前读取物理适配器索引。 | 接受较大的输出，仅写已知结构。过短的输出返回错误。物理适配器索引先验长再读取。固定的 WDDM 版本允许仅带输出的查询。 |

微软规定子设备描述符数组包含一个额外的全零终止项。微软也明确规定，系统会在 `AddDevice` 与 `StartDevice` 之间查询 `WDDMDEVICECAPS`。[子设备枚举契约](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/dispmprt/nc-dispmprt-dxgkddi_query_child_relations) · [WDDM 版本查询时序](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/ns-d3dkmddi-_dxgk_wddmdevicecaps)

## 验证与准入

ARM64 WDK 10.0.26100 构建已链接 WDDM 显示版和 MCDM 纯计算版。使用同批构建对象，在 Apple Silicon 上执行 `test-context`、`test-panel-scanout`、`test-panel-timings`，三个 WDK 用户态模型均返回 0。覆盖表段建立前的查询、单槽和双槽、过短输出的写入边界、DC 释放后的面板标识和错误 UID。

共用改动可进入实机候选。该候选在 Lite 和 OLED 上的驱动加载、冷启动及桌面显示验收均待完成。OLED 冻结的根因仍待确认。判定单槽和旧版能力布局是否需要另行适配，须将同次启动上报的 `NumberOfChildren`、查询类型、输入输出字节数、返回状态和实际加载的 KMD 对齐。
