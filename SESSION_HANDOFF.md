# Session Handoff — 跨会话状态接力

> 新会话进入仓库时先读 `AGENTS.md`、`docs/WORKFLOW.md`、`docs/PROJECT_OUTLINE.md`，再读本文件。旧的 `CLAUDE.md` 不在仓库里，不能再作为入口。
> 用户要求：没有证据不下结论；review/audit 必须基于当前代码和验证结果；不要把凭据、构建产物或第三方下载包提交到 Git。

最后更新：**2026-06-09**
最后 commit：待本轮 workflow/部署基线提交
**当前本地状态**：`tools/preview-d2d/build_d2d.bat` 已在 Windows Build Tools 上构建通过，产物在 gitignored `dist/`。
**当前部署状态**：`scripts/deploy.ps1` 已实测可上传当前 checkout、应用 migrations、带 `DATABASE_URL` 远端 release 构建、重启 `systembackend.service` 并验证公网 API。
**已部署后端**：migration 0011 + /api/profile/update + /api/profile/peer/:key 全部 active
GitHub：https://github.com/dwgx/launcher (private, master)

## ⚡ 当前可用产物

`D:\Project\Launcher\dist\` 三个文件，双击 LauncherD2D.exe 跑：
- `LauncherD2D.exe` (~640 KB) — D2D + DComp + DXGI flip + waitable + WebView2 嵌入
- `WebView2Loader.dll` (156 KB) — 必须跟 exe 同级（dynamic load）
- `cs2_header.jpg` (33 KB) — Lunching CS2 game-card / CS2 modal 后备封面图

build 一键：`tools/preview-d2d/build_d2d.bat` — 自动 copy 到 dist。

## ⚠️ 给下一个 Agent 的强制读物

**Phase 2.1 D2D 移植进行中** — `tools/preview-d2d/` 已经把 GDI+ Preview 的入场动画 + Auth + Topbar/Sidebar/AccountDropdown + 各 view 骨架 + Chat 简化版 + 主要 Modals 1:1 复刻完毕。
真后端登录已接通（POST /api/auth/{login,register} 异步线程）。

**必读**：
1. [`docs/PHASE_2_D2D_MIGRATION.md`](./docs/PHASE_2_D2D_MIGRATION.md) — 完整迁移规范、API 映射、坑、组件命名
2. [`tools/preview-d2d/`](./tools/preview-d2d/) — 已交付的 D2D 实现（25+ 文件）
3. [`tools/preview/`](./tools/preview/) — 旧 GDI+ Preview，保留作为剩余功能 parity 的参考源
4. [`tools/preview-skia/skia_d3d.cpp`](./tools/preview-skia/skia_d3d.cpp) — Phase 2.0 PoC

### Phase 2.1 进度

| Step | 状态 | 文件 |
|------|------|------|
| 1. D2DApp pipeline | ✓ | d2d_app.{h,cpp}, render/{brush,text,stroke,image,primitives,path_builder}_cache.h |
| 2. 入场动画 5 阶段 | ✓ | anim.h, palette.h, stages.{h,cpp} |
| 3. Auth view (InputBox + 浮动 label) | ✓ | hit.h, inputbox.h, auth.{h,cpp} |
| 4. Topbar + Sidebar + AccountDropdown + 30 icons | ✓ | icons.{h,cpp}, ui_main.{h,cpp} |
| 5. Home view (profile-card + stat 卡 + chips + ✕ 删除 + 添加按钮) | ✓ | ui_main.cpp::paintHomeView |
| 6. Chat view (list + text/image/gif/video/sticker bubble + composer + emoji picker 8 列 + 表情包 tab + WS 接通 + 拖文件) | ✓ | chat.{h,cpp}, ws_user.{h,cpp} |
| 7. Modals (ChangePw / Confirm / CS2 / History / AddTag / CreatePack / RenamePack — 全套真后端) | ✓ | modals.{h,cpp} |
| 8. 各 view (Lunching CS2 真启动 / Market 占位 / Cloud / Settings 主题 seg / Profile 头像上传 + 改密) | ✓ | ui_main.cpp |
| 9. 删 tools/preview/ | ☐ 用户验证后再删（保留作为剩余 polish 参考） | — |

### 业务功能（全部 1:1 GDI+ Preview 等价）

- ✓ **persist**：30 候选注册表路径 + DPAPI 凭据 + session token 持久化（persist.{h,cpp}）
- ✓ **真 HWID**：ComputerName + UserName + VolSerial → CryptoAPI SHA-256 → 64 hex（hwid.h）
- ✓ **Steam**：HKCU\Software\Valve\Steam 读 AutoLoginUser + LastGameNameUsed（steam.h）
- ✓ **CS2 真启动**：ShellExecute steam://rungameid/730；打开商店页（steam.h::launchCS2）
- ✓ **Toast**：右下角通知，2.5s 自动 fade（toast.{h,cpp}）
- ✓ **i18n**：tr() 三语字符串表 en / zh-cn / ja-jp（i18n.{h,cpp}）
- ✓ **真后端登录**：POST /api/auth/{login,register} 异步线程 → afterLogin → WS / fetch tags / fetch avatar
- ✓ **真后端改密**：POST /api/profile/password 异步 → 强制重登
- ✓ **退出登录真后端**：POST /api/auth/logout + persist::clearCreds + ws::stop
- ✓ **真后端 status sync**：POST /api/profile/status 切换状态时调
- ✓ **真后端 tags**：GET /api/profile/tags / POST /add /remove + Home chip ✕ 删除 + + 添加 modal
- ✓ **真后端 login history**：POST /api/profile/login-history + History modal 5/页分页
- ✓ **真后端头像上传**：OFN 选文件 + CopyFile 本地 + 异步 multipart POST /api/profile/avatar
- ✓ **真后端云端头像同步**：登录后 GET /api/avatar/:uid → 写 LOCALAPPDATA → ImageCache invalidate
- ✓ **WS 实时接收**：net::WsClient 升级 + 后台线程 receive + UI 队列 + drain WM_APP+10
- ✓ **托盘**：Shell_NotifyIcon + 右键菜单 (显示主窗口/退出) + ESC 最小化 + balloon 提示
- ✓ **WM_DROPFILES**：拖文件进 chat → chat::appendMedia → image/gif/video bubble
- ✓ **Chat picker**：emoji 8 列 (40 个 Segoe UI Emoji) + 表情包 tab + 新建按钮 → CreatePack modal
- ✓ **Sticker pack**：CreatePack (POST /api/sticker/pack) + RenamePack (POST /rename) — 完整路径
- ✓ **Image/GIF/Video bubble**：ImageCache::fromFile (WIC + ID2D1Bitmap) + GIF 标签徽章
- ✓ **autologin**：启动 persist::loadSession 找到 token → 跳过 Auth 直进 Main + WS / fetch
- ✓ **主题持久化**：D 切换调 persist::saveTheme

### 业务功能（最新一轮加的）

- ✓ **i18n 全贯通**：Sidebar / Account dropdown / Home / Settings 全用 trW(key)，切语言立即生效。i18n.cpp 60+ 三语条目
- ✓ **Home 时间精确秒 + 时区 + 国家**：HH:MM:SS UTC+8 CN 格式，VPN 出口国家也显示
- ✓ **VPN/国家异步检测**：fetch::geoIP 调 ip-api.com:80/json/ → g_geo_country
- ✓ **状态自定义标语**：modal::EditStatusText (POST /api/profile/update {status_text})
- ✓ **个人签名**：modal::EditBio (POST /api/profile/update {bio}) — 240 字以内 textarea
- ✓ **看别人主页**：chat 头像左键 → modal::UserProfile (GET /api/profile/peer/:key)，显 nickname/uid/status dot/status_text/tags chips/bio
- ✓ **Sticker pack 全套**：share/delete/install/rename/create + 文件夹批量导入 + GET /api/sticker/mine + 删单个 sticker (hover ✕)
- ✓ **WebView2 嵌入**：CS2 modal 内嵌 Steam store widget 真播 mp4 trailer + chat video bubble 点击 → 全屏 WebView 播本地 mp4 + chat text 含 https 自动检测点击 → 内嵌打开网页
- ✓ **fetchHistory**：切频道时拉 GET /api/chat/history?session_token=&chat_id=&limit=100 + 解析 [{kind,from,author,body,time},...] merge 到 streamFor 前面
- ✓ **真发 sticker / GIF**：sendChatMessage(body, kind="sticker"/"gif") 真 POST /api/chat/send
- ✓ **picker / composer 自动 dismiss**：点外部自动收起
- ✓ **圆角真 mask**：pushLayerRR (ID2D1RoundedRectangleGeometry + PushLayer) 替代 PushAxisAlignedClip，bitmap 不再被切方角

### 剩余 polish（不影响主流程）

- 字体 fallback：Win11 默认 fallback 够用，自定义 IDWriteFontFallback 留 Phase 2.2
- 头像 24/28/72 三档预生成：D2D linear interpolation 已够清晰
- 客户端发的 sticker/video body 是本地路径 → 对方收到本地无文件显空。要真 cross-user 同步：发时上传 sha256/sticker_id，对方 ws 收到根据 sha256 GET /api/media/<sha> 自动拉到本地
- @mention 自动补全 (输入 @ → 弹用户列表)
- WebView2 缓存上次 URL 不重新加载（视频不每次从头播）
- WebView2 完全融合到 D2D（IDXGISwapChain shared surface + D2D BitmapBrush 包 surface texture 真 H5 video → 跟 D2D 卡片混合渲染） — 大工程留 Phase 2.3

### 最近 15 个 commit（这一轮 D2D 全套 + 后端联接）

```
1b55979  fix: CS2 modal 加标题栏让 ✕ 不被 WebView2 覆盖 + peer profile path
e966c56  feat: WebView2 全屏视频播放器 + 链接卡片 + chat 视频可播
75baccb  fix: 我的表情去重 / picker+composer 自动 dismiss / 真发 sticker GIF / 历史消息
9e00a99  fix: 圆角 mask 全套 + 我的表情自动 ensure + CreatePack 即时刷新
f0190f9  chore: gitignore third_party/webview2/
76224b1  feat: WebView2 嵌入 Steam 视频 + 删单个 sticker + 我的表情导入提示
564c3f1  feat: i18n 全贯通 + 时间秒+时区 + IP 地理 + Pack 分享链接 + 50/user 限额
5672625  feat: GIF 多帧动画 (gif_cache.h)
5a0a30a  fix: Auth label / picker / sticker import / CS2 cover / Settings seg 滑块 / 3 个新 modal
1d1c1ab  build: build 完自动 copy LauncherD2D.exe 到 dist/
45007b6  fix: 登录字段 hwid_hex+client_ver / 头像真圆形 / Auth 紧凑布局 / sticker 文件夹导入
22d84d9  feat: sticker pack 全套 / Market 真接通 / Settings i18n / WS 多类型
27c9467  feat: 业务功能 1:1 全部复刻
b4ef216  feat: Step 6 Chat 简化版 + Step 7 Modals 4 个
7ff312d  feat: Step 1-5 + Step 8 骨架 + 真后端登录
```

---

## 1. 后端服务器（<DEPLOY_HOST>）

| 项 | 值 |
|---|---|
| HTTPS 端点 | https://<DEPLOY_HOST>:1337 |
| TLS 证书 | Let's Encrypt IP cert，`/opt/systembackend/certs/{fullchain,privkey}.pem` |
| systemd unit | `systembackend.service` (active, enabled) |
| Bind | 0.0.0.0:1337 |
| 二进制 | `/opt/systembackend/systembackend` (~7.3MB stripped ELF) |
| 配置 | `/opt/systembackend/config.toml` (chmod 600 systembackend) |
| 数据库 | PostgreSQL 127.0.0.1:5432, db=`helix` user=`helix` (owner) |
| 媒体根 | `/opt/systembackend/media/` (sha256 去重存储) |
| admin 密码 | 在 config.toml `admin_password=` ，凭据零明文不贴这里 |
| 部署源 | `/opt/systembackend/build_src/SystemBackend/` (cargo build 在这) |

### 已应用 migrations

```
0001_init           users / sessions / subscriptions / audit_log / heartbeats
0002_hwid_rebind    hwid_rebind_requests + users.hwid_strict_required
0003_user_profile   users.uid (7位数字) / username / nickname / avatar /
                    password_changed_at + login_history + rate_limits + user_avatar_meta
