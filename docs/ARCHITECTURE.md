# 架构

```
┌────────────────────┐                ┌──────────────────────┐
│  Launcher.exe (C++) │ ─ HTTPS ────▶ │ SystemBackend (Rust) │
│                    │ ◀── JSON ──── │  axum :1337          │
│  - Skia + Clay     │                └──────────┬───────────┘
│  - libcurl         │                           │ sqlx
│  - libsodium       │                           ▼
│  - sqlite (cache)  │                ┌──────────────────────┐
│  - Registry+DPAPI  │                │ PostgreSQL :5432     │
│   (sensitive)      │                │ users / sessions /   │
└──────────┬─────────┘                │ subscriptions / log  │
           │                          └──────────────────────┘
           │ HTTPS GET .helix
           ▼
   ┌───────────────────┐         ┌──────────────────┐
   │  CDN (R2 / nginx) │ ◀────── │  signer (CLI)    │
   │  *.helix          │  upload │  Ed25519 私钥    │
   └───────────────────┘         │  离线签发        │
                                  └──────────────────┘
```

## 数据流

### 启动 → 登录
1. `LoadingView` 显示 200×200 旋转卡片
2. （Phase 7）`HttpClient.get("/api/heartbeat?session_token=…")` 探活；若无 token 直接进登录
3. 登录提交 `{username, password, hwid_hex}`，`HwidCollector::collectFingerprint()` 给出 SHA-256
4. 后端验 Argon2id 密码 → 比对/绑定 `hwid_bound` → 发 `session_token` 写注册表 `Slot::SessionToken`

### 订阅刷新
1. 客户端 `GET /api/subscription?session_token=…` 拿 `helix_url`
2. 客户端从 CDN 拉 `.helix` Protobuf
3. Ed25519 验签（签名公钥硬编码在客户端二进制）
4. 解析 `GameEntry`，写入 SQLite `subscription_cache` + `game_meta`
5. EventBus 发 `SubscriptionUpdated`，UI 刷新卡片网格

### 启动游戏
1. UI 卡片 click → core::Launcher::launch(game_id)
2. 校验本地文件 BLAKE3 == `file_hash`
3. （Phase 7+）若有 hooks，IHookEngine 注入
4. CreateProcessW（动态 GetProcAddress）启动 executable

## 进程边界

- 客户端：单进程 + 主线程（UI）+ 后台线程（HTTP/IO）
- 后端：单进程 axum，Tokio 运行时
- `signer` CLI：独立进程，只在管理员机器跑，**不在服务器**

## 信任模型

- **客户端二进制**：硬编码签名公钥（公开）+ HWID 盐（半公开）
- **服务器**：持密码哈希 + HWID 哈希 + session token；**不持签名私钥**
- **管理员机器**：持 Ed25519 私钥，本地 toml 配置，离线 sign → 上传 CDN

被攻击的恢复：
- 服务器被打穿：签发不可伪造，攻击者只能下毒 helix_url 指向其他文件，但客户端仍要验签
- 管理员私钥泄漏：rotate 公钥 → 发新版客户端 → 旧客户端拒绝新订阅（或发个紧急 OTA 通知）
