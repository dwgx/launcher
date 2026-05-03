// Chat view — Discord 风 240/1fr。
// 设计参考：design/styles.css .chat-shell 起，design/views.jsx ChatView。
// 频道写死官方（不要 touhou/vrchat）；后端 admin 可在 /admin/channels 改名/清理。
#pragma once

namespace chatv {

// ============== 数据 ==============
struct Channel {
    const wchar_t* id;
    const wchar_t* name;
    const wchar_t* group;       // IMPORTANT / GENERAL / GAMES / SHOP
    int unread{0};
    bool is_market{false};
};

// 官方频道 — 没有 touhou/vrchat（按用户要求剔除）。unread 置 0，等真消息流接通
const Channel kChannels[] = {
    { L"announcements", L"announcements", L"IMPORTANT", 0, false },
    { L"rules",         L"rules",         L"IMPORTANT", 0, false },
    { L"general",       L"general",       L"GENERAL",   0, false },
    { L"random",        L"random",        L"GENERAL",   0, false },
    { L"helpdesk",      L"helpdesk",      L"GENERAL",   0, false },
    { L"cs2",           L"cs2",           L"GAMES",     0, false },
    { L"market",        L"market",        L"SHOP",      0, true  },
    { L"trades",        L"trades",        L"SHOP",      0, false },
};
const wchar_t* kGroups[] = { L"IMPORTANT", L"GENERAL", L"GAMES", L"SHOP" };

enum class MsgKind { Text, Sticker, Gif, System, DayDivider, Typing, LinkCard, Video, Image };

struct Msg {
    MsgKind kind{MsgKind::Text};
    const wchar_t* from{L"yuki"};       // "me" 表示自己
    const wchar_t* author{L""};         // 显示名
    const wchar_t* status{L"online"};   // online/busy/away/sleep/offline
    const wchar_t* body{L""};           // text 内容 / sticker emoji / gif title / day text / link url / video file
    const wchar_t* time{L""};
    bool read{false};
    // 引用 (Discord 风)
    const wchar_t* reply_author{L""};
    const wchar_t* reply_excerpt{L""};
    // 链接卡
    const wchar_t* link_title{L""};
    const wchar_t* link_host{L""};
    // 视频
    int video_seconds{0};
};

// 频道消息流 — 一律从空开始；后端真正接通后由 WS 推送填充
std::vector<Msg>& streamFor(const wchar_t* chid) {
    static std::unordered_map<std::wstring, std::vector<Msg>> g_streams;
    auto it = g_streams.find(chid);
    if (it == g_streams.end()) {
        it = g_streams.emplace(std::wstring(chid), std::vector<Msg>{}).first;
    }
    return it->second;
}

// ============== 媒体附件存储 ==============
struct Media {
    enum Kind { KImage, KGif, KVideo, KFile } kind{KImage};
    std::wstring path;
    Gdiplus::Image* img{nullptr};   // GDI+ image (image/gif 解码后)
    int width{0}, height{0};
};
inline std::unordered_map<std::wstring, Media>& mediaCache() {
    static std::unordered_map<std::wstring, Media> m;
    return m;
}

// 从路径推 kind + 解码（image/gif）
const Media* loadMedia(const std::wstring& path) {
    auto& cache = mediaCache();
    auto it = cache.find(path);
    if (it != cache.end()) return &it->second;
    Media m; m.path = path;
    std::wstring s = path;
    auto dot = s.find_last_of(L'.');
    std::wstring suf = (dot != std::wstring::npos) ? s.substr(dot) : L"";
    for (auto& c : suf) c = (wchar_t)towlower(c);
    if (suf == L".png" || suf == L".jpg" || suf == L".jpeg" || suf == L".webp" || suf == L".bmp") {
        m.kind = Media::KImage;
        m.img = Gdiplus::Image::FromFile(path.c_str());
    } else if (suf == L".gif") {
        m.kind = Media::KGif;
        m.img = Gdiplus::Image::FromFile(path.c_str());
    } else if (suf == L".mp4" || suf == L".webm" || suf == L".mov" || suf == L".avi" || suf == L".mkv") {
        m.kind = Media::KVideo;
    } else {
        m.kind = Media::KFile;
    }
    if (m.img && m.img->GetLastStatus() == Gdiplus::Ok) {
        m.width = m.img->GetWidth();
        m.height = m.img->GetHeight();
    } else if (m.img) {
        delete m.img; m.img = nullptr;
    }
    auto [ins, _] = cache.emplace(path, std::move(m));
    return &ins->second;
}

// 文件名（路径最后一段）
inline std::wstring basename(const std::wstring& p) {
    auto pos = p.find_last_of(L"\\/");
    return (pos == std::wstring::npos) ? p : p.substr(pos + 1);
}

// 长生命周期 path 字符串 — deque 防 push_back 移走 SSO 短串导致 c_str() 变垃圾
inline std::deque<std::wstring>& mediaPathStore() {
    static std::deque<std::wstring> v; return v;
}

// ============== 状态 ==============
const wchar_t* g_active{L"general"};
InputBox       g_composer;          // 完整 InputBox：选区 + Ctrl+A/C/V/X
bool           g_picker_open{false};

// 官方频道 slug → backend UUID 映射（启动后由 /api/chat/official 填充）
inline std::unordered_map<std::wstring, std::string>& slugToUuid() {
    static std::unordered_map<std::wstring, std::string> m;
    return m;
}

// 启动后异步拉取官方频道映射 (用全局命名空间的 g_session_token)
inline void fetchOfficialChannels(HWND notify_hwnd) {
    if (::g_session_token.empty()) return;
    struct A { HWND h; };
    A* a = new A{notify_hwnd};
    CreateThread(nullptr, 0, [](LPVOID lp) -> DWORD {
        auto* a = (A*)lp;
        std::string url = "/api/chat/official?session_token=" + ::g_session_token;
        std::wstring wurl(url.begin(), url.end());
        auto r = net::request(L"GET", wurl.c_str(), "", L"");
        if (r.ok()) {
            // 解析 [{"id":"uuid","slug":"general",...},{...}]
            auto& m = slugToUuid();
            size_t pos = 0;
            while (true) {
                pos = r.body.find("\"slug\":\"", pos);
                if (pos == std::string::npos) break;
                pos += 8;
                size_t e = r.body.find('"', pos);
                std::string slug = r.body.substr(pos, e - pos);
                size_t ip = r.body.rfind("\"id\":\"", pos);
                if (ip == std::string::npos) break;
                ip += 6;
                size_t ie = r.body.find('"', ip);
                std::string uuid = r.body.substr(ip, ie - ip);
                std::wstring wslug(slug.begin(), slug.end());
                m[wslug] = uuid;
                pos = e;
            }
            PostMessageW(a->h, WM_APP + 11, 1, 0);
        } else {
            PostMessageW(a->h, WM_APP + 11, 0, 0);
        }
        delete a;
        return 0;
    }, a, 0, nullptr);
}

// ========== WebSocket 实时接收 ==========
// 后台线程收到的 message JSON 解析后，先丢这个队列；UI 线程在 WM_APP+10 一次性 drain。
// str_pool 用 deque 而非 vector — push_back 不会让已有元素地址失效（SSO 短串内嵌在对象内，
// 用 vector 重分配会把 c_str() 指向已被销毁的 SSO 内存）。
struct WsInbox {
    std::mutex mu;
    std::vector<Msg> pending;
    std::deque<std::wstring> str_pool;
};
inline WsInbox& wsInbox() { static WsInbox b; return b; }

inline net::WsClient& wsClient() { static net::WsClient c; return c; }

// 反查 chat_id (uuid) → slug
inline std::wstring uuidToSlug(const std::string& uuid) {
    auto& m = slugToUuid();
    for (auto& kv : m) if (kv.second == uuid) return kv.first;
    return L"";
}

// 把 chars 转 wchar_t* 并写进 str_pool，返回常驻指针
inline const wchar_t* poolWString(WsInbox& b, const std::string& utf8) {
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, 0);
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, w.data(), n);
    b.str_pool.push_back(std::move(w));
    return b.str_pool.back().c_str();
}

// 解析 {"type":"message","data":{...}} 一条 push，丢进 pending
inline void parseAndEnqueue(const std::string& body, HWND notify_hwnd) {
    // 类型判断 — 只关心 message
    if (body.find("\"type\":\"message\"") == std::string::npos) return;

    // 取 data 字段子串
    auto dp = body.find("\"data\":");
    if (dp == std::string::npos) return;

    // 提取 chat_id, msg_type, sender_id, payload.text/url
    std::string chat_id  = net::jsonStr(body, "chat_id");
    std::string msg_type = net::jsonStr(body, "msg_type");
    std::string sender   = net::jsonStr(body, "sender_id");
    if (chat_id.empty() || msg_type.empty()) return;

    // payload.text — 当 type=text；payload.url — 当 type=image/video/gif/sticker
    std::string text;
    auto pl = body.find("\"payload\":", dp);
    if (pl != std::string::npos) {
        text = net::jsonStr(body.substr(pl), "text");
        if (text.empty()) text = net::jsonStr(body.substr(pl), "url");
    }

    std::wstring slug = uuidToSlug(chat_id);
    if (slug.empty()) return;   // 非官方频道（暂不显示）

    // 是自己发的？后端 push 给所有成员（包括自己用于多端同步）但本端 send 时已经
    // 本地 echo 过 → 直接 drop，否则同一条消息会显示两次。
    // (代价：多设备登录的另一端看不到这一条，等 fetchHistory 才会同步过来；
    //  Phase 7+ 用 message_id dedup 解决多端实时同步。)
    if (sender == ::g_user_id && !sender.empty()) return;

    auto& b = wsInbox();
    std::lock_guard<std::mutex> lk(b.mu);
    Msg m;
    m.from   = poolWString(b, sender);
    m.author = poolWString(b, sender);
    m.status = L"online";
    m.read   = false;
    m.time   = L"now";
    if (msg_type == "text") {
        m.kind = MsgKind::Text;
        m.body = poolWString(b, text);
    } else if (msg_type == "image") {
        m.kind = MsgKind::Image;
        m.body = poolWString(b, text);
    } else if (msg_type == "gif" || msg_type == "sticker") {
        m.kind = MsgKind::Gif;
        m.body = poolWString(b, text);
    } else if (msg_type == "video") {
        m.kind = MsgKind::Video;
        m.body = poolWString(b, text);
    } else {
        m.kind = MsgKind::System;
        m.body = poolWString(b, text);
    }
    // 记录目标频道 — 用 status 字段位临时存（drain 时再分流），避免持有非线程安全的 streamFor。
    b.str_pool.push_back(slug);
    m.status = b.str_pool.back().c_str();
    b.pending.push_back(m);

    PostMessageW(notify_hwnd, WM_APP + 10, 0, 0);
}

