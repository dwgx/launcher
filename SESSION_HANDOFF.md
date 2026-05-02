# Session Handoff — 跨会话状态接力

> **新 Claude 会话进入仓库时**：读完 CLAUDE.md 后立即读这份，掌握当前最新状态再开干。
> 用户每次交接前会让我更新这份，所以它**永远是最新的**。
> （memory 里的 state_session_handoff.md 是这份的摘要，可能滞后一轮。）

最后更新：**2026-05-03 06:05**
最后 commit：**`d966cd6`** feat(preview): sticker 真上传 + transitions 框架
GitHub：https://github.com/dwgx/launcher (private, master)

### 最近 10 个 commit（这一轮的密集改动）

```
d966cd6  sticker 真上传 + transitions 框架
0c29a1b  修改密码 modal + chat send 真后端 + admin 角色编辑
60e6b0d  WinHTTP × backend 真接通 + 滚动日志 + 打勾动画 + 头像缩略图
1f84cc7  sidebar 加 market 入口 + 头像本地缓存真生效
bef4ca8  表情包文件夹导入 + 主页扩展 + 后端 user_roles + 50 上限
72bc9d6  真齿轮/聊天泡图标 + 卡片切换动画 + 滑块 seg + 按钮立体 + 性能
6c43844  sidebar SVG icons + auto-login 动画 + chat 折叠 + 频道写权限
aa96f04  chat 内嵌图片/视频粘贴 + 拖拽文件 + GDI+ image bubble
22f6347  InputBox 完整键盘 + 选区 + 隐秘注册表 + DPAPI 自动登录
23d810c  modal/dropdown 点击阻断 + 托盘 balloon + 红 badge 清零
```

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
0008_channel_roles      chats.write_role (admin_only / user) + users.is_admin
                        general/random = user (聊天频道 2 个)，其他 = admin_only
0009_user_roles         users.role/role_label + user_roles_catalog (4 预设：
                        admin/oldhand/newhand/user 中文头衔) + user_tags
                        ⚠ 改过 hash 后 _sqlx_migrations DELETE row 9 重跑过
                        ⚠ 表 owner = helix（兜底 ALTER OWNER 在 SQL 里）
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

### 用户明确点过但还没做（高优先）

1. **WS receive** — `WinHttpWebSocketCompleteUpgrade` 后台线程接 /ws/chat?session_token=…
   收到 message JSON 解析 → push 进 streamFor(channel) → PostMessage WM_APP+10 触发重绘
   （已经有 WM_APP+10 的 case 处理但没实际接收）
2. **启动拉取已上传 stickers** — GET /api/sticker/mine（需新加 endpoint）
   下载 media 到本地 cache + 加进 userPack — 用户在另一台机也能看到自己上传的
3. **UI 层迁移到 transitions.inl** — 把现有零散 Tween 调用包装成 tx::Slide / tx::Fade / tx::Scale
   modal 入场 / view 切换 / popover 都用统一 transition 接口
4. **个人标签接 user_tags 表** — Home view "+添加" → modal 输入 → POST 持久化

### 大件（用户没明说但合理）

5. **Phase 2 真实 src/ 工程跑通** — vcpkg + fetch-skia + cmake build，把 Preview 整个搬到 Skia + Clay
   （当前 src/ 只是骨架 + design 给的几个 .h，从未编译）
6. **Source Han Sans CN 嵌入** — assets/fonts/ 下载 + AddFontResourceEx 加载
7. **HWID 真采集** — 现在 Preview 用 "launcher-preview-demo" 字面量
8. **Stripe / 微信支付** 接 credit 充值
9. **真实订阅签发** — Phase 7 BLAKE3 + Ed25519 .helix 验签

### 已知 bug / 用户报但难复现

- 用户多次报"右上角割裂"截图 — PrintWindow 自测复现不出，可能是窗口被拖到屏幕外的截图边缘伪影
- 用户反馈 modal fade in 中间帧文字消失 — 也是截图时机问题，稳态自测正常

---

## 8. 重要的硬约束（这一轮才确认下来的）

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
