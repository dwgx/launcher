# Launcher Animation Brief — 给 claude design 用的设计提示词

> 这份文档是给 UI/动画设计师（claude design）的输入。读者已经看过 [tools/preview](../tools/preview/) 的截图。
>
> 项目要求 **120fps 不掉帧**，所以一切动画必须 GPU 友好（transform/opacity 优先，避免逐帧重绘大区域）。
> 客户端用 Skia 实现，Skia 的合成层等价于浏览器 will-change: transform / opacity。

---

## 1. 全局动画哲学

**Claude Desktop / Linear / Raycast 那一类"丝滑"** 的共同点：
- 所有过渡走 **同一根缓动曲线**：`cubic-bezier(0.16, 1, 0.3, 1)`（≈ easeOutQuint）
- 时长统一档位：`Fast 150ms / Default 200ms / Slow 350ms / Big 500ms`，禁止 `230ms` `260ms` 这种半档
- **stagger（错位）** 而非 **simultaneous**：列表里相邻元素延迟 30~80ms 入场，营造"风推过来"的层级
- 不允许 linear；不允许 easeIn 单独使用（除非匹配 easeOut 做对称）；不允许 bounce 太多（只在按钮 press 用一次轻微 spring）
- 任何动画必须 **可中断**：用户在动画进行中点别的，旧 tween 取消而非排队

**禁忌**
- ❌ 任何颜色变化 < 100ms（看起来像故障）
- ❌ 任何位移 > 32px（除非是 view 切换的整页平移）
- ❌ shadow 突变（必须 200ms 过渡 elevation）
- ❌ 多动画同时争夺注意力（一次只 1~2 个 focal element）

---

## 2. 阶段动画清单

### 2.1 启动 → 加载小卡 (200×200)
| 元素 | from | to | duration | curve | delay |
|---|---|---|---|---|---|
| 卡片 scale | 0.85 | 1.0 | 400ms | easeOutBack | 0 |
| 卡片 opacity | 0 | 1 | 300ms | easeOutCubic | 0 |
| Spinner 旋转 | 0° | 360° | linear 持续 | — | — |
| Caption 浮入 | y+8 / a=0 | y / a=1 | 240ms | easeOutQuint | 120ms |

> 备注：scale 用 easeOutBack 让卡片有"轻微过冲再回正"的物理感；其余用 easeOutQuint 保持柔。

### 2.2 加载完成 → 主窗口扩张
| 元素 | from | to | duration | curve | delay |
|---|---|---|---|---|---|
| 加载卡片 fadeOut | a=1, y | a=0, y-14 | 300ms | easeOutCubic | 0 |
| 窗口宽 | 200 | 1100 | 550ms | easeOutQuint | 100ms |
| 窗口高 | 200 | 720 | 550ms | easeOutQuint | 100ms |
| 主背景 fade | a=0 | a=1 | 350ms | easeOutCubic | 200ms |

> 整段约 700ms。窗口尺寸 tween 跟 SetWindowPos 每帧推进一次（要 60fps 同步）。
> 用户视角：小卡上滑消失 → 边框膨胀拉成大窗 → 内容渐显，**像折纸展开**。

### 2.3 主界面入场 stagger
| 元素 | from | to | duration | curve | delay |
|---|---|---|---|---|---|
| 侧栏 slideIn | x=-200 | x=0 | 450ms | easeOutQuint | 50ms |
| 侧栏菜单项（4 项） | a=0,x-12 | a=1,x=0 | 200ms | easeOutCubic | 80+40n ms |
| 顶栏 slideDown | y=-48 | y=0 | 400ms | easeOutCubic | 100ms |
| HomeView 标题 | a=0,y+12 | a=1,y=0 | 300ms | easeOutQuint | 200ms |
| HomeView 大头像卡 | a=0,y+16 | a=1,y=0 | 300ms | easeOutQuint | 240ms |
| HomeView 详情卡 | a=0,y+16 | a=1,y=0 | 300ms | easeOutQuint | 280ms |

> 关键：**delay 阶梯**。眼睛感受到的"层级"完全靠 delay 制造，不靠 duration。

### 2.4 view 切换（点侧栏菜单）
| 元素 | from | to | duration | curve |
|---|---|---|---|---|
| 当前 view fadeOut | a=1 | a=0 | 120ms | easeOutCubic |
| 新 view fadeIn | a=0,y+8 | a=1,y=0 | 240ms | easeOutQuint |
| sidebar active 指示条 | x_old | x_new | 280ms | easeOutQuint |