// UI 线程在 WM_APP+10 调 — 把 pending 分流到 streamFor(slug)。
inline void drainWsInbox() {
    auto& b = wsInbox();
    std::lock_guard<std::mutex> lk(b.mu);
    for (auto& m : b.pending) {
        // m.status 当时被存为 slug；恢复 online，把 slug 拿出来分流
        std::wstring slug = m.status ? m.status : L"";
        m.status = L"online";
        if (slug.empty()) continue;
        streamFor(slug.c_str()).push_back(m);
    }
    b.pending.clear();
}

// 切频道时拉历史 — 一个频道只拉一次（缓存"已拉"状态防重复）
inline std::unordered_map<std::wstring, bool>& historyFetched() {
    static std::unordered_map<std::wstring, bool> m;
    return m;
}

inline void fetchHistory(HWND notify_hwnd, const std::wstring& slug) {
    auto& m = slugToUuid();
    auto it = m.find(slug);
    if (it == m.end() || ::g_session_token.empty()) return;
    if (historyFetched()[slug]) return;
    historyFetched()[slug] = true;

    struct A { std::wstring slug; std::string uuid; HWND h; };
    A* a = new A{slug, it->second, notify_hwnd};
    CreateThread(nullptr, 0, [](LPVOID lp) -> DWORD {
        auto* a = (A*)lp;
        std::string url = "/api/chat/history?session_token=" + ::g_session_token
            + "&chat_id=" + a->uuid + "&limit=50";
        std::wstring wurl(url.begin(), url.end());
        auto r = net::request(L"GET", wurl.c_str(), "", L"");
        if (!r.ok()) {
            historyFetched()[a->slug] = false;   // 失败：允许下次重试
            delete a; return 0;
        }
        // body 是 [MessageOut, ...] 倒序（DESC）— 我们要正序贴到 streamFor 前面
        auto& b = wsInbox();
        std::vector<Msg> parsed;
        size_t pos = 0;
        while (true) {
            auto p_id = r.body.find("\"id\":", pos);
            if (p_id == std::string::npos) break;
            // 取 sender_id / msg_type / payload.text
            auto next = r.body.find("\"id\":", p_id + 5);
            std::string seg = r.body.substr(p_id, (next == std::string::npos)
                                                  ? std::string::npos : next - p_id);
            std::string sender   = net::jsonStr(seg, "sender_id");
            std::string msg_type = net::jsonStr(seg, "msg_type");
            std::string text;
            auto pl = seg.find("\"payload\":");
            if (pl != std::string::npos) {
                std::string sub = seg.substr(pl);
                text = net::jsonStr(sub, "text");
                if (text.empty()) text = net::jsonStr(sub, "url");
            }
            if (msg_type.empty() || (text.empty() && msg_type != "system")) {
                pos = (next == std::string::npos) ? r.body.size() : next;
                continue;
            }
            std::lock_guard<std::mutex> lk(b.mu);
            Msg m;
            bool is_me = (sender == ::g_user_id) && !sender.empty();
            m.from   = is_me ? L"me" : poolWString(b, sender);
            m.author = poolWString(b, sender);
            m.status = L"online";
            m.read = true; m.time = L"";
            if (msg_type == "text")              { m.kind = MsgKind::Text;  m.body = poolWString(b, text); }
            else if (msg_type == "image")        { m.kind = MsgKind::Image; m.body = poolWString(b, text); }
            else if (msg_type == "gif"
                  || msg_type == "sticker")      { m.kind = MsgKind::Gif;   m.body = poolWString(b, text); }
            else if (msg_type == "video")        { m.kind = MsgKind::Video; m.body = poolWString(b, text); }
            else                                 { m.kind = MsgKind::System;m.body = poolWString(b, text); }
            parsed.push_back(m);
            pos = (next == std::string::npos) ? r.body.size() : next;
        }
        // backend 返回 DESC（最新在前）— 反转成时间正序
        std::reverse(parsed.begin(), parsed.end());

        // 把历史塞进 wsInbox.pending（用 status 字段当 slug 标记，drain 时分流）
        {
            std::lock_guard<std::mutex> lk(b.mu);
            for (auto& m : parsed) {
                b.str_pool.push_back(a->slug);
                m.status = b.str_pool.back().c_str();
                b.pending.push_back(m);
            }
        }
        PostMessageW(a->h, WM_APP + 10, 0, 0);
        delete a;
        return 0;
    }, a, 0, nullptr);
}

// 启动 WS 连接 — 登录成功 / 启动有 session 时调一次
inline void startWebSocket(HWND notify_hwnd) {
    if (::g_session_token.empty()) return;
    auto& ws = wsClient();
    // 已连：先关再重连（多设备登录或 token 刷新场景）
    ws.close();
    std::wstring path = L"/ws/chat?session_token=";
    for (char c : ::g_session_token) path.push_back((wchar_t)c);
    HWND h = notify_hwnd;
    ws.connect(path, [h](const std::string& body) {
        parseAndEnqueue(body, h);
    });
}

// 异步发消息到当前频道（POST /api/chat/send）
inline void sendTextMessage(HWND notify_hwnd, const std::wstring& text) {
    if (text.empty()) return;
    auto& m = slugToUuid();
    auto it = m.find(g_active);
    if (it == m.end() || ::g_session_token.empty()) {
        // 离线模式 — 仅本地显示
        return;
    }
    struct A { std::string uuid, body; HWND h; };
    A* a = new A;
    a->uuid = it->second;
    a->body = std::string("{\"session_token\":\"") + ::g_session_token
        + "\",\"chat_id\":\"" + it->second
        + "\",\"msg_type\":\"text\",\"payload\":{\"text\":\""
        + net::jsonEscape(text) + "\"}}";
    a->h = notify_hwnd;
    CreateThread(nullptr, 0, [](LPVOID lp) -> DWORD {
        auto* a = (A*)lp;
        auto r = net::postJson(L"/api/chat/send", a->body);
        PostMessageW(a->h, WM_APP + 12, r.ok() ? 1 : 0, (LPARAM)(intptr_t)r.status);
        delete a;
        return 0;
    }, a, 0, nullptr);
}
inline std::unordered_map<std::wstring, bool>& groupCollapsed() {
    static std::unordered_map<std::wstring, bool> m;
    return m;
}
int            g_picker_tab{0};   // 0=emoji 1=sticker 2=gif
tx::Scale      g_picker_t;        // emoji/sticker picker popover：scale 0.94→1 + op 0↔1
int            g_streams_dirty_index{-1};   // 上次 active 切换的标记，触发 fade
bool           g_focus_composer{false};
int            g_at_menu_index{-1};   // 显示头像右键菜单的 message index, -1 关闭
RectF          g_at_menu_rect{};

// 计时驱动 typing dots
float g_typing_t = 0.0f;

void switchChannel(const wchar_t* id) {
    g_active = id;
    g_picker_open = false;
    fetchHistory(g_hwnd, id);   // 拉一次历史（缓存）
}

// 把一个文件路径作为 media message 加到当前频道
void appendMedia(const std::wstring& path) {
    if (path.empty()) return;
    const Media* m = loadMedia(path);
    if (!m) return;
    auto& store = mediaPathStore();
    store.push_back(path);
    Msg msg; msg.from = L"me"; msg.author = L""; msg.status = L"online";
    msg.read = false; msg.time = L"now";
    msg.body = store.back().c_str();
    switch (m->kind) {
        case Media::KImage: msg.kind = MsgKind::Image; break;
        case Media::KGif:   msg.kind = MsgKind::Gif;   break;
        case Media::KVideo: msg.kind = MsgKind::Video; break;
        case Media::KFile:  msg.kind = MsgKind::Text;  break;
    }
    streamFor(g_active).push_back(msg);
}

// 保存 HBITMAP 到临时 PNG，返回路径
std::wstring saveBitmapToTempPng(HBITMAP hbm) {
    if (!hbm) return L"";
    Gdiplus::Bitmap b(hbm, nullptr);
    wchar_t tmp[MAX_PATH], file[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    GetTempFileNameW(tmp, L"lpv", 0, file);
    std::wstring out = file; out += L".png";
    DeleteFileW(file);
    CLSID clsid;
    UINT num = 0, sz = 0;
    Gdiplus::GetImageEncodersSize(&num, &sz);
    if (sz == 0) return L"";
    std::vector<BYTE> buf(sz);
    auto* enc = (Gdiplus::ImageCodecInfo*)buf.data();
    Gdiplus::GetImageEncoders(num, sz, enc);
    for (UINT i = 0; i < num; ++i) {
        if (wcscmp(enc[i].MimeType, L"image/png") == 0) {
            clsid = enc[i].Clsid; break;
        }
    }
    if (b.Save(out.c_str(), &clsid, nullptr) == Gdiplus::Ok) return out;
    return L"";
}

// 从剪贴板尝试粘贴媒体（图片 / 文件 drop）。返回 true 表示已消费。
bool tryPasteMedia(HWND hwnd) {
    if (!OpenClipboard(hwnd)) return false;
    bool consumed = false;
    // 先看 HDROP（拖拽 / 复制文件）
    if (HANDLE h = GetClipboardData(CF_HDROP)) {
        HDROP drop = (HDROP)h;
        UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < n; ++i) {
            wchar_t buf[MAX_PATH];
            if (DragQueryFileW(drop, i, buf, MAX_PATH)) {
                appendMedia(buf);
                consumed = true;
            }
        }
    }
    // 再看图片 bitmap
    if (!consumed) {
        if (HANDLE h = GetClipboardData(CF_BITMAP)) {
            std::wstring tmp = saveBitmapToTempPng((HBITMAP)h);
            if (!tmp.empty()) { appendMedia(tmp); consumed = true; }
        }
    }
    CloseClipboard();
    return consumed;
}