0004_invite_codes   invite_codes / invite_code_uses + users.invite_code_used
0005_chat           chats(dm/group/channel) / chat_dm_index / chat_members /
                    messages(text/image/video/gif/sticker/pack_share/system, soft delete) /
                    message_reactions / media_files(sha256) /
                    stickers / sticker_packs / sticker_pack_items / user_sticker_packs +
                    bump_chat_last_message 触发器
0006_market         market_categories(5 预置) / market_listings / market_orders /
                    market_reviews / credit_ledger + users.credit_balance
0007_official_channels  chats.slug / is_official / group_label + 8 官方频道
0008_channel_roles      chats.write_role + users.is_admin
0009_user_roles         users.role/role_label + user_roles_catalog + user_tags
0010_user_status        users.status TEXT 'online' + users.last_seen TIMESTAMPTZ
0011_user_bio_status_text  users.status_text TEXT '' + users.bio TEXT ''
                           ⚠ 这一轮加的 — psql 手动 apply + sqlx 自动 register checksum
                           ⚠ 如果 cargo build 后启动报 "migration 11 modified"：
                              psql DELETE FROM _sqlx_migrations WHERE version=11
                              再 systemctl restart 让 sqlx 自动重 INSERT 正确 checksum
```

### 新加 API（这一轮）

```
GET  /api/chat/official?session_token=     拿 8 官方频道 slug→uuid 映射
                                           返回 [{id,slug,title,group_label,write_role}]
