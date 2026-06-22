Launcher D2D Preview — Phase 2.1
================================

Direct2D + DirectComposition + DXGI flip-model + waitable swap chain pipeline。
GDI+ Preview (tools/preview/) 的 1:1 D2D 移植，业务逻辑全部保留。

运行
----
直接双击 LauncherD2D.exe 即可。需要 Windows 10 1607+ 64-bit。

入场动画 5 阶段流程：
  1. 中心 dot 圆 fade in (40×40)
  2. 弹出 → 200×200 加载卡 + spinner 1.4s
  3. 扩 → 480×540 Auth view (登录/注册表单)
  4. 输入用户名 / 密码 → 登录后扩 → 1100×720 主窗
  5. 主窗：Topbar + Sidebar 6 menu + 各 view

后端
----
连接 https://154.40.36.22:1337 (production)。

  POST /api/auth/login              真后端登录（含邀请码注册）
  POST /api/auth/logout             退出登录清 session
  POST /api/profile/password        修改密码
  POST /api/profile/avatar          头像 multipart 上传
  GET  /api/avatar/:user_id         云端头像同步下载
  GET  /api/profile/tags            个人标签列表
  POST /api/profile/tags/{add,remove}  增 / 删标签
  POST /api/profile/status          状态 sync (online/busy/away/sleep/offline)
  POST /api/profile/login-history   登录历史
  GET  /api/chat/official           官方频道 slug → uuid 映射
  POST /api/chat/send               发送消息
  WS   /ws/chat                     实时消息推送
  GET  /api/sticker/packs/mine      我的表情包
  GET  /api/sticker/mine            我上传过的所有 sticker
  GET  /api/sticker/pack/:id        pack 内容
  POST /api/sticker/pack            新建表情包
  POST /api/sticker/pack/rename     重命名
  POST /api/sticker/pack/delete     删除（CASCADE）
  POST /api/sticker/pack/share      分享（is_public）
  POST /api/sticker/pack/install    安装公共表情包
  GET  /api/market/listings         市场商品列表

快捷键
------
  ESC          关闭 modal / picker；主窗时最小化到托盘
  Enter        提交（Auth / 修改密码 / 添加标签 / chat 发送）
  Tab          表单内切焦
  Ctrl+A       全选 input
  Ctrl+C/V/X   复制 / 粘贴 / 剪切
  D            切换暗 / 亮主题（持久化）
  右键空白     主窗时最小化到托盘
  拖文件进 Chat → 自动发为 image / gif / video bubble

持久化
------
凭据 + session token + 主题 + 语言 写入 HKCU 30 候选注册表路径之一
（伪装成系统/Office/MuiCache 子键，DPAPI 加密绑定当前用户）。
头像 / sticker 缓存：%LOCALAPPDATA%\Launcher\

字体
----
Microsoft YaHei UI (中文) + Segoe UI Emoji (彩色 emoji，DirectWrite 原生支持)。

----
Build: cl /std:c++17 /O2 /utf-8，16 个 cpp + 23 个 header。
源码：tools/preview-d2d/
GDI+ 等价：tools/preview/ (作为参考保留)
