# Agent 工作规则

本仓库的目标是把已经跑通的 Launcher D2D 客户端和 Rust 后端收敛成可维护、可审计、可部署的产品。任何 agent 进入仓库后必须先读本文件，再读 `docs/WORKFLOW.md`、`docs/PROJECT_OUTLINE.md`、`SESSION_HANDOFF.md`。

## 硬规则

- 没有证据就不要下结论。结论必须来自当前代码、命令输出、远端服务状态、官方文档或明确标注的推断。
- 用户要求 review/audit 时，先列问题和证据，再写建议；不要幻想未来可能存在的问题当成当前事实。
- 不要在未闭环时停在口头计划。能构建、验证、写文档、提交同步的，就继续做到闭环。
- 不提交凭据、密钥、证书、`.deploy.local`、`config.toml`、`dist/`、`third_party/` 下载包或本地构建产物。
- 改生产后端前先确认影响面；改完必须验证 systemd 服务、公网 API、必要的 smoke test。
- 改 migration 后必须确认数据库 schema 与 `sqlx::query!` 一致；后端 release 构建必须带 `DATABASE_URL`。
- 客户端主线当前是 `tools/preview-d2d/`，不是 `src/` 的 CMake/Skia 骨架。不要误删 `tools/preview/`，除非已完成 parity 并经过验证。
- 每次收尾前检查 `git status --short`，只提交应该进仓库的源码、文档、脚本和 migration。

## 家里云生产部署

- 生产后端跑在家里云（局域网），systemd 服务 `systembackend.service`，监听 `0.0.0.0:1337`，工作目录 `/opt/systembackend`，运行二进制 `/opt/systembackend/systembackend`，运行用户 `systembackend`。
- 数据库是本机 PostgreSQL（`127.0.0.1:15433`，库 `helix_prod`），连接串在 `/opt/systembackend/config.toml` 的 `database_url`。
- **后台 HTML 模板由 Askama 在编译期嵌入二进制**：改 `SystemBackend/crates/api/templates/*.html` 后，仅 `git pull` 或同步源码不会生效，**必须在服务器上带 `DATABASE_URL` 重新 `cargo build --release -p launcher-api`、替换 `/opt/systembackend/systembackend` 并 `systemctl restart systembackend`**。bin 名是 `systembackend`，不是 `launcher-api`。
- 服务器无 git 仓库，更新走 `scripts/deploy.ps1`（打包本地→上传 `build_src`→连库构建→装二进制→重启），或等价的 rsync/sftp 同步源码到 `/opt/systembackend/build_src/SystemBackend/` 后照上面构建。
- 部署流程参考 `scripts/deploy.ps1` 顶部注释。

## 凭据与安全（所有 AI / agent 必须遵守）

- **SSH 连接信息和密码只记录在本地、被 gitignore 的 `CLAUDE.md` 里，绝不写入 `AGENTS.md` 或任何被 git 跟踪的文件**——本仓库是公开仓库，AGENTS.md 会被发布。
- 连接家里云做部署前，从 `CLAUDE.md` 读取凭据；若 `CLAUDE.md` 不存在或无凭据，停下来问用户，不要猜测或硬编码。
- **改动生产（替换二进制、重启服务、跑 migration、删数据）前必须向用户确认**，并先备份将被覆盖的二进制/数据；构建到临时位置、验证成功后再替换线上，失败保留旧版可回滚。
- 不要把家里云内网地址、SSH 密码、数据库连接串、`config.toml` 内容回显到提交、PR、或会被发布的文档里。含内网地址的本地启动脚本（`tools/preview-d2d/run-multi.bat`、`启动-连homecloud.bat`）已 gitignore，保持如此。
- 未经用户明确授权，不得在家里云上做与当前任务无关的操作。

## 推荐读序

1. `README.md`
2. `docs/WORKFLOW.md`
3. `docs/PROJECT_OUTLINE.md`
4. `docs/UI_STYLE_LEARNING.md`，仅当工作涉及后台 Admin UI 或 `tools/preview-d2d/` UI
5. `SESSION_HANDOFF.md`
6. `docs/PHASE_2_D2D_MIGRATION.md`，仅当工作涉及 D2D UI 或渲染迁移
7. `SystemBackend/migrations/` 和 `SystemBackend/crates/api/src/`，仅当工作涉及后端

## 最低验证

- Windows 客户端：`cmd /c tools\preview-d2d\build_d2d.bat`
- 后端编译：在 VPS 或有 PostgreSQL schema 的环境中设置 `DATABASE_URL` 后运行 `cargo build --release -p launcher-api -p launcher-signer`
- 后端服务：`systemctl status systembackend --no-pager -l`
- 公网 API：`curl -k https://<DEPLOY_HOST>:1337/api/market/categories`
- Git 卫生：`git diff --check` 和 `git status --short`
