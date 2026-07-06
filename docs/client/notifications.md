# 桌面通知（Dynamic-Island Toast）

出货客户端 `tools/preview-d2d/toast.{h,cpp}` 提供一个**顶部居中的 Apple 动态岛（Dynamic-Island）风格胶囊**
通知。它从窗口上边缘弹入，可从瞬态药丸扩展成高卡片 / 常驻广播。本页描述其状态机、时间线与调用接口。

## 外观与生命周期

两条正交枚举 + 附加字段扩展基础胶囊（`toast.h:18-19`）：

- **`Variant`** = 长什么样：`Toast`（药丸）/ `Expanded`（高卡片）/ `Broadcast`（常驻卡片）。
- **`Anim`** = 上一次内容变化怎么动：`Drop / Expand / Swap / Flip`，驱动 tick 子状态 + paint 变换。

**`Phase`** 是容器时间线（`toast.h:25`）：`Hidden → Enter → Hold → Exit`。

几何常量（全 DIP，`paint(W,H)` 空间，`toast.cpp:13-21`）：胶囊高 `kH=36`、圆角 `kR=半高`（真胶囊）、
静止 y `kTopY=14`（贴顶边）、隐藏 y `kHiddenY=-46`（完全藏在客户端上方）；卡片态高 `kHCard=84`、圆角 `kRCard=20`。

## 时间线（tween）

完整入场（`Phase::Hidden` 时的 `show`，`toast.cpp:74-79`）：

| 阶段 | 动作 | 时长 / 曲线 | 证据 |
|---|---|---|---|
| **Enter · 下滑** | y 从 `kHiddenY` 落到 `kTopY`，**easeOutBack 回弹** | 0.50s / easeOutBack | `toast.cpp:75` |
| **Enter · 宽度 morph** | width 0→1 morph-open（paint 里 `lerp(wMin, fullW)`） | 0.44s / easeOutQuint | `toast.cpp:76` |
| **Enter · 淡入** | fade 0→1 | 0.20s / easeOutCubic | `toast.cpp:77` |
| **Hold** | 停留（`live` 累加，Broadcast `sticky` 无限） | ~2.4s | `toast.h:3,38` |
| **Exit · 上滑** | y 回 `hiddenYForCurrentHeight()` | 0.34s / easeOutCubic | `startExit`，`toast.cpp:47` |
| **Exit · 淡出** | fade →0 | 0.30s / easeOutCubic | `toast.cpp:48` |

**顶部锚定 + 宽度按文字 morph**：width 是 0..1 进度，paint 每帧按测量出的文字宽度 `lerp` 到 fullW
（`toast.h:32`）；隐藏 y 按当前展开高度算（`hiddenYForCurrentHeight`，`toast.cpp:24-28`），药丸 = -46、卡片按
`curH`。岛已存活时切换内容**不重播整段下滑**，只做小幅 re-morph 到新文字宽度（`toast.cpp:68-72`）。

## 接口与调用点

| 函数 | 用途 | 证据 |
|---|---|---|
| `show(s)` | Drop-in 瞬态药丸（默认入口，逐字节不变） | `toast.h:44`、`toast.cpp:51` |
| `showBroadcast(title, body, accent, sticky)` | 常驻扩展横幅（Drop-in + expand，`sticky` 默认 true） | `toast.h:48`、`toast.cpp:82` |
| `swap(s)` / `swapExpanded(title, body)` | 交叉淡入淡出可见内容（药丸 / 卡片） | `toast.h:55-56`、`toast.cpp:120,134` |
| `flip(s)` | scaleY 挤压翻转（药丸） | `toast.h:57`、`toast.cpp:150` |
| `dismiss()` | 动画离场（常驻广播或任意存活岛） | `toast.h:58`、`toast.cpp:164` |
| `tick(dt)` / `paint(app, W, H)` | 每帧驱动 + 绘制 | `toast.h:62-63` |

- `accent`（`toast.h:30`）：0 → `pal.primary`，否则如 `pal.status_busy`（alert 用）。
- Swap/Flip 是两段式：Phase A 淡出/压扁旧内容，中点 `midDone` 闩锁（`toast.h:37`）后由 `tick` 落定 `pending*`
  暂存并淡入新内容（`applyPending`，`toast.cpp:31-41`）。
- `swap` / `flip` / `swapExpanded` 在岛隐藏（`Phase::Hidden`）时退化为普通 `show` / `showBroadcast`
  （`toast.cpp:121,135,151`）。

## 关键文件索引

- `tools/preview-d2d/toast.h` —— 枚举 / `Toast` 结构 / 公共接口。
- `tools/preview-d2d/toast.cpp` —— 时间线 tween、状态迁移、`applyPending`。
- 渲染依赖 `render/primitives.h`（圆角胶囊 fill）与 `palette.h`（accent 取色）。
