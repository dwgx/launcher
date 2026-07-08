# Changelog

本项目的所有重要变更记录于此。格式参考 [Keep a Changelog](https://keepachangelog.com/zh-CN/)，
版本号遵循 [语义化版本](https://semver.org/lang/zh-CN/)。

## [0.1.0] - 2026-07-09

首个预发布版本。Windows D2D 客户端 + Rust axum 后端的核心功能已跑通，收敛为可维护、可审计、可部署的产品主线。

### 客户端（`tools/preview-d2d/`，D2D + DComp + DXGI + WebView2）
- 注册 / 登录 / session 持久化，连接生产后端。
- 聊天:官方频道、消息收发、回复引用、@提及、emoji 反应、消息撤回(30s 窗口)、右键上下文菜单。
- 表情包:emoji + sticker pack 选择器、导入/导出/新建/拖拽排序、分享。
- 市场、云端、个人主页、头像、状态、标签、设置等社区功能。
- 图片管线:异步解码 + 下载池 + 后端缩略图/BlurHash/变体/ETag 三波优化。
- 桌面动态岛通知、CS2 视频瓦片、系统托盘。

### 后端（`SystemBackend/`,Rust + axum + PostgreSQL + systemd）
- 认证(Argon2id 密码哈希、HWID 二次盐化、UUID session)、限流、审计日志。
- 聊天/市场/表情包/社区/管理后台 API(27 个模块)。
- Askama SSR 管理面板 + HMAC cookie(独立 `admin_cookie_secret` 签名)。
- 20 个 sqlx migration,服务启动自动应用。
- 离线 Ed25519 签名 CLI(`signer`),私钥永不上服务器。

### 本版重点改动(交互层重构)
- **统一浮层栈 `OverlayStack`**(`overlay.h/.cpp`):把原先三套并存的命中测试机制收敛成单一派发路径,z-order = paint 顺序,加浮层从改 ~8 处降到一次 `add()`。修复一批点击穿透/菜单不消失/双击/拖窗误触的历史 bug。
- **修复右键菜单 + 三点菜单交互**:根因是无边框窗口的 `WM_NCHITTEST` 把可点区误判为标题栏、被 Windows 拿去拖窗而不发点击事件。现在浮层打开时整窗可点、聊天消息流区域整体可点(右键任意消息都能弹菜单)。
- **多行输入框 `MultilineEdit`**(`textedit.h`):Enter 发送 / Shift+Enter 换行、输入框随行数自动增高、撤销重做(Ctrl+Z / Ctrl+Shift+Z)、跨行光标移动、全选/复制/粘贴/剪切。
- **交互冒烟测试**扩充到 68 个断言(`visual_smoke.cpp`),覆盖浮层派发、NCHITTEST 根因、多行编辑等回归。

### 已知限制
- 多行输入框:长行暂不自动软回环(仅 Shift+Enter 硬换行分行);跨行点选较糙。
- 中文输入法(IME)预编辑与表情选择器搜索/键盘导航为后续版本计划。
- 客户端验签 / BLAKE3 校验 / 游戏启动为契约(proto/签名侧就绪),消费侧未落地。
- 传输层生产以 HTTP 运行(明文 session token),属已知弱点。

[0.1.0]: https://github.com/dwgx/launcher/releases/tag/v0.1.0