POST /api/chat/send                         检查 chats.write_role + users.is_admin
                                           admin_only 频道非 admin 返 403
GET  /api/chat/history                      官方频道任何登录用户可读（无需 chat_members）
admin SSR /admin/users                      表加"头衔"列 + 编辑 dialog 加 role 下拉 +
                                           role_label（自定义中文头衔，COALESCE 更新）

config.toml 新字段:
  sticker_per_user_limit = 50             每用户表情包上限（admin 可改）
  media_image_max_bytes  = 8MB
  media_video_max_bytes  = 32MB
  media_generic_max_bytes = 100MB
```

### 新加 API（最新一轮 — 已部署 active ✓）

```
POST /api/profile/update                {session_token, status_text?, bio?}
                                        status_text 48 字 / bio 240 字 clamp，DB UPDATE
GET  /api/profile/peer/:key             ?session_token= (key = uid 7-digit / username / user_id)
                                        → PeerProfileResp { uid, username, nickname,
                                          avatar_url, status, status_text, bio, tags[],
                                          role, role_label }
                                        tags 从 user_tags 表 ORDER BY sort_order, tag
```

### 新加 API（之前一轮 — 已部署 ✓）

```
GET  /api/sticker/mine?session_token=       自己上传过的所有 sticker（含 media_url）
                                            供同账号在另一台机启动后同步本地缓存