bool isMarketChannel() {
    for (auto& c : kChannels) if (wcscmp(c.id, g_active) == 0) return c.is_market;
    return false;
}
const Channel* activeChannel() {
    for (auto& c : kChannels) if (wcscmp(c.id, g_active) == 0) return &c;
    return &kChannels[2];   // general
}

// 判定一个 UTF-16 单元是否落在 emoji 范围（含高代理对的高位）。
// emoji 范围简化：
//   - 高代理 0xD83C..0xD83F → 任何 SMP emoji (U+1F000+) 都从这里开始
//   - BMP 杂项符号 / 装饰符号 0x2600..0x27BF
//   - 装饰附加 (FE0F variation selector) 也算同段
inline bool isEmojiUnit(wchar_t c) {
    return (c >= 0xD83C && c <= 0xD83F)
        || (c >= 0x2600 && c <= 0x27BF)
        || (c >= 0x2300 && c <= 0x23FF)
        || (c == 0xFE0F);
}
inline bool isLowSurrogate(wchar_t c) { return c >= 0xDC00 && c <= 0xDFFF; }

// 文字 + emoji 混排：YaHei UI 走 text，Segoe UI Emoji 走 emoji 段。
// 同一 baseline 顺序绘制；返回总宽度（用于布局）。
inline float drawTextWithEmoji(Graphics& g, const wchar_t* text, int len,
                               float x, float y, float size_pt,
                               Color text_color, FontStyle fs = FontStyleRegular) {
    if (!text || len <= 0) return 0.0f;
    Font text_font(kFontFace, size_pt, fs, UnitPoint);
    Font emoji_font(L"Segoe UI Emoji", size_pt, FontStyleRegular, UnitPoint);
    SolidBrush brush(text_color);
    StringFormat fmt; fmt.SetAlignment(StringAlignmentNear);
    // 关键：FormatFlagsMeasureTrailingSpaces 让 MeasureString 返回真实宽度
    fmt.SetFormatFlags(StringFormatFlagsMeasureTrailingSpaces | StringFormatFlagsNoWrap);

    float cur_x = x;
    int i = 0;
    while (i < len) {
        bool em = isEmojiUnit(text[i]);
        int j = i;
        // 收集一段同类型 run
        while (j < len) {
            bool em_j = isEmojiUnit(text[j]);
            if (em_j != em) break;
            // surrogate pair 一起算
            if (em && j + 1 < len && isLowSurrogate(text[j + 1])) j += 2;
            else j += 1;
        }
        std::wstring run(text + i, text + j);
        Font* use = em ? &emoji_font : &text_font;
        RectF mb;
        g.MeasureString(run.c_str(), (int)run.size(), use, PointF(0, 0), &fmt, &mb);
        g.DrawString(run.c_str(), (int)run.size(), use,
                     RectF(cur_x, y, mb.Width + 2.0f, size_pt * 3.0f), &fmt, &brush);
        cur_x += mb.Width;
        i = j;
    }
    return cur_x - x;
}

// 多行版本：碰到换行用本地宽度回卷。简化版 — 不做 word-break，纯按宽度切。
inline float drawWrappedTextWithEmoji(Graphics& g, const wchar_t* text, int len,
                                      float x, float y, float max_w, float size_pt,
                                      Color text_color, FontStyle fs = FontStyleRegular,
                                      float* out_height = nullptr) {
    if (!text || len <= 0) { if (out_height) *out_height = 0; return 0; }
    Font text_font(kFontFace, size_pt, fs, UnitPoint);
    Font emoji_font(L"Segoe UI Emoji", size_pt, FontStyleRegular, UnitPoint);
    SolidBrush brush(text_color);
    StringFormat fmt; fmt.SetAlignment(StringAlignmentNear);
    fmt.SetFormatFlags(StringFormatFlagsMeasureTrailingSpaces | StringFormatFlagsNoWrap);

    float cur_x = x, cur_y = y;
    float line_h = size_pt * 1.55f;   // 行高
    float used_w = 0;
    int i = 0;
    while (i < len) {
        // 取一个 codepoint（surrogate pair 或单 char 或 emoji 序列）
        int unit_len = 1;
        if (text[i] >= 0xD83C && text[i] <= 0xD83F && i + 1 < len) unit_len = 2;
        bool em = isEmojiUnit(text[i]);
        std::wstring ch(text + i, text + i + unit_len);
        Font* use = em ? &emoji_font : &text_font;
        RectF mb;
        g.MeasureString(ch.c_str(), (int)ch.size(), use, PointF(0, 0), &fmt, &mb);
        if (cur_x - x + mb.Width > max_w && cur_x > x) {
            cur_x = x; cur_y += line_h;
        }
        g.DrawString(ch.c_str(), (int)ch.size(), use,
                     RectF(cur_x, cur_y, mb.Width + 2.0f, size_pt * 3.0f), &fmt, &brush);
        cur_x += mb.Width;
        if (cur_x - x > used_w) used_w = cur_x - x;
        i += unit_len;
        // 显式换行
        if (text[i - unit_len] == L'\n') {
            cur_x = x; cur_y += line_h;
        }
    }
    if (out_height) *out_height = (cur_y + line_h) - y;
    return used_w;
}

// ============== 渲染辅助 ==============
Color statusColor(const Palette& pal, const wchar_t* status) {
    if (wcscmp(status, L"online") == 0) return pal.status_online;
    if (wcscmp(status, L"busy")   == 0) return pal.status_busy;
    if (wcscmp(status, L"away")   == 0) return pal.status_away;
    if (wcscmp(status, L"sleep")  == 0) return pal.status_sleep;
    return pal.status_offline;
}

void drawAvatar(Graphics& g, float x, float y, float r, const wchar_t* name,
                const wchar_t* status, const Palette& pal) {
    SolidBrush bg(pal.primary);
    g.FillEllipse(&bg, x, y, r*2, r*2);
    Font f(kFontFace, r * 0.78f, FontStyleBold, UnitPixel);
    SolidBrush ft(Color(255, 255, 255, 255));
    StringFormat fmt; fmt.SetAlignment(StringAlignmentCenter); fmt.SetLineAlignment(StringAlignmentCenter);
    wchar_t init[2] = { (wchar_t)towupper(name && name[0] ? name[0] : L'?'), 0 };
    g.DrawString(init, -1, &f, RectF(x, y, r*2, r*2), &fmt, &ft);
    if (status && status[0]) {
        float dr = r * 0.28f;
        SolidBrush sb(statusColor(pal, status));
        g.FillEllipse(&sb, x + r*2 - dr*2, y + r*2 - dr*2, dr*2, dr*2);
        Pen ring(pal.bg, 2.0f);
        g.DrawEllipse(&ring, x + r*2 - dr*2, y + r*2 - dr*2, dr*2, dr*2);
    }
}

// ============== 频道列表 ==============
void paintChatList(Graphics& g, RectF area) {
    const Palette& pal = palette();
    fillRR(g, area.X, area.Y, area.Width, area.Height, 0, pal.bg);
    Pen sep(pal.divider, 1.0f);
    g.DrawLine(&sep, area.X + area.Width, area.Y, area.X + area.Width, area.Y + area.Height);

    // header 14 + glyph 26 + label
    float hy = area.Y + 12;
    drawText_(g, L"Launcher Server", area.X + 50, hy + 2, 200,
              11.0f, pal.text, StringAlignmentNear, FontStyleBold);
    SolidBrush gbg(pal.primary);
    fillRR(g, area.X + 14, hy - 2, 26, 26, 7.0f, pal.primary);
    icons::drawSvg(g, icons::Name::Logo, area.X + 14 + 5, hy - 2 + 5, 16,
                   Color(255, 255, 255, 255));
    g.DrawLine(&sep, area.X + 8, area.Y + 44, area.X + area.Width - 8, area.Y + 44);

    // groups
    float row_y = area.Y + 50;
    for (auto* gname : kGroups) {
        // group head — 11px uppercase tracking-1 muted
        RectF ghead(area.X + 6, row_y, area.Width - 12, 22);
        bool ghov = inRect(g_mouse, ghead);
        if (ghov) {
            Color hc(g_dark ? 14 : 10, pal.text.GetR(), pal.text.GetG(), pal.text.GetB());
            fillRR(g, ghead.X, ghead.Y, ghead.Width, ghead.Height, 4.0f, hc);
        }
        bool collapsed = groupCollapsed()[gname];
        drawText_(g, collapsed ? L"▸" : L"▾", area.X + 10, row_y + 4, 12, 8.0f, pal.text_muted);
        drawText_(g, gname, area.X + 26, row_y + 4, 200,
                  8.0f, pal.text_muted, StringAlignmentNear, FontStyleBold);
        const wchar_t* gn = gname;
        hit(ghead, [gn](){ groupCollapsed()[gn] = !groupCollapsed()[gn]; }, true);
        row_y += 24;

        if (collapsed) { row_y += 6; continue; }

        // channels in group
        for (auto& c : kChannels) {
            if (wcscmp(c.group, gname) != 0) continue;
            bool active = (wcscmp(c.id, g_active) == 0);
            RectF cr(area.X + 6, row_y, area.Width - 12, 28);
            bool hov = inRect(g_mouse, cr);
            if (active) {
                Color a(36, pal.primary.GetR(), pal.primary.GetG(), pal.primary.GetB());
                fillRR(g, cr.X, cr.Y, cr.Width, cr.Height, 6.0f, a);
            } else if (hov) {
                Color h(g_dark ? 10 : 8, pal.text.GetR(), pal.text.GetG(), pal.text.GetB());
                fillRR(g, cr.X, cr.Y, cr.Width, cr.Height, 6.0f, h);
            }
            // # 前缀
            drawText_(g, L"#", cr.X + 12, cr.Y + 6, 14, 10.0f,
                      active ? pal.primary : pal.text_muted,
                      StringAlignmentNear, FontStyleBold);
            drawText_(g, c.name, cr.X + 26, cr.Y + 7, cr.Width - 60,
                      9.5f, active ? pal.text : pal.text_muted,
                      StringAlignmentNear, active ? FontStyleBold : FontStyleRegular);
            // unread badge
            if (c.unread > 0) {
                wchar_t buf[16]; swprintf_s(buf, 16, L"%d", c.unread);
                float bw = c.unread > 9 ? 22.0f : 16.0f;
                RectF br(cr.X + cr.Width - bw - 8, cr.Y + 7, bw, 14);
                fillRR(g, br.X, br.Y, br.Width, br.Height, 7.0f, pal.primary);
                drawText_(g, buf, br.X, br.Y + 2, br.Width, 7.5f,
                          Color(255, 255, 255, 255), StringAlignmentCenter, FontStyleBold);
            }
            const wchar_t* tgt = c.id;
            hit(cr, [tgt]() { switchChannel(tgt); }, true);
            row_y += 30;
        }
        row_y += 6;
    }
}

