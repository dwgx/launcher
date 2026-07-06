# Launcher 技术文档

Launcher 是一个 Windows 桌面游戏启动器，由三个信任域组成：

- **客户端** —— C++20 原生程序（Skia UI、libcurl、libsodium、SQLite 缓存、Registry + DPAPI），面向 VMProtect 加固。运行在**不可信环境**，默认被视为可被逆向 / 篡改。
- **后端** —— Rust / axum + PostgreSQL 服务（`SystemBackend`），持密码哈希、HWID 哈希、session、审计日志；**不持签名私钥**。
- **签名器** —— 离线 Ed25519 CLI（`signer`），只在管理员机器运行，为分发内容签名，私钥永不上服务器。

!!! note "内部文档"
    本站是**内部技术文档**，可包含完整的架构、逻辑与加密/信任模型细节。但按 `AGENTS.md` 约定，
    **任何字面秘密**（内网 IP、密码、连接串、`config.toml` 内容、私钥、HWID 盐值）都不得写入 ——
    一律以**名字/角色**引用（如 HWID 盐 `launcher.hwid.salt.v1`、`admin_password`、`signer/private.key`）。

!!! warning "实现进度：客户端仍处于早期阶段（Phase 1）"
    `src/` 下的客户端是**长期骨架**：`net`/`storage`/`core`/`crypto`/`native` 各模块已实现能力，但主入口
    `src/app/main.cpp` 尚未把它们接线，登录页、订阅缓存、验签、游戏启动等跨模块流程**尚未落地**。
    本站如实标注每处「已落地 / 契约（unverified）」的边界，不把设计意图当作已实现。详见各客户端子系统页。

## 从哪里开始

- **[架构总览](architecture/index.md)** —— 进程边界、三信任域、组件拓扑的高层视图。
- **[端到端数据流](architecture/data-flow.md)** —— 启动→登录→订阅→启动游戏的完整流，逐段标注实现状态。
- **客户端子系统** —— [App 与 UI 层](client/app-ui.md)、[网络/存储/核心](client/net-storage-core.md)、[加密与原生模块](client/crypto-native.md)。
- **[图片管线（跨端）](client/image-pipeline.md)** —— 客户端异步解码 + 下载池 + 后端缩略图/BlurHash/变体服务三波优化（已部署）。
- **[桌面通知](client/notifications.md)** —— 顶部动态岛 Toast 的状态机与时间线。
- **后端子系统** —— [认证/会话/共享加密](backend/auth-session.md)、[API 与管理后台](backend/api-admin.md)。
- **[Signer / .helix / Proto](data/signer-proto.md)** 与 **[数据模型](data/data-model.md)**。
- **[安全与信任模型](security/index.md)** —— 信任边界、已知弱点清单，以及 [端到端加密与验证链](security/crypto-chain.md)。
- **[构建与部署](dev/build-deploy.md)**、**[视觉冒烟测试](dev/testing-visual-smoke.md)** 与 **[文档流水线](dev/doc-pipeline.md)**。

!!! info "关于旧的 `ARCHITECTURE.md`"
    早期单页 `docs/ARCHITECTURE.md` 已被本文档集的**架构**与**安全与信任模型**两章取代（superseded）。
    该旧页部分数据流（如「Ed25519 验签 → BLAKE3 → 启动」）描述的是**设计意图而非当前代码**，请以
    [端到端数据流](architecture/data-flow.md) 与 [加密与验证链](security/crypto-chain.md) 中标注了实现状态的版本为准。

## 文档如何保持与代码同步

内容由一条 ultracode「codegraph」workflow 从源码里分析生成，每条承重结论都标注了对应的 `file:line`，
无法验证处标注 `unverified`，并经过对抗式准确性评审。详见[文档流水线](dev/doc-pipeline.md)。
