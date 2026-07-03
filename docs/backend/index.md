# 后端概览

后端 `SystemBackend` 是 Rust / axum 0.7 + PostgreSQL 服务（`SystemBackend/crates/api`），提供客户端业务 API
与 Web 管理后台（SSR）。本章分两页：

- **[认证 / 会话 / 共享加密](auth-session.md)** —— 注册/登录/登出/改密状态机、HWID 咨询式绑定、session 生命周期、`launcher_shared::hashing` 共享加密面。
- **[API 与管理后台](api-admin.md)** —— axum 路由拓扑、客户端 session 鉴权、Admin cookie / operator 角色模型、端点目录、已知安全弱点。

## Workspace 布局

后端是一个 Cargo workspace（`SystemBackend/Cargo.toml`），四个 crate：

| crate | 职责 |
|---|---|
| `api` | axum HTTP 服务，全部 handler、路由、Admin SSR | 
| `shared` | 共享加密（Argon2id、SHA-256 盐化）、config、uid |
| `proto` | 编译 `src/proto/subscription.proto`（跨仓）为 prost 类型 |
| `signer` | 离线 Ed25519 签名 CLI（见 [Signer / Proto](../data/signer-proto.md)） |

关键依赖（`Cargo.toml` workspace deps）：axum 0.7（`macros/multipart/ws`）、sqlx 0.8（postgres/uuid/chrono/macros）、argon2、sha2、hmac、blake3、ed25519-dalek、askama（SSR 模板）、rustls 0.23。

## 服务装配

`crates/api/src/main.rs`：加载 `config.toml`（`main.rs:41-44`）→ 连接 Postgres 并跑 migration（`:46-47`）→ 构造 `AppState`（`:49`）→ 组装 `axum::Router`（`:51-76`）。

- `/api` 用 `nest`，各 admin 模块用 `merge`（内部声明完整绝对路径）。
- 全局中间件：`TraceLayer`（`main.rs:71`）、`RequestBodyLimitLayer` 100MB（`:73-75`，媒体上传）。
- 传输层：配了 TLS 证书走 rustls HTTPS，否则纯 HTTP（`:79-90`）。rustls 0.23 需显式安装 crypto provider（`:37-39`）。

!!! note "两套并存的「管理员」信任链"
    - **Web 管理后台**（`/admin/*` 与 `/api/admin/*`）走 Admin cookie / operator 角色模型（见 [API 与管理后台 §4](api-admin.md#4)）。
    - **社区/工单侧 `/api/admin/*`**（`community.rs`）不走 operator 模型，而是用客户端 session token + `users` 表的 `is_admin`/`role` 字段判定。
    这是两条**独立信任链**，审计与理解权限时需分开看待。

## 错误处理

`error.rs` 提供 `internal`/`internal_msg`（`error.rs:17-40`）：真实错误 + `#[track_caller]` 定位走 `tracing::error!`，返回给客户端只有通用 `"internal server error"`，避免泄露 SQL/schema/路径。业务 handler 普遍 `.map_err(internal)?`。

## 已知安全弱点（索引）

后端存在多个已知弱点，集中记录在 [安全与信任模型](../security/index.md) 并在 [加密与验证链](../security/crypto-chain.md) 展开：

- HWID 不在登录路径强制（审计 High）——[认证/会话 §5](auth-session.md#5)。
- admin `?key=admin_password` 明文 URL 旁路——[API 与管理后台 §5](api-admin.md#5)。
- admin cookie HMAC key = `admin_password`（密钥复用）——[API 与管理后台 §5](api-admin.md#5)。
- bootstrap-owner 常驻后门、过期 session 行不自动清理、明文 HTTP 传输。