> 总共 ~250ms 不要更长。指示条用 spring 也行（1 次轻微过冲）。
> **不要做横向 slide**（"轮播图"风格不适合 launcher）。

### 2.5 头像点击 → 下拉
| 元素 | from | to | duration | curve |
|---|---|---|---|---|
| dropdown 容器 opacity | 0 | 1 | 180ms | easeOutCubic |
| dropdown 容器 y | -6 | 0 | 180ms | easeOutCubic |
| dropdown 容器 scale | 0.96 | 1.0 | 180ms | easeOutBack |
| 关闭：opacity+scale 反向 | — | — | 150ms | easeOutCubic |

> transform-origin 必须是 **右上角**（顶点对准头像中心）。
> 关闭比开启短 30ms（消失要利落，否则发"粘"）。

### 2.6 历史登录 overlay
| 元素 | from | to | duration | curve |
|---|---|---|---|---|
| 遮罩 opacity | 0 | 0.55 | 250ms | easeOutCubic |
| 卡片 opacity | 0 | 1 | 250ms | easeOutCubic |
| 卡片 y | +12 | 0 | 250ms | easeOutQuint |
| 卡片 scale | 0.98 | 1.0 | 250ms | easeOutQuint |
| 列表行 stagger fadeIn | a=0,x-8 | a=1,x=0 | 200ms | easeOutCubic | 50+25n ms |
| 关闭 (ESC / 点外部 / X) | 反向 | 反向 | 180ms | easeOutCubic |

### 2.7 主题切换
| 元素 | from | to | duration | curve |
|---|---|---|---|---|
| 全局色调插值 | light_palette | dark_palette | 200ms | easeOutCubic |
| 卡片 / 文字 / 图标颜色 | RGB lerp | RGB lerp | 200ms | easeOutCubic |
| 阴影 alpha | 14 | 80 | 200ms | easeOutCubic |

> 不允许"翻转一下"或"扫光过场"。**所有颜色同步 lerp**，让眼睛感受到"灯调暗了一点"的物理感。

### 2.8 语言切换
当前 view 整体 `fadeOutIn`：
- a=1 → a=0.6 (120ms easeOutCubic) → 切换文字 → a=0.6 → a=1 (180ms easeOutQuint)
- 总共 300ms。卡片不能 layout 跳。

### 2.9 卡片 hover (Lunching tile)
| 元素 | from | to | duration | curve |
|---|---|---|---|---|
| translateY | 0 | -2 | 150ms | easeOutCubic |
| shadow alpha | 14 | 30 | 150ms | easeOutCubic |
| shadow blur | 3 | 12 | 150ms | easeOutCubic |
| shadow offsetY | 1 | 4 | 150ms | easeOutCubic |
| 取消 hover | 反向 | 反向 | 150ms | easeOutCubic |

### 2.10 按钮 press
| 元素 | from | to | duration | curve |
|---|---|---|---|---|
| scale | 1.0 | 0.97 | 80ms | easeOutCubic |
| 释放回弹 | 0.97 | 1.0 | 200ms | spring(damping=0.6) |

### 2.11 输入框 focus
- 边框 1px → 2px (150ms) + 颜色 divider → primary (150ms)
- focus halo: 0 0 0 0 → 0 0 0 4px rgba(primary, 0.18)，over 200ms

### 2.12 Toast (右下角)
- 入场：x+16,a=0 → x=0,a=1，300ms easeOutQuint
- 停留 3000ms（progress bar 显示倒计时，linear）
- 退场：a=1 → a=0,y-8，220ms easeOutCubic

### 2.13 Skeleton shimmer (loading 占位骨架)
- 渐变线性扫描，4° 倾斜，8s 周期，linear；不停循环
- 颜色：base + (base+5%) 两段

---

## 3. 物理感细节

- **下落感**：纵向上滑入场 (`y: +12 → 0`) > 直接淡入。眼睛对垂直运动更敏感。
- **回弹要克制**：`easeOutBack` 只用在 dropdown 容器 / spinner 入场 / button release。**不要**全局乱用。
- **阴影是层级核心**：hover 时阴影从 `0 1px 3px` → `0 4px 12px`，配合 -2px 上浮 = "卡片抬起来"的错觉。
- **切换不切布局**：view 切换时左右两块卡片**位置完全不变**，只换内容，避免视觉错位。

