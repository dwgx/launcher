# 视觉冒烟测试层（可分离）

`tools/preview-d2d/visual_smoke.{h,cpp}` 是一个**可插拔、编译期门控**的视觉冒烟钩子：编出带钩子的
`LauncherD2D.exe`，跑一遍就把 **17 个核心屏**逐屏截成 PNG，再用 `visual_smoke_analyze.py` 做**区域级布局
回归体检**。它绕过 autologin 与所有 live fetch，自带冻结帧循环，产出确定性截图。与只测后端 API 的
[Smoke / 审计](../AUDIT_WORKFLOW.md) 互补——一个看后端行为，一个看客户端像素。

!!! note "可分离是硬约束"
    整个 TU 都在 `#ifdef LAUNCHER_VISUAL_SMOKE` 之内（`visual_smoke.h:13`）：宏未定义时（正常 `build_d2d.bat`）
    本文件编译为**空翻译单元**，不向运行期路径泄漏任何符号，call-site 也被 `#ifdef` 掉。整块移除只需删
    **3 处**：`visual_smoke.{h,cpp}` + `build_d2d_visual.bat` + `main.cpp` 里那段 `#ifdef` 块（含 `#include`），
    移除后 `grep visual_smoke` / 宏名应为 0（`visual_smoke.h:9-10`）。

## 接入方式

- **构建**：`build_d2d_visual.bat` 是 `build_d2d.bat` 的克隆 + `visual_smoke.cpp` + `/DLAUNCHER_VISUAL_SMOKE`
  （见脚本头注）。**不改** `build_d2d.bat`，保正常构建纯净、宏未定义、钩子编译为空。
- **触发**：`visual_smoke::requested()`（`visual_smoke.h:20`）当环境变量 `LAUNCHER_VISUAL_SMOKE` 非空或命令行含
  `--visual-smoke` 时为真。
- **唯一 call-site**：`main.cpp:826-830` 的 `#ifdef` 块——`requested()` 为真则调 `visual_smoke::run(g_app, hwnd)`
  并直接返回其 exit code，**绝不进正常消息循环**（也就绕过 autologin 与 live fetch）。
- **产物**：PNG 落在 `tools/preview-d2d/.visual-smoke/`，或 env `LAUNCHER_VISUAL_SMOKE_OUT` 指定目录。

## run() —— 确定性冻结帧循环

`run()`（`visual_smoke.cpp:212`）自带 `beginFrame→stages::paint→capture→endFrame` 循环，**从不调用任何
tick**，所以 paint 是冻结全局的纯函数 → 确定性。它把窗口设成 1100×720 dip（不 `ShowWindow`，隐藏窗口下
`GetBuffer(0)` 照常可用，`visual_smoke.cpp:215-224`），然后：

- **`seedFixtures()`**：填确定性的假数据（绕过 live fetch）。
- **`freezeToMain()`**：tween 强制置 1、`g_stage=Main`、清头像等——消除入场动画与异步态。auth 屏另外强制
  auth 卡片入场 tween 完成（否则 `op<=0.001` 直接 return，`visual_smoke.cpp:257-259`）。

!!! tip "shot() 手动 settle 异步解码"
    Wave 1 起解码走后台线程，而 smoke **无消息泵**。`shot()`（`visual_smoke.cpp:234-254`）因此手动 settle：先画
    一帧让 paint 入队解码 → 有界地 `Sleep(15)` + 主动 `drainCompleted()`，直到 `cacheSize()`（条目数+字节）连续
    两次无变化（上限 ~600ms），最后一帧才截图。保证 cover / 头像等异步图在截图里是**真图而非占位**。

## capture() —— 抓帧到 PNG

`capture()`（`visual_smoke.cpp:68`）必须在 `stages::paint` 之后、`endFrame()` 之前调（target 只在此窗口活着）：
`ctx->Flush` → `GetTarget()` 拿到 live back-buffer 的 `ID2D1Bitmap1` → 因 back buffer 是
`TARGET|CANNOT_DRAW` 非 CPU-readable，建 `CPU_READ` 副本 `CopyFromBitmap` → `Map(READ)` → WIC 编码 PNG
（`visual_smoke.cpp:71-113`）。像素格式是 **premultiplied BGRA**（`GUID_WICPixelFormat32bppPBGRA`），与 back
buffer 精确匹配，`WritePixels` 即 memcpy。

## 17 屏覆盖

| # | 屏 | 基于 |
|---|---|---|
| 01/02 | auth login / register | Auth stage |
| 03..09 | home / lunching / chat / market / settings / profile / cloud | Main 各 view |
| 10 | 账户下拉浮层 | Home |
| 11..16 | 编辑状态 / 编辑 bio / 用户资料 / 改密 / 确认 / 搜索 modal | overlay |
| 17 | 表情/贴纸选择器 overlay | Chat |

（scenario 列表见 `visual_smoke.cpp:263-317`；scenario 之间 `resetOverlays()` 回到干净基底。）

## visual_smoke_analyze.py —— 区域级布局体检

`visual_smoke_analyze.py` 读钩子导出的 PNG 做区域级布局回归（`visual_smoke_analyze.py` 头注）：

- **合成到已知深底**：D2D 导出的是 premultiplied BGRA（带透明圆角），先合成到客户端深底 `(18,17,16)` 再分析，
  消除圆角透明对占比统计的干扰（`load_rgb`）。
- **分区占比**：侧栏（左 64px）、顶栏（上 56px）、内容区各自 ink 占比（`analyze_one`）；内容 ink 阈值 `INK_THRESH=36`。
- **包围盒 + 重心偏移**：ink 包围盒、垂直重心相对画面中心的 `voff`。
- **居中/对称判定**：左右留白接近=居中卡片（正常），差距大=真偏移（左偏/右偏 flag）。
- **空白/缺失 flag**：主壳屏侧栏占比 <1% → 「侧栏疑似缺失」；内容区 <0.5% → 「内容区近空」。
- **可选 golden 像素 diff**：`--golden <gdir>` 与基准图逐像素回归；`--json report.json` 附机读报告。

用法：`python analyze.py <png_dir> [--json report.json] [--golden <gdir>]`。

!!! warning "为什么靠量化脚本而非直接看图"
    harness 对 premultiplied PNG 的 `Read` 会返空（透明通道预乘导致解析异常），所以判定不靠人眼/模型读图，而靠
    `analyze.py` 输出的**量化指标**（分区占比、包围盒、重心、对称、可选像素 diff）做回归。

## 关键文件索引

- `tools/preview-d2d/visual_smoke.h` / `.cpp` —— 钩子（全部 `#ifdef` 门控）。
- `tools/preview-d2d/build_d2d_visual.bat` —— 带钩子的构建脚本（`build_d2d.bat` 保持纯净）。
- `tools/preview-d2d/visual_smoke_analyze.py` —— 区域级布局体检 + 可选 golden diff。
- `tools/preview-d2d/main.cpp:826-830` —— 唯一 call-site。
