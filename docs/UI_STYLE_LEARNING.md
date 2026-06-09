# Launcher UI Style Learning Record

本记录用于约束后续所有 UI 修复。用户已明确要求：不允许乱改、不允许重写 UI、不允许换风格。任何 UI 工作只能先学习现有写法，再在原体系内修具体问题。

## 结论

- Desktop 主线是 `tools/preview-d2d/`，使用 Win32 + Direct2D + DirectComposition 即时绘制。
- Backend Admin 是 `SystemBackend/crates/api/templates/`，使用 Askama 模板 + Tailwind CDN + DaisyUI CDN + `base.html` 内的自定义 `launcher` theme。
- 两边不是同一种技术栈，不能互相移植实现方式，但必须保持同一产品气质：暖色深色、低噪声、克制、偏工具型。
- UI 修复只能改局部 bug：布局错位、遮挡、层级、溢出、可读性、状态反馈、表单行为。不能重做页面结构、换配色、换组件库、加营销式视觉。

## 禁止事项

- 不要重写整个页面、组件或渲染系统。
- 不要把后台改成 React/Vue/Svelte，也不要把 desktop 改成 WebView 页面。
- 不要替换现有暖色深色主题，不要引入大面积紫蓝渐变、玻璃球、装饰光斑、营销 landing page 风格。
- 不要改动 UI 文案、结构和交互含义来“看起来更现代”，除非该改动直接修复已证实的问题。
- 不要新增与现有风格冲突的卡片套卡片、超大圆角、夸张阴影、巨大标题、宽松营销布局。
- 不要在没有截图、代码位置或可复现步骤时断言 UI 问题已修复。

## Backend Admin 风格

权威入口：

- `SystemBackend/crates/api/templates/base.html`
- `SystemBackend/crates/api/templates/*_content.html`
- `SystemBackend/crates/api/src/admin*.rs`

现有视觉语言：

- 页面布局：固定左侧 sidebar + 顶部 navbar + 主内容区。
- 技术栈：Askama 模板直接输出 HTML；Tailwind/DaisyUI 只通过 CDN 使用。
- 主题：`data-theme="launcher"`，色彩 token 写在 `base.html`。
- 主背景：暖黑棕 `--b1`，surface/card 使用 `--b2`，divider 使用 `--b3`。
- 主色：橙棕 `#D97757` / `#C96442`，只用于 primary、active、重点 code、轻微 hover。
- 字体：`Source Han Sans CN`, `BIZ UDPGothic`, `Segoe UI`, system-ui。
- 信息密度：后台是管理工具，表格、表单、按钮要紧凑可扫读，不能做成展示页。

后台组件写法：

- 页面外壳只从 `base.html` 继承，不单独复制 layout。
- 内容卡片沿用 `card bg-base-100 shadow` + `card-body`。
- 表格沿用 `table table-zebra table-sm`，header 使用现有 CSS 主题。
- 状态 badge 沿用 `.badge-active/.badge-revoked/.badge-expired/.badge-exhausted` 或 DaisyUI badge 的现有组合。
- 表单沿用 `form-control`, `label-text text-xs`, `input input-sm input-bordered`, `btn btn-primary btn-sm`。
- 危险操作沿用低饱和红色，不引入新的红色体系。

Backend Admin 修复规则：

- 优先修 HTML 结构、Tailwind class、少量 `base.html` scoped CSS。
- 如果 table 内 dropdown 被遮挡或压住其他单元格，只修层级、overflow、定位和点击目标，不改整体表格风格。
- 如果文字过暗或重叠，先确认是浏览器插件/截图覆盖层还是页面 CSS，再局部调 opacity、line-height、cell alignment。
- 任何后台 UI 改动后至少验证 `/admin`, `/admin/invites`, `/admin/channels` 三个页面。

当前截图中的待核查问题：