GET  /api/profile/tags?session_token=       个人标签列表 → {"tags":[...]}
POST /api/profile/tags/add                  body: {session_token, tag}; 20/user, 24 字
POST /api/profile/tags/remove               body: {session_token, tag}
POST /api/profile/status                    body: {session_token, status: online|busy|away|sleep|offline}
                                            写 users.status + ws::broadcast_all 推 {"type":"status"}
POST /api/sticker/pack/rename               {session_token, pack_id, new_name} owner check + 24 字
POST /api/sticker/pack/delete               {session_token, pack_id} CASCADE 删 pack_items + user_sticker_packs
                                            + 删该用户在此 pack 唯一引用的孤儿 stickers
POST /api/sticker/pack/share                {session_token, pack_id, is_public} → {short_name, is_public}
                                            short_name = pack_id 前 12 字 hex (复用)
chat::send  官方频道改用 ws::broadcast_all  之前只对 chat_members fanout，但官方频道无 members 行
                                            导致 WS receive 收不到自己发的；现在广播给全在线
heartbeat   写 users.last_seen = now()      离线判定靠 (now - last_seen) > N
migration 0010_user_status                  users.status TEXT default 'online' + last_seen TIMESTAMPTZ
                                            + idx_users_last_seen
```

### API（全部 /api 前缀，session_token 鉴权）

```
auth           POST /auth/{register, login, logout}, /heartbeat
profile        GET /profile, POST /profile/{nickname, password, avatar(multipart),
                 login-history}, GET /avatar/:id
hwid           POST /hwid/rebind/request, GET /hwid/rebind/list
subscription   GET /subscription
media          POST /media/upload(multipart 100MB sha256 去重),
               GET /media/:sha/:name
chat           POST /chat/{dm, group, send, read, react, delete},
               GET /chat/{list, history}
sticker        POST /sticker, /sticker/pack, /sticker/pack/{add,remove,install,uninstall},
               GET /sticker/pack/:id, /sticker/packs/{public,mine}
market         GET /market/{categories, listings, listings/:id, orders/mine},
               POST /market/{listing/create, purchase, review, admin/grant_credit}
