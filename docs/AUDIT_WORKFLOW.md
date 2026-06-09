# Launcher Audit Workflow

本流程用于破坏性 debug、上传漏洞探测和聊天协议回归。不要在未隔离的生产库上跑大规模 fuzz。

## 1. 部署隔离服务

```powershell
.\scripts\audit\deploy-audit.ps1
```

设置连接目标后部署到 VPS:

```powershell
$env:LAUNCHER_DEPLOY_HOST = "<host>"
$env:LAUNCHER_DEPLOY_USER = "<ssh-user>"
$env:LAUNCHER_DEPLOY_KEY = "$HOME\.ssh\launcher_deploy"
.\scripts\audit\deploy-audit.ps1
```

- runtime: `/opt/systembackend-audit`
- service: `systembackend-audit.service`
- database: `helix_audit`
- URL: `http://<host>:1338`

## 2. 跑 API/安全 smoke

```powershell
$env:LAUNCHER_AUDIT_BASE = "http://<host>:1338"
node .\scripts\audit\api-smoke.mjs
```

脚本覆盖：

- 注册/登录/错误密码/HWID mismatch/logout
- profile、heartbeat、subscription
- WebSocket ready/status broadcast
- DM、history、react、read、delete 权限
- media/avatar 上传 MIME 欺骗、非法 sha 下载
- sticker pack owner 权限和 delete
- market purchase/review guard

## 3. 清理测试数据

```powershell
$env:LAUNCHER_DEPLOY_HOST = "<host>"
$env:LAUNCHER_DEPLOY_USER = "<ssh-user>"
.\scripts\audit\cleanup-audit-data.ps1 -RemoteDir /opt/systembackend-audit
```

清理规则只匹配 `audit%` 用户，并输出删除数和残留数。

## 4. Desktop 指向隔离服务

不改 UI，使用环境变量覆盖 API 目标：

```powershell
$env:LAUNCHER_API_SCHEME = "http"
$env:LAUNCHER_API_HOST = "<host>"
$env:LAUNCHER_API_PORT = "1338"
.\dist\LauncherD2D.exe
```

生产默认不跳过 TLS 证书校验。如确实要连自签名测试 HTTPS，显式设置：

```powershell
$env:LAUNCHER_ALLOW_INSECURE_TLS = "1"
```