// ============== 单条气泡 ==============
// 返回这条占用的高度
float paintBubble(Graphics& g, const Msg& m, float x, float y, float maxw,
                  const Palette& pal, bool prev_same_author, int msg_index) {
    if (m.kind == MsgKind::DayDivider) {
        std::wstring s = m.body;
        float tw = measureText(g, s.c_str(), 8.0f).Width + 24;
        float bx = x + (maxw - tw) / 2;
        Color pillC(g_dark ? 12 : 10, pal.text.GetR(), pal.text.GetG(), pal.text.GetB());
        fillRR(g, bx, y + 6, tw, 18, 9.0f, pillC);
        drawText_(g, s.c_str(), bx, y + 9, tw, 7.5f, pal.text_muted, StringAlignmentCenter);
        return 30;
    }
    if (m.kind == MsgKind::System) {
        std::wstring s = m.body;
        float tw = measureText(g, s.c_str(), 8.5f).Width + 24;
        float bx = x + (maxw - tw) / 2;
        Color pillC(g_dark ? 12 : 10, pal.text.GetR(), pal.text.GetG(), pal.text.GetB());
        fillRR(g, bx, y + 4, tw, 22, 11.0f, pillC);
        drawText_(g, s.c_str(), bx, y + 8, tw, 8.5f, pal.text_muted, StringAlignmentCenter);
        return 32;
    }
    if (m.kind == MsgKind::Typing) {
        // 三点弹动
        float dot_y = y + 14;
        for (int i = 0; i < 3; ++i) {
            float phase = g_typing_t * 1.5f + i * 0.18f;
            float dy = sinf(phase * 6.28f) * 3.0f;
            if (dy < 0) dy = 0;
            Color dc((BYTE)(120 + 80 * (dy / 3.0f)), pal.text_muted.GetR(),
                     pal.text_muted.GetG(), pal.text_muted.GetB());
            SolidBrush b(dc);
            g.FillEllipse(&b, x + 18 + 28 + i * 12.0f, dot_y - dy, 6.0f, 6.0f);
        }
        Color cardC(g_dark ? 200 : 240, pal.card.GetR(), pal.card.GetG(), pal.card.GetB());
        fillRR(g, x + 18 + 18, y + 6, 60, 24, 12.0f, cardC);
        // avatar
        if (!prev_same_author) drawAvatar(g, x + 4, y + 4, 14, m.from, m.status, pal);
        return 38;
    }

    bool me = (wcscmp(m.from, L"me") == 0);
    bool isImage = (m.kind == MsgKind::Image);
    bool isGif   = (m.kind == MsgKind::Gif);
    bool isVideo = (m.kind == MsgKind::Video);

    // ---------- Image / GIF / Video 媒体气泡 ----------
    if (isImage || isGif || isVideo) {
        const Media* mm = loadMedia(m.body ? m.body : L"");
        const float bub_max_w = std::min(maxw * 0.55f, 320.0f);
        float bub_w = 240.0f, bub_h = 180.0f;
        if (mm && mm->img && mm->width > 0 && mm->height > 0) {
            float aspect = (float)mm->height / (float)mm->width;
            bub_w = std::min(bub_max_w, (float)mm->width);
            bub_h = bub_w * aspect;
            if (bub_h > 240.0f) { bub_h = 240.0f; bub_w = bub_h / aspect; }
        } else if (isVideo) {
            bub_w = 260.0f; bub_h = 160.0f;
        }
        const float gutter_m = 38.0f;
        float bub_x = me ? (x + maxw - 14.0f - bub_w) : (x + gutter_m);

        // 头像（非自己 + 非连续）
        if (!me && !prev_same_author) {
            drawAvatar(g, x, y + bub_h - 28.0f, 14.0f, m.author, m.status, palette());
        }

        // 圆角裁剪 + 画图 / 视频封面
        GraphicsPath cp; buildRoundRect(cp, bub_x, y, bub_w, bub_h, 12.0f);
        g.SetClip(&cp);
        if (mm && mm->img) {
            g.DrawImage(mm->img, RectF(bub_x, y, bub_w, bub_h));
        } else {
            // video 没缩略 / 图片解码失败 — 纯色占位
            SolidBrush bg(Color(255, 0x28, 0x24, 0x20));
            g.FillRectangle(&bg, bub_x, y, bub_w, bub_h);
        }
        // 视频底部渐变蒙版让 ▶ 可读
        if (isVideo) {
            LinearGradientBrush vmask(PointF(bub_x, y + bub_h * 0.5f), PointF(bub_x, y + bub_h),
                                      Color(0, 0, 0, 0), Color(160, 0, 0, 0));
            g.FillRectangle(&vmask, bub_x, y + bub_h * 0.5f, bub_w, bub_h * 0.5f);
        }
        g.ResetClip();

        // 视频中央播放按钮 + 文件名
        if (isVideo) {
            float btnr = 24.0f;
            float btnx = bub_x + bub_w / 2 - btnr;
            float btny = y + bub_h / 2 - btnr;
            SolidBrush pbg(Color(190, 0, 0, 0));
            g.FillEllipse(&pbg, btnx, btny, btnr * 2, btnr * 2);
            icons::drawSvg(g, icons::Name::Play, btnx + 12.0f, btny + 12.0f, 24.0f,
                           Color(255, 255, 255, 255));
            // 文件名（底部 padding）
            std::wstring fn = basename(m.body ? m.body : L"");
            drawText_(g, fn.c_str(), bub_x + 10.0f, y + bub_h - 22.0f, bub_w - 20.0f,
                      8.0f, Color(255, 255, 255, 255), StringAlignmentNear, FontStyleBold);
            // 整个气泡点击 → ShellExecute 默认播放器
            std::wstring path_copy = m.body ? m.body : L"";
            hit(RectF(bub_x, y, bub_w, bub_h), [path_copy](){
                if (!path_copy.empty()) {
                    ShellExecuteW(nullptr, L"open", path_copy.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                }
            }, true);
        } else if (isImage || isGif) {
            // 图片点击 → 用默认查看器打开（暂不做内嵌大图）
            std::wstring path_copy = m.body ? m.body : L"";
            hit(RectF(bub_x, y, bub_w, bub_h), [path_copy](){
                if (!path_copy.empty()) {
                    ShellExecuteW(nullptr, L"open", path_copy.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                }
            }, true);
        }
        // time + 双勾
        const Palette& palc = palette();
        Color metaC(220, 255, 255, 255);
        drawText_(g, m.time, bub_x, y + bub_h - 14.0f, bub_w - 10.0f,
                  7.0f, metaC, StringAlignmentFar);
        if (me) {
            Color tickC = m.read ? Color(255, 0x7D, 0xD3, 0xFC) : metaC;
            icons::drawSvg(g, icons::Name::Check2,
                           bub_x + bub_w - 24.0f, y + bub_h - 16.0f, 12.0f, tickC);
        }
        (void)palc;
        return bub_h + 10.0f;
    }

    // Telegram 风：紧凑 padding + meta 默认 inline（同行最右），文字塞不下才换行
    const float pad_l = 10.0f, pad_r = 10.0f;
    const float pad_t = 6.0f,  pad_b = 6.0f;
    const float content_w_max = maxw * 0.62f;
    std::wstring body_w = m.body ? m.body : L"";

    Font body_font(kFontFace, 9.0f, FontStyleRegular, UnitPoint);
    StringFormat body_fmt;
    body_fmt.SetAlignment(StringAlignmentNear);

    RectF unbounded(0, 0, 4096.0f, 4096.0f);
    RectF measured;
    g.MeasureString(body_w.c_str(), -1, &body_font, unbounded, &body_fmt, &measured);
    float text_one_line_w = measured.Width;

    bool has_reply = m.reply_excerpt && m.reply_excerpt[0];
    bool show_author = !me && m.author && m.author[0] && !prev_same_author;

    // meta = 时间 + 可选双勾。Telegram 把时间紧贴文字尾巴（同行）。
    Font meta_font(kFontFace, 7.0f, FontStyleRegular, UnitPoint);
    RectF meta_box;
    g.MeasureString(m.time ? m.time : L"", -1, &meta_font, unbounded, &body_fmt, &meta_box);
    const float tick_w = me ? 14.0f : 0.0f;
    float meta_w = meta_box.Width + tick_w + 4.0f;   // 4 = 文字到 meta 的 gap

    // 单行能塞下：文字 + meta 同行
    bool meta_inline = (text_one_line_w + meta_w + pad_l + pad_r) <= content_w_max;
    float bubble_w;
    if (meta_inline) {
        bubble_w = text_one_line_w + meta_w + pad_l + pad_r;
    } else {
        // 多行：bubble 拉到内容上限，meta 单独一行右下
        bubble_w = std::min(text_one_line_w + pad_l + pad_r, content_w_max);
        bubble_w = std::max(bubble_w, meta_w + pad_l + pad_r);
    }

    // 第二遍：测真实换行后高度
    RectF inner_layout(0, 0, bubble_w - pad_l - pad_r, 4096.0f);
    g.MeasureString(body_w.c_str(), -1, &body_font, inner_layout, &body_fmt, &measured);
    float text_h = measured.Height;

    float bubble_h = pad_t + (show_author ? 13.0f : 0.0f)
                          + (has_reply ? 22.0f : 0.0f)
                          + text_h
                          + (meta_inline ? 0.0f : 12.0f)
                          + pad_b;

    const float gutter = 36.0f;
    float bubble_x;
    if (me) {
        bubble_x = x + maxw - pad_r - bubble_w;
    } else {
        bubble_x = x + gutter;
    }

    // 头像（仅非自己 + 非连续）— 缩到 12 半径 (24x24)
    if (!me && !prev_same_author) {
        drawAvatar(g, x, y + bubble_h - 24.0f, 12.0f, m.author, m.status, pal);
        int idx = msg_index;
        hit(RectF(x, y + bubble_h - 24.0f, 24.0f, 24.0f), [idx](){
            auto& s = chatv::streamFor(g_active);
            if (idx >= 0 && idx < (int)s.size() && s[idx].author && s[idx].author[0]) {
                std::wstring at = std::wstring(L"@") + s[idx].author + L" ";
                g_composer.replaceSelection(at);
                g_focus_composer = true;
            }
        }, true);
    }

    // 气泡背景 — 12 圆角，连续作者去掉一角让消息成"柱"
    Color cardC = me ? pal.primary : pal.card;
    fillRR(g, bubble_x, y, bubble_w, bubble_h, 12.0f, cardC);

    float ty = y + pad_t;

    if (has_reply) {
        Color repBar = me ? Color(255, 255, 255, 255)
                          : Color(255, pal.primary.GetR(), pal.primary.GetG(), pal.primary.GetB());
        fillRR(g, bubble_x + pad_l, ty + 1.0f, 2.5f, 16.0f, 1.5f, repBar);
        Color repNameC = me ? Color(255, 255, 255, 255) : pal.primary;
        Color repTextC = me ? Color(220, 255, 255, 255) : pal.text_muted;
        drawText_(g, m.reply_author, bubble_x + pad_l + 7.0f, ty, bubble_w - pad_l - pad_r - 7.0f,
                  7.0f, repNameC, StringAlignmentNear, FontStyleBold);
        drawText_(g, m.reply_excerpt, bubble_x + pad_l + 7.0f, ty + 9.0f,
                  bubble_w - pad_l - pad_r - 7.0f, 7.0f, repTextC, StringAlignmentNear);
        ty += 22.0f;
    }
    if (show_author) {
        drawText_(g, m.author, bubble_x + pad_l, ty, bubble_w - pad_l - pad_r,
                  7.5f, pal.primary, StringAlignmentNear, FontStyleBold);
        ty += 13.0f;
    }

    // 正文 — emoji + 文字混排（emoji 走 Segoe UI Emoji 防"找不到字符"方框）
    Color textC = me ? Color(255, 255, 255, 255) : pal.text;
    drawWrappedTextWithEmoji(g, body_w.c_str(), (int)body_w.size(),
                             bubble_x + pad_l, ty,
                             bubble_w - pad_l - pad_r, 9.0f, textC);

    // meta — inline 时在文字尾巴右侧；否则单独一行右下
    Color metaC = me ? Color(210, 255, 255, 255) : pal.text_muted;
    float meta_y;
    if (meta_inline) {
        meta_y = ty + (text_h - meta_box.Height) * 0.5f + 1.0f;   // 跟文字基线对齐
    } else {
        meta_y = y + bubble_h - 12.0f;
    }
    drawText_(g, m.time, bubble_x, meta_y, bubble_w - pad_r - tick_w,
              7.0f, metaC, StringAlignmentFar);
    if (me) {
        Color tickC = m.read ? Color(255, 0x7D, 0xD3, 0xFC) : metaC;
        icons::drawSvg(g, icons::Name::Check2,
                       bubble_x + bubble_w - pad_r - 12.0f,
                       meta_y - 1.0f, 11.0f, tickC);
    }

    return bubble_h + 4.0f;
}

// (旧版 sticker/gif/link/video 分支已移除 — sample 数据清空后用不到。
//  后端真正接通后按 message_type 重新加。)
#if 0
static float paintBubble_legacy_unused(Graphics& g, const Msg& m, float x, float y, float maxw,
                                       const Palette& pal, bool prev_same_author, int msg_index) {
    bool me = false; (void)g; (void)m; (void)x; (void)y; (void)maxw; (void)pal; (void)prev_same_author; (void)msg_index;
    if (false) {
        Color cardC = pal.card;
        if (false) {
            Color tickC = m.read ? Color(255, 0x7D, 0xD3, 0xFC) : metaC;
            icons::drawSvg(g, icons::Name::Check2,
                           bubble_x + bubble_w - 22, y + bubble_h - 16, 14, tickC);
        }
    }

    return 0.0f;
}
#endif

// ============== Composer ==============
// 只保留 emoji + textarea + send 三件套；左下三个杂图标全部删掉。
void paintComposer(Graphics& g, RectF area) {
    const Palette& pal = palette();
    SolidBrush bg(pal.bg);
    g.FillRectangle(&bg, area.X, area.Y, area.Width, area.Height);
    Pen sep(pal.divider, 1.0f);
    g.DrawLine(&sep, area.X, area.Y, area.X + area.Width, area.Y);

    // emoji 圆角图标按钮
    const float ico_sz = 30.0f;
    float ix = area.X + 14.0f;
    float iy = area.Y + (area.Height - ico_sz) / 2.0f;
    bool ehov = inRect(g_mouse, RectF(ix, iy, ico_sz, ico_sz));
    if (ehov) fillRR(g, ix, iy, ico_sz, ico_sz, 8.0f, pal.card);
    icons::drawSvg(g, icons::Name::Smile, ix + 6.0f, iy + 6.0f, 18.0f,
                   ehov ? pal.text : pal.text_muted);
    hit(RectF(ix, iy, ico_sz, ico_sz), [](){
        g_picker_open = !g_picker_open;
        g_picker_tab = 0;
        if (g_picker_open) g_picker_t.enter(0.94f, 0.22f);
        else               g_picker_t.exit(0.94f, 0.18f);
    }, true);

    // textarea — 居中精确，placeholder 与文字垂直对齐
    float fx = ix + ico_sz + 10.0f;
    float send_w = 38.0f;
    float fw = area.Width - (fx - area.X) - 14.0f - send_w - 10.0f;
    float fh = ico_sz;
    float fy = iy;
    fillRR(g, fx, fy, fw, fh, fh / 2.0f, pal.card);
    strokeRR(g, fx, fy, fw, fh, fh / 2.0f,
             g_focus_composer ? pal.primary : pal.divider, g_focus_composer ? 1.4f : 1.0f);
    if (g_focus_composer) {
        Color halo(22, pal.primary.GetR(), pal.primary.GetG(), pal.primary.GetB());
        strokeRR(g, fx - 2.0f, fy - 2.0f, fw + 4.0f, fh + 4.0f, fh / 2.0f + 2.0f, halo, 3.0f);
    }
    // 文字 / placeholder：垂直居中（fy + (fh - line_h)/2，line_h 约 14px @ 9.5pt）
    const float pad_l = 16.0f;
    const float text_y = fy + (fh - 14.0f) / 2.0f;
    g_composer.bounds = RectF(fx, fy, fw, fh);
    if (g_composer.text.empty()) {
        drawText_(g, L"写点什么…", fx + pad_l, text_y, fw - pad_l * 2.0f,
                  9.5f, pal.text_muted);
    } else {
        // 选区高亮
        Font* f = fontcache::get(9.5f);
        if (g_focus_composer && g_composer.hasSelection()) {
            RectF bb_pre, bb_in;
            g.MeasureString(g_composer.displaySlice(0, g_composer.selStart()).c_str(), -1, f,
                            PointF(0, 0), &bb_pre);
            g.MeasureString(g_composer.displaySlice(g_composer.selStart(), g_composer.selEnd()).c_str(), -1, f,
                            PointF(0, 0), &bb_in);
            Color sel_bg(96, pal.primary.GetR(), pal.primary.GetG(), pal.primary.GetB());
            SolidBrush sel_b(sel_bg);
            g.FillRectangle(&sel_b, fx + pad_l + bb_pre.Width, text_y - 1.0f,
                            bb_in.Width, 16.0f);
        }
        // emoji + 文字混排：emoji codepoint 走 Segoe UI Emoji，否则 YaHei
        // (caret 测量仍用 YaHei 单字体，emoji 字符的 cursor 位置可能略偏，可接受)
        drawTextWithEmoji(g, g_composer.text.c_str(), (int)g_composer.text.size(),
                          fx + pad_l, text_y, 9.5f, pal.text);
    }
    // caret blink
    if (g_focus_composer && !g_composer.hasSelection()) {
        Font* fnt = fontcache::get(9.5f);
        std::wstring sub = g_composer.displaySlice(0, g_composer.cursor);
        RectF bb; g.MeasureString(sub.c_str(), -1, fnt, PointF(0, 0), &bb);
        int phase = (int)(g_time_in_stage * 1000) % 1000;
        if (phase < 500) {
            Pen p(pal.primary, 1.5f);
            float cx_ = fx + pad_l + bb.Width;
            g.DrawLine(&p, cx_, fy + 7.0f, cx_, fy + fh - 7.0f);
        }
    }
    hit(RectF(fx, fy, fw, fh), [](){ g_focus_composer = true; }, true);

    // send btn 圆形主色
    float sx = area.X + area.Width - 14.0f - send_w;
    float sy = iy + (fh - send_w) / 2.0f;
    bool can_send = !g_composer.text.empty();
    bool sh = inRect(g_mouse, RectF(sx, sy, send_w, send_w));
    Color sbg = !can_send ? Color(140, pal.primary.GetR(), pal.primary.GetG(), pal.primary.GetB())
                          : (sh ? pal.primary_hover : pal.primary);
    SolidBrush sbgB(sbg);
    g.FillEllipse(&sbgB, sx, sy, send_w, send_w);
    icons::drawSvg(g, icons::Name::Send, sx + 10.0f, sy + 10.0f, 18.0f,
                   Color(255, 255, 255, 255));
    if (can_send) {
        hit(RectF(sx, sy, send_w, send_w), [](){
            // 1. 本地立即显示（乐观更新）
            auto& s = streamFor(g_active);
            Msg m; m.kind = MsgKind::Text; m.from = L"me"; m.author = L"";
            m.status = L"online"; m.read = false; m.time = L"now";
            // deque 不像 vector 那样 push_back 重分配 → 已存的 c_str() 不会失效
            // （短 wstring SSO 内嵌在对象，vector 重分配 = 字符串变垃圾）
            static std::deque<std::wstring> g_my_msgs;
            g_my_msgs.push_back(g_composer.text);
            m.body = g_my_msgs.back().c_str();
            s.push_back(m);
            // 2. 真发后端（如果有 session）
            sendTextMessage(g_hwnd, g_composer.text);
            g_composer.text.clear();
            g_composer.cursor = 0;
            g_composer.clearSel();
            g_focus_composer = true;
        }, true);
    }
}

// ============== Picker ==============
const wchar_t* kEmoji[] = {
    L"😀",L"😁",L"😂",L"🤣",L"😄",L"😅",L"😉",L"😊",
    L"😎",L"😍",L"🥰",L"🙃",L"🙂",L"🤩",L"🤔",L"😐",
    L"😴",L"😌",L"😜",L"🤪",L"🥳",L"🥺",L"😢",L"😭",
    L"💀",L"👻",L"🤖",L"👍",L"👎",L"👏",L"🙏",L"💪",
    L"🔥",L"💯",L"🎮",L"🍣",L"🌸",L"⭐",L"🚀",L"💖",
};

// ========== 表情包分组 (Pack) ==========
// 设计：每个 user 可创建多个表情包分组（pack），每个分组最多 25 张。
// 分组 0 是固定的 "系统 emoji"（不可删/不可重命名）。其余分组对应 backend 的
// sticker_packs 行，id = pack_id (uuid)，可云端同步 + 分享 + 重命名 + 删除。
struct Pack {
    std::string  id;             // backend uuid (空表示本地占位 / 系统)
    std::wstring name;
    std::vector<std::wstring> stickers;   // 本地路径（已 loadMedia）
    std::vector<std::string>  sticker_ids;// 后端 uuid 同步用
    bool is_system{false};       // 系统 emoji（不可改/删）
    bool is_public{false};       // 已分享
};
constexpr int kPackMaxStickers = 25;

inline std::vector<Pack>& packs() {
    static std::vector<Pack> v = []{
        Pack sys; sys.is_system = true; sys.name = L"系统 emoji";
        return std::vector<Pack>{ sys };
    }();
    return v;
}
inline int& activePack() { static int idx = 0; return idx; }

// 兼容老 API：userPack() 返回当前活跃 pack 的 stickers（系统 emoji 不算）
// 或全部用户上传的 stickers — 旧调用方主要用于 picker grid 渲染。
inline std::vector<std::wstring>& userPack() {
    auto& ps = packs();
    int idx = activePack();
    if (idx > 0 && idx < (int)ps.size()) return ps[idx].stickers;
    // 没用户 pack 时返回一个静态空 vector（避免 crash）
    static std::vector<std::wstring> empty;
    return empty;
}

// 找/建一个用户 pack — 用于 fetchMyStickers 的兜底（若用户还没建 pack 就把
// 历史 sticker 都丢进默认 pack "我的表情"）
inline Pack& ensureDefaultUserPack() {
    auto& ps = packs();
    for (size_t i = 1; i < ps.size(); ++i) if (ps[i].name == L"我的表情") return ps[i];
    Pack p; p.name = L"我的表情"; p.id = "";
    ps.push_back(std::move(p));
    return ps.back();
}

// 本地 sticker 缓存目录: %LOCALAPPDATA%/Launcher/stickers/
inline std::wstring stickerCacheDir() {
    wchar_t base[MAX_PATH] = {0};
    if (!SHGetSpecialFolderPathW(nullptr, base, CSIDL_LOCAL_APPDATA, FALSE)) return L"";
    std::wstring dir = std::wstring(base) + L"\\Launcher\\stickers\\";
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    return dir;
}

// 启动后异步拉取自己上传过的所有 stickers (GET /api/sticker/mine)，
// 把每张图下载到本地缓存目录加进 userPack — 同账号在另一台机也能看到。
inline void fetchMyStickers(HWND notify_hwnd) {
    if (::g_session_token.empty()) return;
    struct A { HWND h; };
    A* a = new A{notify_hwnd};
    CreateThread(nullptr, 0, [](LPVOID lp) -> DWORD {
        auto* a = (A*)lp;
        std::string url = "/api/sticker/mine?session_token=" + ::g_session_token;
        std::wstring wurl(url.begin(), url.end());
        auto r = net::request(L"GET", wurl.c_str(), "", L"");
        if (!r.ok()) { delete a; return 0; }

        // 简易解析 [{"id":"...","media_url":"/api/media/<sha>/file.<ext>", ...}, ...]
        // 只关心 media_url 字段，每条一个
        std::wstring dir = stickerCacheDir();
        if (dir.empty()) { delete a; return 0; }

        size_t pos = 0;
        int added = 0;
        while (true) {
            pos = r.body.find("\"media_url\":\"", pos);
            if (pos == std::string::npos) break;
            pos += 13;
            size_t e = r.body.find('"', pos);
            if (e == std::string::npos) break;
            std::string url_path = r.body.substr(pos, e - pos);
            pos = e;

            // 从 url_path 取 sha 和 ext: /api/media/<sha>/file.<ext>
            auto p1 = url_path.find("/api/media/");
            if (p1 == std::string::npos) continue;
            p1 += 11;
            auto p2 = url_path.find('/', p1);
            if (p2 == std::string::npos) continue;
            std::string sha = url_path.substr(p1, p2 - p1);
            auto dot = url_path.find_last_of('.');
            std::string ext = (dot != std::string::npos) ? url_path.substr(dot + 1) : "bin";

            // 本地路径 cache/<sha>.<ext>
            std::wstring fname = std::wstring(sha.begin(), sha.end())
                + L"." + std::wstring(ext.begin(), ext.end());
            std::wstring local = dir + fname;

            // 已存在就直接用
            if (GetFileAttributesW(local.c_str()) == INVALID_FILE_ATTRIBUTES) {
                std::wstring wpath(url_path.begin(), url_path.end());
                auto dr = net::request(L"GET", wpath.c_str(), "", L"");
                if (!dr.ok()) continue;
                HANDLE f = CreateFileW(local.c_str(), GENERIC_WRITE, 0,
                                       nullptr, CREATE_ALWAYS, 0, nullptr);
                if (f == INVALID_HANDLE_VALUE) continue;
                DWORD wn = 0;
                WriteFile(f, dr.body.data(), (DWORD)dr.body.size(), &wn, nullptr);
                CloseHandle(f);
            }

            // 去重 + 加入默认 user pack "我的表情"
            // （UI 线程在 paintPicker 同时迭代；启动期碰撞窗口短，但仍用 mutex 防 race）
            auto& dp = ensureDefaultUserPack();
            bool dup = false;
            for (auto& p : dp.stickers) if (p == local) { dup = true; break; }
            if (dup) continue;
            if (loadMedia(local)) {
                dp.stickers.push_back(local);
                ++added;
            }
        }
        if (added > 0) PostMessageW(a->h, WM_APP + 14, added, 0);
        delete a;
        return 0;
    }, a, 0, nullptr);
}

// 异步上传一个表情文件 → /api/media/upload → /api/sticker
// session_token 空时跳过（只本地缓存）
inline void uploadStickerAsync(const std::wstring& path) {
    if (::g_session_token.empty()) return;
    struct A { std::wstring path; HWND h; };
    A* a = new A{path, g_hwnd};
    CreateThread(nullptr, 0, [](LPVOID lp) -> DWORD {
        auto* a = (A*)lp;
        HANDLE f = CreateFileW(a->path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                               nullptr, OPEN_EXISTING, 0, nullptr);
        if (f == INVALID_HANDLE_VALUE) { delete a; return 0; }
        DWORD sz = GetFileSize(f, nullptr);
        std::vector<BYTE> bytes(sz);
        DWORD rd = 0;
        ReadFile(f, bytes.data(), sz, &rd, nullptr);
        CloseHandle(f);

        // mime + 文件名
        std::string mime = "application/octet-stream";
        bool is_animated = false;
        auto dot = a->path.find_last_of(L'.');
        std::wstring fn = a->path;
        auto sl = a->path.find_last_of(L"\\/");
        if (sl != std::wstring::npos) fn = a->path.substr(sl + 1);
        if (dot != std::wstring::npos) {
            std::wstring ext = a->path.substr(dot);
            for (auto& c : ext) c = (wchar_t)towlower(c);
            if (ext == L".png") mime = "image/png";
            else if (ext == L".jpg" || ext == L".jpeg") mime = "image/jpeg";
            else if (ext == L".gif") { mime = "image/gif"; is_animated = true; }
            else if (ext == L".webp") mime = "image/webp";
        }
        // 1. /api/media/upload
        auto mr = net::uploadMultipart(L"/api/media/upload", ::g_session_token,
                                        L"file", fn, mime, bytes);
        if (!mr.ok()) { delete a; return 0; }
        long long media_id = net::jsonInt(mr.body, "media_id");
        if (media_id <= 0) { delete a; return 0; }

        // 2. /api/sticker create
        std::string sbody = std::string("{\"session_token\":\"") + ::g_session_token
            + "\",\"media_id\":" + std::to_string(media_id)
            + ",\"label\":\"" + net::jsonEscape(fn)
            + "\",\"is_animated\":" + (is_animated ? "true" : "false") + "}";
        auto sr = net::postJson(L"/api/sticker", sbody);
        // 失败 toast 在主线程统一处理；成功也不弹 — 默默同步
        PostMessageW(a->h, WM_APP + 13, sr.ok() ? 1 : 0, (LPARAM)(intptr_t)sr.status);
        delete a;
        return 0;
    }, a, 0, nullptr);
}

// 把单个文件加进当前激活的 user pack（不超过 25/pack）
inline bool addStickerToActivePack(const std::wstring& full) {
    auto& ps = packs();
    int idx = activePack();
    if (idx <= 0 || idx >= (int)ps.size()) return false;   // 系统 emoji 或越界
    Pack& p = ps[idx];
    if ((int)p.stickers.size() >= kPackMaxStickers) return false;
    for (auto& s : p.stickers) if (s == full) return false;   // 去重
    if (!loadMedia(full)) return false;
    p.stickers.push_back(full);
    uploadStickerAsync(full);
    return true;
}

// 选文件夹：扫描里面的图片/GIF/WebP 加进当前 pack（≤ 25/pack）
void importEmojiFolder() {
    auto& ps = packs();
    if (activePack() <= 0 || activePack() >= (int)ps.size()) {
        ::g_toast.show(L"先创建一个表情包分组，再导入");
        return;
    }
    BROWSEINFOW bi{};
    bi.hwndOwner = g_hwnd;
    bi.lpszTitle = L"选择文件夹（扫描 .gif / .png / .jpg / .webp）";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_USENEWUI;
    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return;
    wchar_t folder[MAX_PATH];
    if (!SHGetPathFromIDListW(pidl, folder)) { CoTaskMemFree(pidl); return; }
    CoTaskMemFree(pidl);

    std::wstring pat = std::wstring(folder) + L"\\*";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    int added = 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::wstring name = fd.cFileName;
        auto dot = name.find_last_of(L'.');
        if (dot == std::wstring::npos) continue;
        std::wstring ext = name.substr(dot);
        for (auto& c : ext) c = (wchar_t)towlower(c);
        if (ext != L".gif" && ext != L".png" && ext != L".jpg" && ext != L".jpeg"
            && ext != L".webp" && ext != L".bmp") continue;
        std::wstring full = std::wstring(folder) + L"\\" + name;
        if (addStickerToActivePack(full)) ++added;
        if ((int)ps[activePack()].stickers.size() >= kPackMaxStickers) break;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    wchar_t msg[128];
    swprintf_s(msg, 128, L"已导入 %d 张到 %ls（%zu/%d）",
               added, ps[activePack()].name.c_str(),
               ps[activePack()].stickers.size(), kPackMaxStickers);
    ::g_toast.show(msg);
}

// 选单个图片 / GIF：GetOpenFileNameW
void importEmojiSingle() {
    auto& ps = packs();
    if (activePack() <= 0 || activePack() >= (int)ps.size()) {
        ::g_toast.show(L"先创建一个表情包分组，再导入");
        return;
    }
    if ((int)ps[activePack()].stickers.size() >= kPackMaxStickers) {
        ::g_toast.show(L"已达 25 张上限");
        return;
    }
    OPENFILENAMEW ofn{};
    wchar_t buf[MAX_PATH * 4]; buf[0] = 0;
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hwnd;
    ofn.lpstrFilter = L"图片/GIF\0*.png;*.jpg;*.jpeg;*.gif;*.webp;*.bmp\0全部\0*.*\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = MAX_PATH * 4;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_ALLOWMULTISELECT | OFN_EXPLORER;
    if (!GetOpenFileNameW(&ofn)) return;
    int added = 0;
    // multi-select: buf 里第一段是目录，后面是文件名(用 \0 分隔)；单选时整段就是完整路径
    std::wstring dir = buf;
    size_t pos = wcslen(buf) + 1;
    bool multi = (buf[pos] != 0);
    if (!multi) {
        if (addStickerToActivePack(dir)) ++added;
    } else {
        while (buf[pos] != 0) {
            std::wstring full = dir + L"\\" + (buf + pos);
            if (addStickerToActivePack(full)) ++added;
            if ((int)ps[activePack()].stickers.size() >= kPackMaxStickers) break;
            pos += wcslen(buf + pos) + 1;
        }
    }
    wchar_t msg[128];
    swprintf_s(msg, 128, L"已导入 %d 张到 %ls（%zu/%d）",
               added, ps[activePack()].name.c_str(),
               ps[activePack()].stickers.size(), kPackMaxStickers);
    ::g_toast.show(msg);
}

// 当前 pack 头部 kebab 菜单（点 ⋯ 弹）— 简化版，直接列三个选项
struct PackMenu { bool open{false}; int pack_idx{-1}; float ax{0}, ay{0}; };
inline PackMenu& packMenu() { static PackMenu m; return m; }

void paintPicker(Graphics& g, float anchor_x, float anchor_y) {
    if (g_picker_t.value() < 0.001f && !g_picker_open) return;
    const Palette& pal = palette();
    float t = g_picker_t.value();
    float pw = 420, ph = 400;            // 宽 +80，给右侧 tab strip 留位
    float px = anchor_x;
    float py = anchor_y - ph - 8;
    BYTE a = (BYTE)(255 * t);
    if (a == 0) return;

    auto fade = [&](Color c) { return Color((BYTE)(c.GetA() * t), c.GetR(), c.GetG(), c.GetB()); };
    Color cardC(a, pal.card.GetR(), pal.card.GetG(), pal.card.GetB());
    drawShadow(g, px, py, pw, ph, 12, fade(pal.shadow_card), 4, 2);
    fillRR(g, px, py, pw, ph, 12, cardC);
    strokeRR(g, px, py, pw, ph, 12, fade(pal.divider));

    // 布局：左 content 350，右 tab strip 70（含 8 内边距）
    float strip_w = 62.0f;
    float strip_x = px + pw - strip_w - 6;
    float content_x = px + 8;
    float content_w = strip_x - content_x - 6;

    // ---------------- 左 content ----------------
    auto& ps = packs();
    if (activePack() < 0 || activePack() >= (int)ps.size()) activePack() = 0;
    Pack& cur = ps[activePack()];

    // header: pack name + 操作按钮
    drawText_(g, cur.name.c_str(), content_x + 6, py + 12, content_w - 60, 11.0f,
              fade(pal.text), StringAlignmentNear, FontStyleBold);

    // 用户 pack 才有 "+ 添加" / kebab 菜单 按钮
    if (!cur.is_system) {
        // kebab "⋯" — 右侧倒数第 1 个
        float bx = content_x + content_w - 26;
        RectF mb(bx, py + 10, 24, 22);
        bool mh = inRect(g_mouse, mb);
        if (mh) fillRR(g, mb.X, mb.Y, mb.Width, mb.Height, 5, fade(pal.bg));
        drawText_(g, L"⋯", mb.X, mb.Y + 2, mb.Width, 12.0f, fade(pal.text_muted),
                  StringAlignmentCenter, FontStyleBold);
        int idx_capt = activePack();
        hit(mb, [idx_capt, mb](){
            auto& m = packMenu();
            m.open = !m.open || m.pack_idx != idx_capt;
            m.pack_idx = idx_capt;
            m.ax = mb.X; m.ay = mb.Y + 26;
        }, true);

        // "+图片" — 倒数第 2
        float bx2 = content_x + content_w - 26 - 56;
        RectF abi(bx2, py + 10, 50, 22);
        bool ahi = inRect(g_mouse, abi);
        fillRR(g, abi.X, abi.Y, abi.Width, abi.Height, 5,
               fade(ahi ? pal.primary_hover : pal.primary));
        drawText_(g, L"+ 图片", abi.X, abi.Y + 4, abi.Width, 8.0f,
                  Color((BYTE)(255 * t), 255, 255, 255),
                  StringAlignmentCenter, FontStyleBold);
        hit(abi, [](){ importEmojiSingle(); }, true);

        // "+文件夹" — 倒数第 3
        float bx3 = bx2 - 60;
        RectF abf(bx3, py + 10, 54, 22);
        bool ahf = inRect(g_mouse, abf);
        fillRR(g, abf.X, abf.Y, abf.Width, abf.Height, 5,
               fade(ahf ? pal.primary_hover : pal.primary));
        drawText_(g, L"+ 文件夹", abf.X, abf.Y + 4, abf.Width, 8.0f,
                  Color((BYTE)(255 * t), 255, 255, 255),
                  StringAlignmentCenter, FontStyleBold);
        hit(abf, [](){ importEmojiFolder(); }, true);
    }

    // 分隔
    Pen sep(fade(pal.divider), 1.0f);
    g.DrawLine(&sep, content_x, py + 40, content_x + content_w, py + 40);

    // body grid
    float gy = py + 50;
    if (cur.is_system) {
        // 系统 emoji 8 列
        int n = (int)(sizeof(kEmoji) / sizeof(kEmoji[0]));
        const float cell = (content_w - 4) / 8;
        Font ef(L"Segoe UI Emoji", cell * 0.55f, FontStyleRegular, UnitPixel);
        SolidBrush eb(fade(pal.text));
        StringFormat efmt; efmt.SetAlignment(StringAlignmentCenter); efmt.SetLineAlignment(StringAlignmentCenter);
        int max_rows = (int)((py + ph - gy - 12) / cell);
        int max_n = std::max(0, max_rows * 8);
        n = std::min(n, max_n);
        for (int i = 0; i < n; ++i) {
            int row = i / 8, col = i % 8;
            float cx = content_x + 2 + col * cell;
            float cy = gy + row * cell;
            bool hov = inRect(g_mouse, RectF(cx, cy, cell, cell));
            if (hov) fillRR(g, cx, cy, cell, cell, 6, fade(pal.bg));
            g.DrawString(kEmoji[i], -1, &ef, RectF(cx, cy, cell, cell), &efmt, &eb);
            const wchar_t* val = kEmoji[i];
            hit(RectF(cx, cy, cell, cell), [val]() {
                g_composer.replaceSelection(val);
                g_focus_composer = true;
            }, true);
        }
    } else {
        // 用户 pack：5 列图片 grid + 计数
        wchar_t cap[40]; swprintf_s(cap, 40, L"%zu / %d", cur.stickers.size(), kPackMaxStickers);
        drawText_(g, cap, content_x + content_w - 70, py + 44, 64, 7.5f,
                  fade(pal.text_muted), StringAlignmentFar);
        if (cur.stickers.empty()) {
            drawText_(g, L"点上方『+ 图片』或『+ 文件夹』添加（每组 ≤ 25）",
                      content_x + 6, py + 76, content_w - 12, 8.5f, fade(pal.text_muted));
        } else {
            const float cell = (content_w - 4) / 5;
            int n = std::min((int)cur.stickers.size(), 25);
            for (int i = 0; i < n; ++i) {
                int row = i / 5, col = i % 5;
                float cx = content_x + 2 + col * (cell + 2);
                float cy = gy + 8 + row * (cell + 2);
                bool hov = inRect(g_mouse, RectF(cx, cy, cell, cell));
                if (hov) fillRR(g, cx, cy, cell, cell, 6, fade(pal.bg));
                const Media* m = loadMedia(cur.stickers[i]);
                if (m && m->img) {
                    GraphicsPath cp; buildRoundRect(cp, cx + 2, cy + 2, cell - 4, cell - 4, 6);
                    g.SetClip(&cp);
                    g.DrawImage(m->img, RectF(cx + 2, cy + 2, cell - 4, cell - 4));
                    g.ResetClip();
                }
                std::wstring path = cur.stickers[i];
                hit(RectF(cx, cy, cell, cell), [path]() {
                    appendMedia(path);
                    g_picker_open = false;
                    g_picker_t.exit(0.94f, 0.18f);
                }, true);
            }
        }
    }

    // ---------------- 右 tab strip ----------------
    float ty = py + 12;
    const float cell = 50.0f;
    for (int i = 0; i < (int)ps.size(); ++i) {
        bool active = (i == activePack());
        RectF tr(strip_x + 2, ty, cell, cell);
        bool hov = inRect(g_mouse, tr);
        Color tbg = active ? fade(Color((BYTE)(48), pal.primary.GetR(), pal.primary.GetG(), pal.primary.GetB()))
                           : (hov ? fade(pal.bg) : fade(Color(0, 0, 0, 0)));
        fillRR(g, tr.X, tr.Y, tr.Width, tr.Height, 8, tbg);
        if (active) {
            strokeRR(g, tr.X, tr.Y, tr.Width, tr.Height, 8, fade(pal.primary), 1.4f);
        }
        if (ps[i].is_system) {
            Font ef(L"Segoe UI Emoji", 22.0f, FontStyleRegular, UnitPixel);
            SolidBrush eb(fade(pal.text));
            StringFormat efmt; efmt.SetAlignment(StringAlignmentCenter); efmt.SetLineAlignment(StringAlignmentCenter);
            g.DrawString(L"😀", -1, &ef, tr, &efmt, &eb);
        } else if (!ps[i].stickers.empty()) {
            // 用第一张作为 thumbnail
            const Media* m = loadMedia(ps[i].stickers.front());
            if (m && m->img) {
                GraphicsPath cp; buildRoundRect(cp, tr.X + 6, tr.Y + 6, tr.Width - 12, tr.Height - 12, 6);
                g.SetClip(&cp);
                g.DrawImage(m->img, RectF(tr.X + 6, tr.Y + 6, tr.Width - 12, tr.Height - 12));
                g.ResetClip();
            }
        } else {
            // 空 pack：首字母
            wchar_t init[2] = { ps[i].name.empty() ? L'?' : (wchar_t)towupper(ps[i].name[0]), 0 };
            Font af(kFontFace, 14.0f, FontStyleBold, UnitPoint);
            SolidBrush ab(fade(pal.text_muted));
            StringFormat afmt; afmt.SetAlignment(StringAlignmentCenter); afmt.SetLineAlignment(StringAlignmentCenter);
            g.DrawString(init, -1, &af, tr, &afmt, &ab);
        }
        int idx_capt = i;
        hit(tr, [idx_capt](){ activePack() = idx_capt; packMenu().open = false; }, true);
        ty += cell + 4;
    }
    // 末尾 "+" 创建 tab
    {
        RectF tr(strip_x + 2, ty, cell, cell);
        bool hov = inRect(g_mouse, tr);
        if (hov) fillRR(g, tr.X, tr.Y, tr.Width, tr.Height, 8, fade(pal.bg));
        strokeRR(g, tr.X, tr.Y, tr.Width, tr.Height, 8, fade(pal.divider), 1.0f);
        Font af(kFontFace, 18.0f, FontStyleBold, UnitPoint);
        SolidBrush ab(fade(pal.text_muted));
        StringFormat afmt; afmt.SetAlignment(StringAlignmentCenter); afmt.SetLineAlignment(StringAlignmentCenter);
        g.DrawString(L"+", -1, &af, tr, &afmt, &ab);
        hit(tr, [](){ ::modal::openCreatePack(); }, true);
    }

    // ---------------- kebab 菜单 (overlay) ----------------
    auto& pm = packMenu();
    if (pm.open && pm.pack_idx == activePack() && !cur.is_system) {
        float mw = 130, mh = 96;
        float mx = pm.ax - mw + 24;
        float my = pm.ay;
        if (mx + mw > px + pw) mx = px + pw - mw - 4;
        drawShadow(g, mx, my, mw, mh, 8, fade(pal.shadow_card), 3, 2);
        fillRR(g, mx, my, mw, mh, 8, fade(pal.card));
        strokeRR(g, mx, my, mw, mh, 8, fade(pal.divider));
        struct Item { const wchar_t* label; std::function<void()> click; bool danger; };
        Item items[] = {
            { L"重命名", [](){ ::requestRenamePack(activePack()); packMenu().open = false; }, false },
            { L"分享",   [](){ ::requestSharePack(activePack());   packMenu().open = false; }, false },
            { L"删除",   [](){ ::requestDeletePack(activePack());   packMenu().open = false; }, true  },
        };
        float iy = my + 6;
        for (auto& it : items) {
            RectF r(mx + 4, iy, mw - 8, 26);
            bool h = inRect(g_mouse, r);
            if (h) fillRR(g, r.X, r.Y, r.Width, r.Height, 4, fade(pal.bg));
            Color tc = it.danger ? fade(Color(255, 0xE3, 0x4B, 0x4B)) : fade(pal.text);
            drawText_(g, it.label, r.X + 12, r.Y + 6, r.Width - 24, 9.0f, tc);
            hit(r, it.click, true);
            iy += 28;
        }
    }
}

// ============== chat header + main ==============
void paintChatPane(Graphics& g, RectF area) {
    const Palette& pal = palette();
    fillRR(g, area.X, area.Y, area.Width, area.Height, 0, pal.bg);

    // header 56
    float hdr_h = 56;
    RectF hdr(area.X, area.Y, area.Width, hdr_h);
    Pen sep(pal.divider, 1.0f);
    g.DrawLine(&sep, hdr.X, hdr.Y + hdr_h, hdr.X + hdr.Width, hdr.Y + hdr_h);
    // # + name + sub + actions
    auto* ch = activeChannel();
    drawText_(g, L"#", hdr.X + 18, hdr.Y + 16, 16, 14.0f, pal.text_muted, StringAlignmentNear);
    drawText_(g, ch->name, hdr.X + 36, hdr.Y + 14, 200, 11.0f, pal.text,
              StringAlignmentNear, FontStyleBold);
    drawText_(g, ch->is_market ? L"社区交易市场（出售 .cfg / 灵敏度配置）" : L"官方频道",
              hdr.X + 36, hdr.Y + 32, 300, 8.5f, pal.text_muted);
    // actions: search / more
    {
        float ax = hdr.X + hdr.Width - 14 - 34 * 2 - 4;
        for (int i = 0; i < 2; ++i) {
            RectF ar(ax, hdr.Y + 11, 34, 34);
            bool hov = inRect(g_mouse, ar);
            if (hov) fillRR(g, ar.X, ar.Y, ar.Width, ar.Height, 8, pal.card);
            icons::Name n = (i == 0) ? icons::Name::Search : icons::Name::More;
            icons::drawSvg(g, n, ar.X + 8, ar.Y + 8, 18, hov ? pal.text : pal.text_muted);
            ax += 38;
        }
    }

    // 如果 market 频道：market view 顶到 stream 区
    if (ch->is_market) {
        ::paintMarketView(g, RectF(area.X, area.Y + hdr_h, area.Width, area.Height - hdr_h));
        return;
    }

    // stream
    float comp_h = 64;
    RectF stream(area.X, area.Y + hdr_h, area.Width, area.Height - hdr_h - comp_h);

    auto& msgs = streamFor(g_active);
    float my = stream.Y + 12;
    float maxw = stream.Width - 32;
    for (size_t i = 0; i < msgs.size(); ++i) {
        const Msg& m = msgs[i];
        const Msg* prev = (i > 0) ? &msgs[i - 1] : nullptr;
        bool prev_same = prev && prev->kind == MsgKind::Text && m.kind == MsgKind::Text
                         && wcscmp(prev->from, m.from) == 0
                         && wcscmp(m.from, L"me") != 0;
        if (my > stream.Y + stream.Height) break;
        float h = paintBubble(g, m, stream.X + 16, my, maxw, pal, prev_same, (int)i);
        my += h;
    }

    // composer
    paintComposer(g, RectF(area.X, area.Y + area.Height - comp_h, area.Width, comp_h));

    // picker
    if (g_picker_open || g_picker_t.value() > 0.001f) {
        paintPicker(g, area.X + 14, area.Y + area.Height - comp_h);
    }
}

// ============== 顶层 entry ==============
void paintChatViewTop(Graphics& g, RectF area) {
    // view 切换 fade（共用 g_view_fade）
    float op = g_view_fade.started ? g_view_fade.value() : 1.0f;
    if (op < 0.999f) {
        // 平移 + 透明 — 简单做法：偏 8px 上 + 全局 alpha 控不住，所以直接改 area.Y
        // 让用户感受到切换；alpha 影响子调用复杂，暂只做 translate
        area.Y += (1.0f - op) * 8.0f;
    }
    float lw = 240;
    paintChatList(g, RectF(area.X, area.Y, lw, area.Height));
    paintChatPane(g, RectF(area.X + lw + 1, area.Y, area.Width - lw - 1, area.Height));
}

}  // namespace chatv