- `/admin/channels` 操作 dropdown 在表格行内展开时疑似与状态 badge/行内容重叠，需要检查 `details.dropdown`、`overflow-x-auto` 和 z-index/定位关系。
- `/admin/invites` 状态 badge 在表格中看起来位置偏移，需要检查单元格 vertical-align、badge line-height、浏览器缩放和翻译插件影响。
- 页面整体看起来偏暗，需要先排除浏览器扩展或截图遮罩影响，再决定是否调整文字 opacity。不能直接换主题。

## Desktop D2D 风格

权威入口：

- `tools/preview-d2d/main.cpp`
- `tools/preview-d2d/ui_main.cpp`
- `tools/preview-d2d/auth.cpp`
- `tools/preview-d2d/chat.cpp`
- `tools/preview-d2d/modals.cpp`
- `tools/preview-d2d/sticker.cpp`
- `tools/preview-d2d/palette.h`
- `tools/preview-d2d/render/primitives.h`
- `docs/PHASE_2_D2D_MIGRATION.md`

现有视觉语言：

- 渲染方式：Direct2D immediate-mode，每帧由状态驱动绘制。
- 几何单位：DIP，必须考虑 DPI 缩放。
- 字体：主要使用 `Microsoft YaHei UI`，文本绘制通过 DirectWrite。
- Palette：必须使用 `palette.h` 的 `kLight/kDark` token，不临时发明新主题。
- 主色：dark 下 `primary = 0xFFD97757`, `primary_hover = 0xFFE58666`。
- 背景和卡片：dark 下 `bg = 0xFF1A1816`, `surface = 0xFF201E1B`, `card = 0xFF242220`, `divider = 0xFF36322D`。
- 组件形态：圆角适中、阴影克制、动画短促、信息密度高。

Desktop 组件写法：

- 使用 `prim::fillRR`, `prim::strokeRR`, `prim::drawText_`, `prim::drawShadow`, `LayoutRect` 等现有 helper。
- 点击区域通过 `hit(...)`/`hitClear()` 管理，不能另起一套事件系统。
- 动画沿用已有 tween/transition/state，不加独立计时器体系。
- 网络和业务状态沿用 `g_session_token`, `g_user`, `chat::*`, `sticker::*`, `fetch::*` 等现有全局状态和 worker pattern。
- 图片、头像、sticker 缓存沿用 `%LOCALAPPDATA%\Launcher` 相关路径，不新增平行缓存目录。

Desktop 修复规则：

- 先找对应 view 的 paint + event handler，不跨文件大搬运。
- 文字溢出先用现有测量逻辑和裁剪/省略策略解决，不扩大整体布局。
- 修聊天、表情、上传时必须保持 Telegram/QQ 类操作习惯，但视觉仍按现有 D2D 风格。
- 每次 desktop UI 改动后运行 `cmd /c tools\preview-d2d\build_d2d.bat`，并用本地窗口实际点击验证关键路径。

## UI 工作流程

1. 先复现问题，记录页面/视图、路径、点击步骤、截图或命令证据。
2. 读对应现有实现，确认该页面的本地组件写法。
3. 写最小修复，不改设计系统，不迁移技术栈。
4. 验证相关页面和相邻页面，避免修一处破一处。
5. `git diff --check`，检查没有构建产物、秘密、无关 UI 重写。
6. 如果改了生产后台，部署后验证公网页面和服务状态。

## 后续审计记录格式

每发现一个 UI 问题，按这个格式记录：

```text
问题：
证据：
位置：
影响：
修复边界：
验证：
```

示例：

```text
问题：频道管理 dropdown 展开后压住状态 badge。
证据：用户截图 `/admin/channels`，操作菜单展开后与同一行 badge 区域重叠。
位置：SystemBackend/crates/api/templates/channels_content.html；base.html dropdown/table CSS。
影响：后台操作入口可读性和点击目标混乱。
修复边界：只修 dropdown 层级/定位/overflow，不重写页面布局。
验证：登录后台，访问 /admin/channels，逐行展开操作菜单，确认菜单不遮挡关键文字且可点击。
```