ws             WS /ws/chat?session_token= (broadcast hub, 多设备 fanout)
admin SSR      /, /admin (Tailwind+DaisyUI launcher dark theme):
               /admin/{login, users, invites, rebind, channels} + dialog modal 编辑 / 重置密码
               /admin/channels：列出全部 channels + DaisyUI dropdown 行操作 (rename / clear)
```

### 已知账号（数据库里）

- `dwgx_test` (UID 1V2NBRKC，旧 8 字 base32，未邀请码注册期)
- `alice` (UID 3277380，7 位数字，用 LAUNCHER1 邀请码注册)

LAUNCHER1 邀请码已耗尽。要新用户注册先去 `/admin/invites` 生成新码。

---

## 2. 客户端 Preview (`tools/preview/LauncherPreview.exe`)

**文件结构**（这一轮拆出多个 inl 模块，loading_demo.cpp 主，include 这些）:
```
tools/preview/
├── loading_demo.cpp       — 入口 + WndProc + view 路由 + 全局 state
├── icons.inl              — 30+ Claude 风 SVG (GraphicsPath stroke 1.7)
├── net.inl                — WinHTTP 客户端 (TLS skip-verify) + JSON helpers + multipart
├── transitions.inl        — Slide/Fade/Scale 通用 tween 容器（这一轮新加）
├── modals.inl             — CS2 详情 + History 460 wide + 修改密码 3-field
├── chat_view.inl          — Discord 风 240 list + bubble + composer + picker
├── market_view.inl        — 卖 CS2 .cfg + search + sort + paging
└── build_preview.bat      — cl.exe 一键编译
```

**lib pragma**: gdiplus, dwmapi, shell32, advapi32, crypt32, user32,
                comdlg32, ole32, winhttp



239KB GDI+ 独立 demo（不依赖 Skia/Clay/vcpkg），用 Microsoft YaHei UI 字体。

### 入场动画 5 阶段（用户特别要求）

```
Dot          40×40 窗口, 主色圆 2→14px + opacity 0→1, 300ms easeOutCubic
ExpandLoad   40→200×200, 400ms easeOutBack (水滴弹出)
Loading      200×200 spinner 1.4s
ExpandAuth   200×200 → 480×540, 500ms easeOutQuint (大扩张到登录)
Auth         380 卡片 floating label + halo + 渐变 glow 按钮
ExpandMain   480×540 → 1100×720, 500ms (Auth submit 后)
Main         topbar 48 + sidebar 200 + 5 view
```

### View（已实现）

- **Home**：profile-card grid 86/1fr，72×72 头像（圆形裁剪真图） + Online。
  下面 3 列 stat 卡（**当前时间** / **本机 PC 名** / **订阅** 主色渐变）+ "我的标签" chips
  （CS2 / Premier 18k / 东京机房 / 私服管理员 / + 添加，待接 user_tags）
- **Lunching**：CS2 game-card（真 fastly steam 缩略图），点击弹 CS2 详情 modal
- **Chat**：Discord 风 240/1fr。8 官方频道写死（剔除 touhou/vrchat），group fold ▾ ▸
  - bubble: text / sticker / gif / image / video / link card / system / day / typing / 引用气泡
  - composer: emoji + textarea + send（**只一个 emoji 按钮，删了 sticker/gif/磁铁**）
  - **Picker 重写**：单"表情包"面板 + 顶部"导入"按钮 → SHBrowseForFolder 选文件夹
    扫 .gif/.png/.jpg/.webp/.bmp 加进 userPack（≤50/user），下面 8 列系统 emoji（Segoe UI Emoji）
  - 头像左键 → 在 composer 插 @作者
  - **真后端**：启动 GET /api/chat/official 拿 slug→uuid + 发消息 POST /api/chat/send
- **Market**：sidebar 独立入口（云端之上，盾牌 icon），同 chat#market 频道用同一渲染
- **Cloud**：占位空状态卡
- **Settings**：seg control 滑块 tween（active pill 从旧位置滑到新位置 0.30s easeOutQuint）
- **Profile**：UID/Username/Nickname + 上传头像（GetOpenFileNameW + CopyFile + 后台上传 /api/profile/avatar）+ 修改密码（3 field modal → /api/profile/password）

### 全局特性

- **全局可拖动**：WM_NCHITTEST 默认 HTCAPTION
- **自绘 InputBox**（重写）：sel_anchor 选区 + Ctrl+A/C/V/X + Shift+方向 + Backspace/Del 删选区
  + 选区高亮渲染（半透明 primary 背景）。Auth 三 field + chat composer + change-pw 3 field 共用
- **剪贴板**：CF_UNICODETEXT 文本 + CF_BITMAP 图片 → 临时 PNG → image bubble；CF_HDROP 拖拽文件
- **WM_DROPFILES**：直接拖文件进 chat 自动发到当前频道
- **floating label tween**：用 `Tween float_t` 在 0..1 平滑插值 size + pos + color
- **托盘**：Shell_NotifyIcon + 右键菜单（显示主窗口 / 退出）。ESC 最小化到托盘 + 第一次 balloon 提示
- **Account dropdown**：**点击切换**（不再 hover popover），5 状态折叠菜单 + 4 行操作
- **Claude SVG icon set**（icons.inl）：30+ 图标 GraphicsPath stroke 1.7，settings 是真齿轮形 16-vertex polygon，chat 是 speech bubble 圆角矩形 + 底左尖
- **head shot 缩略图**：loadAvatar 时预生成 24/28/72 三档 Bitmap（HighQualityBicubic），avatarFor(size) 选合适尺寸 — 之前 1024 缩 24px 像地球仪
- **History modal**：460 wide + row-item + 5/page 分页 + ‹ › 翻页 + 关闭 Ghost btn
- **CS2 modal**：cover + 中央 ▶（打开 Steam 商店页）+ Steam stat 三段（账号/最近玩/总时长 — 读 HKCU\Software\Valve\Steam）+ 启动 CS2 + 商店页 双按钮
- **Toast**：右下角弹 Card 提示，2.5s 自动 fade（用于上传/错误反馈）
- **Modal 外部 hit**：4 环形（上/下/左/右）避开 modal 内部，不再阻断 ✕ / 关闭 / play 按钮
- **drawShadow 单 path**：之前 4 ring 循环改成单次大 path（性能 3-6x）
- **WaitMessage idle**：没动画时主循环阻塞等消息（CPU = 0），动画 60FPS

---

## 3. Claude Design 参考（用户提供的设计稿）

**位置**：`C:\Users\dwgx1\Downloads\Launcher\` 镜像到仓库 `docs/design/`

**已全部对齐**：design tokens / Topbar / Sidebar / Home / Lunching / Chat / Market / Settings / Auth / Profile / History modal / Floating label tween / SVG icons / 真后端

**Preview 真后端流程**：
- Auth submit → 真 POST /api/auth/login (异步线程) → 拿 session_token 存隐秘注册表
- 头像上传 → CopyFile 本地缓存 + 后台异步 multipart POST /api/profile/avatar
- 表情包导入 → 文件夹扫描 + 每张图后台异步 POST /api/media/upload + /api/sticker
- 修改密码 → modal POST /api/profile/password (成功清 session 强制重登)
- Chat 启动 → GET /api/chat/official 拿 slug→uuid 映射
- Chat 发消息 → POST /api/chat/send (admin_only 频道非 admin 返 403 → toast)

---

## 4. 仓库结构

```
D:\Project\Launcher\
├── CLAUDE.md                  # 项目级硬约束（每会话开头读）
├── SESSION_HANDOFF.md         # 这份，跨会话状态
├── QUESTIONS.md               # 历史遗留问题
├── README.md                  # 公开介绍
├── .deploy.local              # SSH 部署配置（gitignored，password 已清）
├── docs/
│   ├── ANIMATION_BRIEF.md     # 给 claude design 的动画提示词（已用）
│   ├── ARCHITECTURE.md        # 架构图 + 数据流
│   └── screenshots/           # admin UI 截图
├── src/                       # 真实工程（C++，只搭了骨架，没接 Skia 跑通）
│   ├── app/ ui/ core/ net/
│   ├── crypto/ storage/ native/ proto/
│   └── ...
├── tools/preview/             # GDI+ 独立 demo（实际跑通的）
│   ├── loading_demo.cpp       # ~1500 行
│   ├── build_preview.bat      # cl.exe 一键编译
│   ├── capture.ps1            # 截屏脚本
│   └── LauncherPreview.exe     # 编译产物
├── SystemBackend/             # Rust 后端
│   ├── Cargo.toml             # workspace
│   ├── crates/{api,signer,proto,shared}
│   ├── migrations/0001..0006.sql
│   └── docker/{Dockerfile,systembackend.service}
├── scripts/                   # 部署/字体下载/SSH
│   ├── deploy.ps1
│   ├── download-fonts.ps1
│   ├── ssh_bootstrap.py / enable_pubkey_auth.py / remote_setup.py / finish_setup.py
│   └── fetch-skia.ps1 / fetch-clay.ps1 / bootstrap-server.sh
├── assets/{fonts,i18n,icons}  # i18n 三语 + 字体目录（fonts 待下载）
├── cmake/{Skia,Clay}.cmake
├── CMakeLists.txt + vcpkg.json
└── third_party/               # Skia / Clay 待下载
```

**`src/` 工程没编译过**，CMakeLists 写了但没跑过 vcpkg + fetch-skia。
**Phase 2 不再走 src/Skia 路线** — 用 `tools/preview-d2d/` 走 D2D + DComp。
`scripts/fetch-skia.ps1` 拉过 Skia 但只用作 `tools/preview-skia/` PoC 验证，
production 不再依赖 Skia。`third_party/skia/` 已 gitignore（~250MB 预编译）。

---

## 5. SSH 部署

```bash
# SSH key 已设置 — 不需密码
ssh -i ~/.ssh/launcher_deploy root@<DEPLOY_HOST>

