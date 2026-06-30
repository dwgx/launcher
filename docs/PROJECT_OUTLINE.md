# Launcher 项目大纲

## 结论

Launcher 当前要做的不是从零搭一个启动器，而是把已经成型的 D2D 预览客户端、Rust 后端、账号/聊天/市场/表情包能力，收敛成一条稳定主线。仓库里同时存在历史骨架、预览实现、迁移文档和生产后端代码，所以后续工作必须先分清“当前可交付入口”和“长期产品化入口”。

## 当前事实

- 当前可运行客户端在 `tools/preview-d2d/`，构建脚本是 `tools/preview-d2d/build_d2d.bat`。
- `src/` 是 CMake + Skia/Clay 的产品化骨架，但不是现在可运行交付物。
- `SystemBackend/` 是真实后端，使用 Rust、axum、PostgreSQL、sqlx migrations 和 systemd。
- 客户端网络目标写在 `tools/preview-d2d/net.h`，当前指向 `<DEPLOY_HOST>:1337`。
- `tools/preview/` 和 `tools/preview-skia/` 是历史预览和渲染参考，不是最终交付，但在 parity 完成前有保留价值。

## 产品目标

1. 用户可以在 Windows 客户端注册、登录、持久化 session，并连接生产后端。
2. 用户可以使用个人主页、头像、状态、标签、聊天、官方频道、表情包、市场等核心社区功能。
3. 管理员可以通过后端管理面板维护用户、邀请码、频道和权限。
4. 未来订阅和游戏启动能力通过签名 `.helix`、BLAKE3、Ed25519、媒体/CDN 和客户端验签补齐。
5. 最终把 `tools/preview-d2d/` 的成熟功能迁入产品化客户端主线，减少历史预览代码。

## 技术目标

- 客户端保持 D2D + DComp + DXGI flip-model + DirectWrite 的渲染路线。
- WebView2 仅作为嵌入网页/视频能力，SDK 和 loader 通过本地下载或构建脚本获取，不进 Git。
- 后端所有数据库变化必须进入 `SystemBackend/migrations/`，并能被 `sqlx::migrate!` 应用。
- 后端构建必须在有 schema 的 PostgreSQL 上通过 `sqlx::query!` 编译期校验。
- 部署使用 `/opt/systembackend`、`systembackend.service`、`/opt/systembackend/config.toml`，真实凭据只留服务器。

## 阶段路线

### Phase A: 仓库可信化

- 修正 README、AGENTS、workflow，让新会话按仓库就能复现真实状态。
- 保证 `dist/`、`third_party/`、密钥、证书、配置不会进入 Git。
- 每次提交都通过本地客户端构建、远端后端构建或明确标注不可验证原因。

### Phase B: 当前 D2D 客户端稳定化

- 保持 `tools/preview-d2d/` 主流程可构建、可启动、可登录。
- 优先修真实用户路径：Auth、Profile、Chat、Sticker、Market、WebView2 视频/链接。
- UI 修改必须配合实际运行验证，不能只看静态代码。

### Phase C: 后端生产化

- 固化部署脚本，让 migrations、`DATABASE_URL` 编译、systemd restart、smoke test 成为标准路径。
- 管理后台和 API 必须有最小 smoke test。
- 生产配置不进入仓库，管理员密码和数据库密码只在 VPS 本地保存。

### Phase D: 产品化主线迁移

- 在 `tools/preview-d2d/` 与 `src/` 之间建立明确迁移边界。
- 完成 parity 后再决定删除旧 `tools/preview/`。
- CMake/vcpkg/Skia/Clay 路线只有在明确接回主线时才投入，不再误导 README。

### Phase E: 安全和发布

- 订阅签发、下载、验签、证书 pinning、媒体上传限制、权限模型进入独立 review。
- 发布前形成固定 checklist：构建、smoke、服务状态、Git 状态、部署记录。

## 不做的事

- 不把本地构建产物或第三方下载包提交到 Git。
- 不在没有证据时声称某个功能“已完成”。
- 不为了看起来整洁而删除历史预览代码，除非有 parity 证据。
- 不把 VPS 上的真实密码、证书、私钥写进仓库或聊天记录。
