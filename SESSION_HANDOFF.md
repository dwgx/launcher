# Session Handoff — 跨会话状态接力

> **新 Claude 会话进入仓库时**：读完 CLAUDE.md 后立即读这份，掌握当前最新状态再开干。
> 用户每次交接前会让我更新这份，所以它**永远是最新的**。
> （memory 里的 state_session_handoff.md 是这份的摘要，可能滞后一轮。）

最后更新：**2026-05-02 22:42**
最后 commit：**待提交** feat: Chat + Market + Picker + CS2 modal + admin channels + media size config
GitHub：https://github.com/dwgx/launcher (private, master)

---

## 1. 后端服务器（154.40.36.22）

| 项 | 值 |
|---|---|
| HTTPS 端点 | https://154.40.36.22:1337 |
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
0007_official_channels  chats.slug / is_official / group_label + 8 官方频道写入
                        (announcements/rules/general/random/helpdesk/cs2/market/trades)
                        — 不要 touhou/vrchat
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

- **Home**：profile-card grid 86/1fr，72×72 头像 + 14×14 online dot 3px ring，name 18 bold + email + Online，4 行 meta + history-link 链接
- **Lunching**：240×140 game-card，**真 CS2 fastly steam 缩略图**（assets/images/games/cs2_header.jpg）+ 渐变蒙版让文字可读；fallback 渐变（无图时）。点击弹出 **CS2 详情 Modal** (Steam 商店链接 + 启动按钮 + steam:// 协议)
- **Chat**（**新**）：Discord 风 240/1fr，server head + chan-group fold (▾) + chat-row + bubble。
    - 8 官方频道写死（剔除 touhou/vrchat）
    - bubble: text / sticker / gif / link card (Steam URL 自动渲染) / video (▶ 缩略+时长) / system pill / day-divider / typing 三点弹动 / 引用气泡 (3px 主色竖条 + 缩略原文 — Discord 风)
    - tail-stack：连续同作者尾部圆角拉直
    - composer：emoji/sticker/gif/attach 4 图标 + textarea + send btn (右下 38 圆 + glow)
    - Picker 320×360 三 tab (Emoji 8 列 / Sticker 8 列 / GIF 2 列渐变占位)，emoji 插入 draft、sticker/gif 直接发
    - 头像左键 → 在自己 draft 插入 @作者 (用户要求的"右键艾特")
    - 搜索 + more 行操作（design icon-btn 34）
- **Market**（**新**）：在 SHOP/market 频道里，单列卡 + search 38 + sort（热度/最新/价格）+ 5/page 分页。**卖 CS2 .cfg 参数 / 灵敏度 / autoexec / crosshair / 代练**（按用户要求"出售 cfg 参数"）。10 条 sample 含 NiKo crosshair / s1mple cfg / Faceit 代练 等
- **Cloud**：占位空状态卡
- **Settings**：seg control，三段 Language/Theme/About
- **Profile**：3 字段 UID/Username/Nickname（前两锁），上传头像/改密按钮

### 全局特性

- **全局可拖动**：WM_NCHITTEST 默认 HTCAPTION，g_hits 区域返 HTCLIENT
- **自绘 InputBox**：替换 Win32 EDIT，WM_CHAR + caret blink + password mode
- **floating label tween**（**修**）：现在用 `Tween float_t` 在 0..1 平滑插值 size + position + color（之前是单帧切换）
- **头像 hover 下拉**：scale-up from 右上原点 + auto-hide。
    - **新**：5 状态折叠菜单 (online/busy/away/sleep/offline) — 用户要求的"在线情况折叠菜单"
    - 4 行操作 (profile/history/password/signout)，每行 Claude SVG icon
- **Claude 审美 SVG icon**（**新**）：`icons.inl` 提供 30+ 图标用 GraphicsPath 路径手画 (Home/Library/Cloud/Chat/Settings/Logout/Logo/Eye/EyeOff/X/History/Search/Send/Phone/Video/More/Smile/Paperclip/Check/Check2/User/Moon/Shield/Bell/Reply/At/Link/Play/Trash/Edit/Plus/Hash)。stroke=1.7 + LineCap=Round 跟 design 一致
- **History overlay**（**修**）：460 wide design 规格 + row-item 排版 + 5/page 分页 + ‹ › 翻页 + 关闭按钮 (Ghost btn) — 全部 13 条记录

---

## 3. Claude Design 参考（用户提供的设计稿）

**位置**：`C:\Users\dwgx1\Downloads\Launcher\`
- `styles.css` 898 行（design 真理来源）
- `Launcher.html` / `views.jsx` / `components.jsx` / `tweaks-panel.jsx`
- `i18n/*.json`，`anim/*` 和 `theme/*` C++ 头

**已对齐到 Preview**：所有 design tokens / 尺寸 / Topbar / Sidebar / Home / Library(=Lunching CS) / Settings / Auth(Login)

**Design 有但 Preview 还没做**：
- **Chat view**：Discord 风 grid 240/1fr，server head + chan-group + chat-row + bubble + composer + picker(emoji/gif) + typing 动画
- **Market view**：market-grid + market-card + market-search 38 高 + market-sort 排序
- **History modal 460 wide** + row-item 排版（现在是 800-padding 占满）
- **floating label tween** 真做缓动（现在 anim_t 单帧 0/1）
- **stagger entry**（design 的 .stagger > * nth-child rise 动画）
- **真接后端**（Auth 现在是 600ms 假成功，没调 WinHTTP）

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

**真实 Skia 工程没编译过**（src/），CMakeLists 写了但没跑过 vcpkg + fetch-skia。

---

## 5. SSH 部署

```powershell
# 改后端代码后部署：
cd D:\Project\Launcher
tar --exclude='target' --exclude='*.png' --exclude='build_src' -czf /tmp/launcher_build.tgz SystemBackend
scp -i ~/.ssh/launcher_deploy ... root@154.40.36.22:/opt/systembackend/build_src/launcher_build.tgz
ssh -i ~/.ssh/launcher_deploy ... 'cd /opt/systembackend/build_src && tar xzf ... && cd SystemBackend && cargo build --release -p launcher-api && install -m 755 target/release/systembackend /opt/systembackend/systembackend && systemctl restart systembackend'
```

服务器侧每次写 CHANGELOG：
```bash
echo "$(date +%Y-%m-%d\ %H:%M) | claude-opus-4-7 | <动作> | 影响生产/不影响" >> /root/workspace/CHANGELOG.md
```

---

## 6. 用户偏好快照

- **不要在回复贴截图**，自己看自己用 Playwright/computer-use 但不嵌图
- **每轮收尾必须 build**，client 走 Preview cl.exe + server 走 cargo build；失败修不要默默交付
- **凭据零明文**：admin password / DB 密码不进对话历史；要看让用户自己 SSH grep
- **遇到不懂的事记到 QUESTIONS.md**，不打断流程问
- **代码风格**：Google C++ Style + m_ 前缀；少 RTTI/异常；关键字符串走 CRYPT_STR；注释只写 why
- **AGENTS.md 守则**（服务器有）：改生产组件前必问，CHANGELOG 必记

---

## 7. 下一步候选（用户未指定时不要主动开干）

按可能价值排序：

1. **Preview 加 Chat view** — design 完整规格在 styles.css 的 .chat-shell ~ .chat-composer
2. **Preview 加 Market view** — design 在 .market / .market-card
3. **Preview 真接 WinHTTP 调后端** — 现在 600ms 假登录，应该真 POST /api/auth/{register,login}
4. **History modal 改 460 design** — 现在是简陋占满版
5. **floating label tween** + **view fade tween** — 用户提过"刷新率/卡顿"，缓动接好就丝滑
6. **Source Han Sans CN 嵌入** — `scripts/download-fonts.ps1` 有，AddFontResourceEx 加载
7. **CS2 PNG 缩略图** — 替换文字 logo（assets/images/）
8. **真实工程 src/** Phase 2 移植 — vcpkg + fetch-skia + cmake build，把 Preview 实现搬 Skia
9. **Stripe / 微信支付** 接 credit 充值
10. **管理后台 chat 监控** — 看消息流 / 封号 / 删消息
