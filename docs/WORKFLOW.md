# Launcher Workflow

## 入口判断

进入仓库后先确认三个事实：

```powershell
git status --short --branch
Get-ChildItem -Force
rg -n "154\\.40\\.36\\.22|build_d2d|sqlx::query!|status_text|bio" README.md SESSION_HANDOFF.md docs SystemBackend tools src
```

当前结论：

- 客户端可运行入口是 `tools/preview-d2d/build_d2d.bat`。
- 后端可运行入口是 `SystemBackend/crates/api/src/main.rs`，部署目标是 VPS 上的 `systembackend.service`。
- `src/` 和 CMake/vcpkg 是长期产品化骨架，不是当前主交付入口。

## 工作闭环

每轮工作按这个顺序推进：

1. 读相关代码和文档，确认影响面。
2. 写下可验证结论，不确定的地方标为未知。
3. 修改最小必要文件。
4. 运行匹配影响面的验证。
5. 检查 Git 状态，只保留应该提交的变化。
6. commit 并 push 到 GitHub。

不要在第 2 步没有结论时停止；不要在第 4 步没验证时声称完成。

## 客户端构建

先准备 WebView2 SDK：

```powershell
New-Item -ItemType Directory -Force third_party | Out-Null
Invoke-WebRequest https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2 -OutFile third_party/Microsoft.Web.WebView2.nupkg
Copy-Item third_party/Microsoft.Web.WebView2.nupkg third_party/Microsoft.Web.WebView2.zip -Force
Expand-Archive third_party/Microsoft.Web.WebView2.zip third_party/webview2 -Force
```

构建和运行：

```powershell
cmd /c tools\preview-d2d\build_d2d.bat
.\dist\LauncherD2D.exe
```

验证点：

- `dist/LauncherD2D.exe` 存在。
- `dist/WebView2Loader.dll` 存在。
- 进程启动后 `MainWindowTitle` 是 `Launcher`，状态响应正常。

## 后端部署

生产配置只在 VPS：

- `/opt/systembackend/config.toml`
- `/opt/systembackend/certs/`
- PostgreSQL `helix` role/db
- systemd unit `systembackend.service`

标准远端构建必须读取服务器配置里的 `database_url`：

```bash
DBURL=$(sed -n 's/^database_url *= *"\(.*\)"/\1/p' /opt/systembackend/config.toml)
cd /opt/systembackend/build_src/SystemBackend
for f in migrations/*.sql; do psql "$DBURL" -v ON_ERROR_STOP=1 -f "$f"; done
DATABASE_URL="$DBURL" cargo build --release -p launcher-api -p launcher-signer
install -m 755 target/release/systembackend /opt/systembackend/systembackend
rsync -a --delete migrations/ /opt/systembackend/migrations/
chown -R systembackend:systembackend /opt/systembackend
systemctl restart systembackend
```

原因：`sqlx::query!` 在编译期需要数据库 schema。没有 `DATABASE_URL` 或 schema 不完整时，release build 会失败。

## 后端验证

```bash
systemctl status systembackend --no-pager -l
ss -tlnp | grep ':1337'
curl -k -I https://<DEPLOY_HOST>:1337/admin
curl -k https://<DEPLOY_HOST>:1337/api/market/categories
journalctl -u systembackend --no-pager -n 60
```

需要验证 Auth 时，用临时用户注册、登录、拉 profile，然后删除临时用户。不要把临时密码输出到聊天或文档。

## Migration 规则

- 新 schema 必须新增 `SystemBackend/migrations/00XX_name.sql`。
- SQL 使用 `IF NOT EXISTS`，重复执行不能破坏环境。
- 如果代码引用新列，例如 `status_text`、`bio`，migration 必须先补齐。
- 应用 migration 后用 `\d <table>` 或 API smoke test 验证。

## Review/Audit 规则

用户要求 review 或 audit 时：

1. 先列发现的问题，按严重度排序。
2. 每个问题必须有文件/行号/命令输出/API 结果作为证据。
3. 不把“可能以后会发生”写成当前缺陷。
4. 如果没有发现问题，就明确说没有发现，并列剩余测试缺口。

## GitHub 同步

提交前：

```powershell
git diff --check
git status --short
```

只提交：

- 源码
- migration
- 文档
- 构建/部署脚本
- 小型静态资源

不要提交：

- `dist/`
- `third_party/`
- `SystemBackend/target/`
- `.deploy.local`
- `config.toml`
- 密钥、证书、真实密码

推送：

```powershell
git add <files>
git commit -m "<type>: <short summary>"
git push origin master
```

## 当前 VPS 状态基线

最近一次实测状态：

- Debian 12 VPS: `<DEPLOY_HOST>`
- service: `systembackend.service`
- runtime dir: `/opt/systembackend`
- source dir: `/opt/systembackend/build_src`
- public API: `https://<DEPLOY_HOST>:1337`
- TLS: 服务直接加载 `/opt/systembackend/certs/cert.pem` 和 `key.pem`

这些是运行基线，不是凭据。实际数据库密码、admin 密码、证书私钥只保存在 VPS 本地。
