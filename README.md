# Launcher

Windows 桌面游戏启动器 / 订阅管理器。当前可运行产品由 Windows D2D 客户端和 Rust axum 后端组成。

进入仓库先读：

1. [AGENTS.md](./AGENTS.md) - agent 工作规则和证据要求
2. [docs/WORKFLOW.md](./docs/WORKFLOW.md) - 构建、部署、review、同步 GitHub 流程
3. [docs/PROJECT_OUTLINE.md](./docs/PROJECT_OUTLINE.md) - 项目目标和阶段大纲
4. [SESSION_HANDOFF.md](./SESSION_HANDOFF.md) - 历史会话交接和细节状态

## 当前主线

| 模块 | 路径 | 状态 |
|---|---|---|
| Windows 客户端 | `tools/preview-d2d/` | 当前可运行主线，D2D + DComp + DXGI + WebView2 |
| 后端服务 | `SystemBackend/` | Rust + axum + PostgreSQL，systemd 部署 |
| 产品化 CMake 客户端 | `src/` | 长期主线骨架，不是当前可交付入口 |
| 旧预览/参考 | `tools/preview/`, `tools/preview-skia/` | parity 和渲染参考，确认替代前不要删除 |

## 本地客户端构建

```powershell
git clone https://github.com/dwgx/launcher.git
cd launcher
New-Item -ItemType Directory -Force third_party | Out-Null
Invoke-WebRequest https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2 -OutFile third_party/Microsoft.Web.WebView2.nupkg
Copy-Item third_party/Microsoft.Web.WebView2.nupkg third_party/Microsoft.Web.WebView2.zip -Force
Expand-Archive third_party/Microsoft.Web.WebView2.zip third_party/webview2 -Force
cmd /c tools\preview-d2d\build_d2d.bat
.\dist\LauncherD2D.exe
```

`build_d2d.bat` 会自动查找 Visual Studio C++ Build Tools，并把 `LauncherD2D.exe`、`WebView2Loader.dll`、`cs2_header.jpg` 复制到 `dist/`。

## 后端部署

后端部署到 VPS 时以 [docs/WORKFLOW.md](./docs/WORKFLOW.md) 为准。关键规则：

- 不提交 `.deploy.local`、`config.toml`、私钥、证书或任何真实密码。
- 改 migration 后必须远端以 `DATABASE_URL` 编译 `launcher-api`，因为 `sqlx::query!` 需要编译期校验。
- 每次改生产后端必须验证 `systemctl status systembackend` 和公网 API。

## 目录

| 路径 | 说明 |
|---|---|
| `tools/preview-d2d/` | 当前可运行 Windows 客户端 |
| `src/` | 产品化客户端 C++ 骨架 |
| `SystemBackend/` | Rust 后端 + 签名 CLI |
| `assets/` | 客户端图片、i18n、字体占位 |
| `scripts/` | 部署、字体下载 |
| `docs/` | 详细架构 / 协议文档 |

## 状态

当前目标是把已跑通的 D2D 预览客户端和 Rust 后端收敛成可维护、可审计、可部署的产品主线。不要只依据旧 README 或历史交接判断状态，必须以当前代码、构建结果、远端服务状态为准。