---

## 4. 性能预算

每帧 8ms 内完成（120fps 目标）：
- 渲染本帧 < 4ms（Skia GPU）
- 动画 tick < 1ms（最多 30 个并行 tween）
- 布局 < 1ms（Clay 增量）
- 系统 swap + 余量 ≈ 2ms

如果有动画超过 16ms（60fps 也撑不住），优先：
1. 减少同时跑的 tween（合并到 view-level fade）
2. shadow 改用预渲染的 9-slice 而非 SkImageFilters::Blur
3. 文字 cache 成 SkTextBlob 复用

---

## 5. 状态机参考（Profile view 上传头像）

```
[Idle] ── click "上传头像" ──> [Picking File] (系统 dialog 显示，UI 不动)
[Picking File] ── 选完 ──> [Previewing] (头像区域 cross-fade 200ms 显示新图)
[Previewing] ── click "保存" ──> [Uploading] (按钮 spinner + 进度条 0→100%)
[Uploading] ── 200 OK ──> [Success Toast] ── 3s ──> [Idle]
[Uploading] ── error ──> [Error Toast] ── click 重试 ──> [Uploading]
```

每个状态过渡都要 fade，不允许"瞬切"。

---

## 6. 给 claude design 的具体任务

阅读完上述后，请：

1. **画一组完整的 hi-fi mock**：
   - Loading 200×200（含 spinner 静帧）
   - Login 页（用户名/密码/记住我）
   - HomeView（亮 + 暗 各一）
   - LunchingView（4×3 网格，含 hover 态）
   - SettingsView
   - ProfileView（含 4 个状态：默认 / 编辑昵称 / 改密 modal / 上传中）
   - LoginHistory overlay（含成功 + 失败两种行）
   - 头像下拉菜单（含 hover 态）
   - Toast 通知（成功/警告/错误三色）
2. **导出 design tokens** 为 JSON（颜色/字号/圆角/阴影/动画时长曲线），格式参考 [color_tokens.h](../src/ui/theme/color_tokens.h)
3. **每张 mock 标注动画**：哪些元素要 enter / 哪些要 exit，时长 + curve，跟本文档对得上
4. **如果想偏离本文档某条规则**，写 1 句解释为什么；不要默默改。

---

## 7. 字体

- 中文：**Source Han Sans CN**（Regular / Medium / Bold）
- 日文：**BIZ UDPGothic**（Regular / Bold）
- 英文 / 数字：**Space Grotesk**（Regular / Medium / SemiBold）
- 等宽（UID / 设备 ID 这类）：**DejaVu Sans Mono**

数字一律用等宽对齐，UID 那种 8 位短码用 Mono 显示更稳。

---

## 8. 暗色 palette 校准点

亮色到暗色的过渡不是简单"反色"。以 Claude Desktop 为参考，暗色：
- 背景 `#1A1816`（不是纯黑）
- 卡片 `#242220`（高于背景一档，差值约 6-8%）
- 主色 `#D97757`（比亮色 `#C96442` 略亮，因为暗背景吃饱和度）
- 文字主 `#F5F1EA`（不要纯白 #FFF，眼睛会刺）

每对颜色之间的对比度 ≥ 4.5:1（WCAG AA）。

---

## 9. 不要做的事

- 不要做 **3D 翻转 / 立方体切换** —— 廉价
- 不要做 **粒子 / 光晕 / 模糊背景** —— 性能爆炸
- 不要做 **加载条**（顶部那种灰条）—— 用 Skeleton 或 Spinner
- 不要做 **音效** —— launcher 不是游戏
- 不要做 **emoji 装饰**（除非用户明确需要）

---

## 10. 评审标准

设计交付时我会用这几条打勾：
- [ ] 所有过渡时长来自 `[150, 200, 300, 350, 500]ms` 五档之一
- [ ] 所有缓动来自 `[easeOutQuint, easeOutCubic, easeInOutCubic, easeOutBack, spring]` 五选一
- [ ] 暗色 / 亮色对调时无任何元素消失或位移
- [ ] view 切换不超过 250ms，期间用户能再次点击别的菜单（取消旧动画）
- [ ] 任何 modal / dropdown 都能 ESC / 点外部 / 点 X 三种方式关
- [ ] hover 阴影差值 ≥ 6 alpha 单位（看得见但不抢戏）
- [ ] 字体加载失败时回退链不报错（中日英都要测）
