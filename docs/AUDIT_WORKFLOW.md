# Launcher Audit / Smoke Workflow

本流程用于回归后端 API、WebSocket、媒体上传、聊天权限和客户端启动健康。默认不要在生产库上跑写入型测试。

## 1. 一键 smoke

默认目标是隔离 audit 环境：

```powershell
$env:LAUNCHER_DEPLOY_HOST = "<host>"
$env:LAUNCHER_DEPLOY_USER = "<ssh-user>"
$env:LAUNCHER_DEPLOY_KEY = "$HOME\.ssh\launcher_deploy"
powershell -ExecutionPolicy Bypass -File .\scripts\smoke.ps1 -Target audit -Depth full
```

行为：

- 部署 `/opt/systembackend-audit`
- 使用 `systembackend-audit.service`
- 使用 `helix_audit` 数据库
- 对外 URL 为 `http://<host>:1338`
- 跑 API/WS 写入 smoke
- 构建 D2D 客户端并做启动探测
- 最后按 `run_id` 清理临时测试数据

生成 JSON 报告：

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\smoke.ps1 `
  -Target audit -Depth full `
  -JsonReport "$env:TEMP\launcher-smoke.json"
```

## 2. 常用模式

只跑本地预检：

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\smoke.ps1 `
  -Target custom -Depth preflight -BaseUrl http://127.0.0.1:1338
```

只跑 API/WS 写入 smoke，目标为已有隔离服务：

```powershell
$env:LAUNCHER_AUDIT_BASE = "http://<host>:1338"
powershell -ExecutionPolicy Bypass -File .\scripts\smoke.ps1 `
  -Target audit -Depth api -BaseUrl $env:LAUNCHER_AUDIT_BASE
```

生产只读 smoke：

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\smoke.ps1 `
  -Target production -Depth readonly
```

生产默认只读。写入型生产 smoke 必须显式传 `-AllowProductionWrites`，并且必须有 SSH 配置用于清理数据。

## 3. API smoke 单独运行

写入型 audit smoke：

```powershell
$env:LAUNCHER_AUDIT_BASE = "http://<host>:1338"
$env:LAUNCHER_AUDIT_MODE = "write"
node .\scripts\audit\api-smoke.mjs
```

只读 smoke：

```powershell
$env:LAUNCHER_AUDIT_BASE = "https://154.40.36.22:1337"
$env:LAUNCHER_AUDIT_MODE = "readonly"
$env:LAUNCHER_AUDIT_INSECURE_TLS = "1"
node .\scripts\audit\api-smoke.mjs
```

JSON 输出：

```powershell
$env:LAUNCHER_AUDIT_JSON = "1"
$env:LAUNCHER_AUDIT_JSON_PATH = "$env:TEMP\launcher-api-smoke.json"
node .\scripts\audit\api-smoke.mjs
```

## 4. 覆盖范围

写入型 smoke 覆盖：

- 注册、登录、错误密码、HWID mismatch、logout
- heartbeat、profile、subscription
- WebSocket ready/status broadcast
- DM、history、react、read、delete 权限
- media/avatar MIME 探测、非法 sha、带 token 下载
- sticker pack owner 权限、pack get/add/remove/delete
- market categories、listing、purchase、review guard

只读 smoke 覆盖：

- `GET /api/market/categories`
- `GET /api/market/listings?limit=5`
- `GET /admin`

## 5. 清理

手动清理隔离环境中某个 run id：

```powershell
$env:LAUNCHER_DEPLOY_HOST = "<host>"
$env:LAUNCHER_DEPLOY_USER = "<ssh-user>"
powershell -ExecutionPolicy Bypass -File .\scripts\audit\cleanup-audit-data.ps1 `
  -RemoteDir /opt/systembackend-audit `
  -Prefix smoke20260621120000
```

清理 SQL 只匹配 `username LIKE '<prefix>%'` 的测试用户，并删除相关 session、chat、media、sticker、market 数据。

## 6. Desktop 指向隔离服务

不改代码，用环境变量覆盖 API 目标：

```powershell
$env:LAUNCHER_API_SCHEME = "http"
$env:LAUNCHER_API_HOST = "<host>"
$env:LAUNCHER_API_PORT = "1338"
.\dist\LauncherD2D.exe
```

如果连接自签 HTTPS 测试服务，显式设置：

```powershell
$env:LAUNCHER_ALLOW_INSECURE_TLS = "1"
```

`third_party/webview2` 和 `dist/` 都不提交到 Git。