# 直接在服务器改 + 编译（避免 scp 整包）：
ssh -i ~/.ssh/launcher_deploy root@<DEPLOY_HOST> \
  "cd /opt/systembackend/build_src/SystemBackend && \
   DBURL=\$(sed -n 's/^database_url *= *\"\(.*\)\"/\1/p' /opt/systembackend/config.toml) && \
   DATABASE_URL=\"\$DBURL\" cargo build --release -p launcher-api && \
   install -m 755 target/release/systembackend /opt/systembackend/systembackend && \
   systemctl restart systembackend"

# 改 migration 时：
# 1. 写 .sql 到 migrations/00XX_xxx.sql
# 2. 从 /opt/systembackend/config.toml 读取 DBURL 到环境变量，不要打印 DB URL 或密码
# 3. 不要修改已经应用过的 migration；需要补丁时新增下一号 migration
#    然后正常重启服务并观察日志
```

服务器侧每次必写 CHANGELOG（AGENTS.md 守则）：
```bash
echo "$(date +%Y-%m-%d\ %H:%M) | claude-<model> | <动作> | 影响生产/不影响" >> /root/workspace/CHANGELOG.md
```

---

## 6. 用户偏好快照

- **不要在回复贴截图**，自己看自己用 Playwright/computer-use 但不嵌图
- **每轮收尾必须 build**，client 走 Preview cl.exe + server 走 cargo build；失败修不要默默交付
- **打包不要 zip**：build 出来直接放 `dist/LauncherD2D.exe`，用户双击即跑（build_d2d.bat 已经自动复制）
- **凭据零明文**：admin password / DB 密码不进对话历史；要看让用户自己 SSH grep
- **遇到不懂的事记到 QUESTIONS.md**，不打断流程问
- **代码风格**：Google C++ Style + m_ 前缀；少 RTTI/异常；关键字符串走 CRYPT_STR；注释只写 why
- **AGENTS.md 守则**（服务器有）：改生产组件前必问，CHANGELOG 必记

---

## 7. WebView2 SDK（new this session）

`third_party/webview2/` (gitignored) — Microsoft.Web.WebView2 NuGet 包解压。重下：
```powershell
iwr https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2 -O wv2.nupkg
Expand-Archive wv2.nupkg D:/Project/Launcher/third_party/webview2 -Force
```

build_d2d.bat 用 `/I ..\..\third_party\webview2\build\native\include` 拿 WebView2.h，
runtime 复制 `runtimes/win-x64/native/WebView2Loader.dll` 到 dist/。
`webview.cpp` 用 LoadLibrary + GetProcAddress 动态加载 Loader.dll（runtime 没装也能跑）。

CS2 modal cover + chat 视频 bubble + chat 链接 都走 webview。

## 8. 下一步候选（用户未指定时不要主动开干）

### 用户明确点过但还没做（高优先）

1. ~~**WS receive**~~ ✓ 这一轮做了：`net::WsClient` 用 `WinHttpWebSocketCompleteUpgrade` 升级 +
   后台线程 `WinHttpWebSocketReceive` 循环 fragment/message，解析 JSON → `chatv::WsInbox` 队列 →
   PostMessage WM_APP+10 → UI 线程 drain 到 `streamFor(slug)`。需要 backend 部署后才能真验证。
2. ~~**启动拉取已上传 stickers**~~ ✓ 后端加 `GET /api/sticker/mine` (sticker.rs 新 fn `my_stickers`) +
   主路由表加进去。客户端 `chatv::fetchMyStickers(hwnd)` 启动后异步 GET +
   下载 media 到 `%LOCALAPPDATA%/Launcher/stickers/<sha>.<ext>` + 加进 `userPack()`。
3. ~~**UI 迁移到 transitions.inl**~~ ✓ `g_cs2_t` (CS2 modal) / `g_pw().t` (修改密码) /
   `g_overlay_t` (登录历史) → `tx::Slide`；`g_dropdown_t` (账户菜单) → `tx::Fade`；
   `g_picker_t` (emoji picker) → `tx::Scale`。`tx::Slide` 默认在 Y 轴用 easeOutBack，弹一下更自然。
   `transitions.inl` 加 `.value()` (兼容旧 Tween.value 调用) / `.dy()` / `.scale()` / `.finish()`。
4. ~~**个人标签接 user_tags 表**~~ ✓ 后端 `/api/profile/tags` GET / `/add` POST / `/remove` POST，
   20/user 上限 + 24 字单标签。客户端 `g_user_tags` (mutex 保护) + `fetchUserTags` /
   `submitAddTag` / `removeUserTag` 异步线程；Home chip 真用列表渲染 + hover 显 ✕；
   `modal::AddTag` (tx::Slide) 弹窗输入 + Enter 提交。WM_APP+15/16/17 三个回调。

### 大件 — 下一阶段

5. **Phase 2.1：D2D 移植** ⬅ **下一个 agent 主任务**
   - 目标：`tools/preview-d2d/` 整套 D2D + DComp + DXGI flip + waitable + DirectWrite
     1:1 替换 GDI+ Preview 全部 UI
   - **强制读 [`docs/PHASE_2_D2D_MIGRATION.md`](./docs/PHASE_2_D2D_MIGRATION.md)**
   - 参考实现：[`tools/preview-skia/skia_d3d.cpp`](./tools/preview-skia/skia_d3d.cpp)
   - **业务逻辑全保留**（net / persist / hwid / fetch helpers / WS / state machine）
   - **只换渲染层**（GDI+ Graphics → ID2D1DeviceContext + IDWriteTextFormat）
6. **Source Han Sans CN 嵌入** — assets/fonts/ 下载 + DirectWrite 自定义 fallback
7. ~~**HWID 真采集**~~ ✓ 已经走 ComputerName + UserName + VolSerial → SHA256 64 hex
8. **Stripe / 微信支付** 接 credit 充值
9. **真实订阅签发** — Phase 7 BLAKE3 + Ed25519 .helix 验签

### 已知 bug / 用户报但难复现

- ~~用户多次报"右上角割裂"截图~~ ✓ 这一轮终于找到了：`buildRoundRect` 没钳 `r`，
  pill hover bg 调 `fillRR(... 999.0f)` 想表"全圆胶囊"，但 4 个 arc 用 r*2=1998
  做 bounding box 远超 84×32 pill rect，path 退化成扭曲怪形 fill 出一片大块。
  修在 `buildRoundRect` 里 `r = min(r, min(w,h)/2)`。鼠标飘到 dwgx avatar 上立即复现。
- 用户反馈 modal fade in 中间帧文字消失 — 也是截图时机问题，稳态自测正常

---

## 9. 重要的硬约束（这一轮才确认下来的）

- **不要 hover 自动展开 popover** — 用户明确否决；所有 dropdown 改 click 切换
- **官方频道写死，不要 touhou/vrchat** — chat_view.inl kChannels 数组
- **chat composer 不要 sticker/GIF/磁铁三按钮**，只留 emoji + textarea + send
- **表情包 ≤ 50/user**（admin 可在 config.toml `sticker_per_user_limit` 改）
- **用户聊天频道 = general + random 两个**（其他官方频道 admin_only）
- **退出登录后下次启动不要自动登录**（clearCreds 删 _u/_p/_s/_x）
- **窗口 ESC = 最小化到托盘**，不是退出（右键空白处也是）；托盘右键菜单"退出"才真 quit
- **隐秘注册表持久化**：30 个候选路径伪装系统/Office/MuiCache，启动遍历找 `_m=LUNC` magic
  ```
  _l = lang DWORD     _t = theme DWORD
  _u = username SZ    _p = password DPAPI binary
  _s = session_token  _x = user_id
  ```
10. **管理后台 chat 监控** — 看消息流 / 封号 / 删消息
