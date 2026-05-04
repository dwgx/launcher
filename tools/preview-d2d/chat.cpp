// Chat view 实现 — 见 chat.h。GDI+ 等价 tools/preview/chat_view.inl。

#include "chat.h"
#include "icons.h"
#include "palette.h"
#include "user_state.h"
#include "net.h"
#include "hit.h"
#include "stages.h"
#include "sticker.h"
#include "render/primitives.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <ctime>
#include <memory>
#include <mutex>

namespace launcher::d2d::chat {

// fadeArgb 复用 — 把 ARGB 的 alpha 分量乘 op
namespace {
inline uint32_t fadeArgb(uint32_t argb, float op) {
    uint32_t a = (argb >> 24) & 0xFFu;
    a = (uint32_t)(a * op + 0.5f);
    if (a > 255) a = 255;
    return (a << 24) | (argb & 0xFFFFFFu);
}
}

// 8 官方频道（不要 touhou/vrchat — 跟 GDI+ 那边一致）
std::vector<Channel> g_channels = {
    { L"announcements", L"announcements", L"IMPORTANT", false, "", 1 },
    { L"rules",         L"rules",         L"IMPORTANT", false, "", 1 },
    { L"general",       L"general",       L"GENERAL",   false, "", 0 },
    { L"random",        L"random",        L"GENERAL",   false, "", 0 },
    { L"helpdesk",      L"helpdesk",      L"GENERAL",   false, "", 1 },
    { L"cs2",           L"cs2",           L"GAMES",     false, "", 1 },
    { L"market",        L"market",        L"SHOP",      true,  "", 1 },
    { L"trades",        L"trades",        L"SHOP",      false, "", 1 },
};
const wchar_t* kGroups[] = { L"IMPORTANT", L"GENERAL", L"GAMES", L"SHOP" };

std::wstring g_active = L"general";
InputBox     g_composer;
bool         g_focus_composer = false;
bool         g_picker_open = false;
Tween        g_picker_t;
int          g_picker_tab = 0;

Tween g_top_seg_x, g_top_seg_w;
Tween g_pack_tab_x, g_pack_tab_w;
PackDrag g_pack_drag;
std::unordered_map<std::wstring, ChatScroll> g_scroll;
ScrollBarDrag g_scroll_drag;

static std::unordered_map<std::wstring, std::vector<Msg>> g_streams;
static std::unordered_map<std::wstring, bool> g_group_collapsed;
static std::mutex g_streams_mtx;

// 头像 hit 表 — paintChatPane 帧首清空，paintBubble 填充，WM_RBUTTONDOWN 命中
struct AvatarHit { LayoutRect rect; std::wstring from; };
static std::vector<AvatarHit> g_avatar_hits;
// emoji 滚动偏移（picker 内部）
static float g_emoji_scroll_y = 0.0f;
static float g_emoji_grid_h_last = 0.0f;
static float g_emoji_total_h_last = 0.0f;
// 消息体 hit 表 — paintChatPane 帧首清空，paintBubble 填充
struct MsgHit { LayoutRect rect; int idx; };
static std::vector<MsgHit> g_msg_hits;
// 当前 picker 内 pack tab 实际像素位置（paintPicker 写，鼠标命中读用以拖拽）
struct PackTabRect { LayoutRect r; int idx; };
static std::vector<PackTabRect> g_pack_tab_rects;
// 鼠标在 picker 上的本地相对坐标（pack tabs 子区域）
static float g_picker_origin_x = 0, g_picker_origin_y = 0;

std::vector<Msg>& streamFor(const std::wstring& slug) {
    auto it = g_streams.find(slug);
    if (it == g_streams.end()) {
        it = g_streams.emplace(slug, std::vector<Msg>{}).first;
    }
    return it->second;
}

static std::unordered_map<std::wstring, bool> g_history_loaded;

void switchChannel(const std::wstring& slug) {
    g_active = slug;
    g_focus_composer = false;
    // 第一次切到这个频道 → 异步拉历史 + 滚到底
    if (!g_history_loaded[slug]) {
        g_history_loaded[slug] = true;
        fetchHistory(GetActiveWindow(), slug);
    }
    // 切到这个频道时不重置 scroll — 保留之前的位置（用户切到设置再切回来还在原位）
    // 但首次进入会通过 ChatScroll::initialized = false 自动 stick to bottom
}

void onWheel(int delta) {
    // picker 开 + emoji tab 时，滚轮滚 emoji grid
    if (g_picker_open && g_picker_tab == 0) {
        g_emoji_scroll_y -= (float)delta * 0.5f;
        return;
    }
    auto& sc = g_scroll[g_active];
    if (sc.total_height <= sc.viewport_h) return;
    // 写 target_offset，每帧 lerp 平滑到位（避免一格 60px 跳跃感）
    // 一次 wheel notch (delta=120) → 滚 90px，连续滚自动累积
    float dy = (float)delta * 0.75f;
    sc.target_offset += dy;
    float max_off = sc.total_height - sc.viewport_h;
    if (sc.target_offset > max_off) sc.target_offset = max_off;
    if (sc.target_offset < 0) sc.target_offset = 0;
}

void appendLocalMessage(Msg msg) {
    auto& s = streamFor(g_active);
    auto& sc = g_scroll[g_active];
    bool was_at_bottom = (sc.offset_from_bottom < 8.0f);
    s.push_back(std::move(msg));
    // 在底部就跟随；不在底部说明用户在翻历史，不打扰
    if (was_at_bottom) {
        sc.offset_from_bottom = 0;
        sc.target_offset = 0;
    }
}

void retargetTopSeg() {
    // 表情 / 表情包 顶部 seg pill — 0/1
    int idx = (g_picker_tab == 0) ? 0 : 1;
    float target_x = idx * (60 + 6);     // 第二个 tab 起点 (60 宽 + 6 间隔)
    float target_w = (idx == 0) ? 60.0f : 80.0f;     // 表情 60 / 表情包 80
    if (!g_top_seg_x.started) g_top_seg_x.start(target_x, target_x, 0.001f, 0, curve::easeOutQuint);
    else if (std::abs(g_top_seg_x.to - target_x) > 0.5f)
        g_top_seg_x.start(g_top_seg_x.value(), target_x, 0.30f, 0, curve::easeOutQuint);
    if (!g_top_seg_w.started) g_top_seg_w.start(target_w, target_w, 0.001f, 0, curve::easeOutQuint);
    else if (std::abs(g_top_seg_w.to - target_w) > 0.5f)
        g_top_seg_w.start(g_top_seg_w.value(), target_w, 0.30f, 0, curve::easeOutQuint);
}

void retargetPackTab() {
    int active = g_picker_tab - 1;     // pack idx (0-based 在 g_packs 里)
    if (active < 0) return;
    if (active >= (int)g_pack_tab_rects.size()) return;
    LayoutRect tr;
    bool found = false;
    for (auto& pt : g_pack_tab_rects) {
        if (pt.idx == active) { tr = pt.r; found = true; break; }
    }
    if (!found) return;
    if (!g_pack_tab_x.started) g_pack_tab_x.start(tr.x, tr.x, 0.001f, 0, curve::easeOutQuint);
    else if (std::abs(g_pack_tab_x.to - tr.x) > 0.5f)
        g_pack_tab_x.start(g_pack_tab_x.value(), tr.x, 0.28f, 0, curve::easeOutQuint);
    if (!g_pack_tab_w.started) g_pack_tab_w.start(tr.w, tr.w, 0.001f, 0, curve::easeOutQuint);
    else if (std::abs(g_pack_tab_w.to - tr.w) > 0.5f)
        g_pack_tab_w.start(g_pack_tab_w.value(), tr.w, 0.28f, 0, curve::easeOutQuint);
}

namespace {
struct HistArg { std::wstring slug; std::string chat_id; HWND h; };
std::mutex g_pending_hist_mtx;
std::unordered_map<std::wstring, std::vector<Msg>> g_pending_history;

std::wstring utf8wHist(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(n - 1, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}
}

void fetchHistory(HWND notify, const std::wstring& slug) {
    if (g_session_token.empty()) return;
    std::string chat_id;
    for (auto& c : g_channels) if (c.slug == slug) { chat_id = c.id; break; }
    if (chat_id.empty()) return;

    auto* a = new HistArg{ slug, chat_id, notify };
    CreateThread(nullptr, 0, [](LPVOID lp) -> DWORD {
        std::unique_ptr<HistArg> a((HistArg*)lp);
        std::string url = "/api/chat/history?session_token=" + g_session_token
                        + "&chat_id=" + a->chat_id + "&limit=100";
        std::wstring wurl(url.begin(), url.end());
        auto r = net::request(L"GET", wurl.c_str(), {}, L"");
        if (!r.ok()) return 0;
        // 后端 history 真实字段（chat.rs MessageOut）：
        //   id (i64) / sender_id (Option<String>) / msg_type / payload (JSON Value)
        //   / created_at (i64) / deleted (bool)
        std::vector<Msg> msgs;
        size_t pos = 0;
        while (true) {
            auto ob = r.body.find('{', pos);
            if (ob == std::string::npos) break;
            // 找匹配的 }（payload 可能是 nested object）— 简单 brace 计数
            int depth = 1;
            size_t scan = ob + 1;
            while (scan < r.body.size() && depth > 0) {
                char c = r.body[scan];
                if (c == '"') {
                    // 跳过字符串
                    ++scan;
                    while (scan < r.body.size() && r.body[scan] != '"') {
                        if (r.body[scan] == '\\' && scan + 1 < r.body.size()) ++scan;
                        ++scan;
                    }
                } else if (c == '{') depth++;
                else if (c == '}') depth--;
                if (depth == 0) break;
                ++scan;
            }
            if (depth != 0) break;
            size_t cb = scan;
            std::string obj = r.body.substr(ob, cb - ob + 1);
            // deleted 软删除消息跳过
            {
                auto pd = obj.find("\"deleted\":");
                if (pd != std::string::npos
                    && obj.compare(pd + 10, 4, "true") == 0) {
                    pos = cb + 1;
                    continue;
                }
            }
            Msg m;
            std::string kind = net::jsonStr(obj, "msg_type");
            if (kind == "sticker") m.kind = MsgKind::Sticker;
            else if (kind == "image") m.kind = MsgKind::Image;
            else if (kind == "gif") m.kind = MsgKind::Gif;
            else if (kind == "video") m.kind = MsgKind::Video;
            else if (kind == "system") m.kind = MsgKind::System;
            else m.kind = MsgKind::Text;
            m.server_id = net::jsonInt(obj, "id");
            // sender_id 是 UUID — 跟当前 user_id 比较决定 me / 别人
            std::string sender = net::jsonStr(obj, "sender_id");
            if (!g_user_id.empty() && sender == g_user_id) {
                m.from = L"me";
                m.author = g_user.nickname;
            } else {
                // 别人：暂时显示 sender_id 前 8 字 — 真昵称留 peer-cache 的 fetchUserNickname 拉
                if (sender.size() > 8) sender = sender.substr(0, 8);
                m.from = utf8wHist(sender);
                m.author = m.from;
            }
            // payload 可能是 string 字面量 "abc" 或 JSON object {...}（image/sticker 含 url 等）
            // 简单做法：找 "payload":" 后第一个 unescaped " 之间的内容
            {
                auto pp = obj.find("\"payload\":");
                if (pp != std::string::npos) {
                    pp += 10;
                    while (pp < obj.size() && (obj[pp] == ' ' || obj[pp] == '\t')) ++pp;
                    if (pp < obj.size() && obj[pp] == '"') {
                        ++pp;
                        std::string tmp;
                        while (pp < obj.size() && obj[pp] != '"') {
                            if (obj[pp] == '\\' && pp + 1 < obj.size()) {
                                char nc = obj[pp + 1];
                                if (nc == 'n') tmp.push_back('\n');
                                else if (nc == 't') tmp.push_back('\t');
                                else if (nc == 'r') tmp.push_back('\r');
                                else tmp.push_back(nc);
                                pp += 2;
                            } else {
                                tmp.push_back(obj[pp]);
                                ++pp;
                            }
                        }
                        m.body = utf8wHist(tmp);
                    }
                    // payload 是 object 时（image/video）— 提 url 字段
                    else if (pp < obj.size() && obj[pp] == '{') {
                        std::string sub_url = net::jsonStr(obj.substr(pp), "url");
                        if (sub_url.empty()) sub_url = net::jsonStr(obj.substr(pp), "media_url");
                        m.body = utf8wHist(sub_url);
                    }
                }
            }
            // created_at i64 → HH:MM 格式
            int64_t ts = net::jsonInt(obj, "created_at");
            if (ts > 0) {
                time_t tt = (time_t)ts;
                struct tm lt{};
                localtime_s(&lt, &tt);
                wchar_t tbuf[16];
                swprintf_s(tbuf, L"%02d:%02d", lt.tm_hour, lt.tm_min);
                m.time = tbuf;
            } else {
                m.time = L"";
            }
            m.status = L"online";
            msgs.push_back(std::move(m));
            pos = cb + 1;
        }
        {
            std::lock_guard<std::mutex> lk(g_pending_hist_mtx);
            g_pending_history[a->slug] = std::move(msgs);
        }
        // PostMessage 让主线程把 pending 替换到 streamFor
        auto* slug_p = new std::wstring(a->slug);
        PostMessageW(a->h, WM_APP + 45, 0, (LPARAM)slug_p);
        return 0;
    }, a, 0, nullptr);
}

// 主线程调（WM_APP+45）— merge 历史到 streamFor
void applyHistoryResult(const std::wstring& slug) {
    std::vector<Msg> msgs;
    {
        std::lock_guard<std::mutex> lk(g_pending_hist_mtx);
        auto it = g_pending_history.find(slug);
        if (it == g_pending_history.end()) return;
        msgs = std::move(it->second);
        g_pending_history.erase(it);
    }
    auto& s = streamFor(slug);
    // 历史消息插到流的开头（之前实时收到的"me"放在后面）
    if (s.empty()) {
        s = std::move(msgs);
    } else {
        // 简单 merge：历史在前，本地实时在后
        std::vector<Msg> merged = std::move(msgs);
        for (auto& m : s) merged.push_back(std::move(m));
        s = std::move(merged);
    }
}

static Channel* activeChannel() {
    for (auto& c : g_channels) if (c.slug == g_active) return &c;
    return &g_channels[2];   // general
}

static float measureW(D2DApp& app, std::wstring_view s, IDWriteTextFormat* fmt) {
    if (s.empty() || !fmt) return 0.0f;
    DWRITE_TEXT_METRICS m{};
    if (!app.texts().measure(fmt, s, 8192.0f, 256.0f, &m)) return 0.0f;
    return m.width;
}

void tick(float dt) {
    g_picker_t.tick(dt);
    g_top_seg_x.tick(dt); g_top_seg_w.tick(dt);
    g_pack_tab_x.tick(dt); g_pack_tab_w.tick(dt);
}

void appendMedia(const std::wstring& path) {
    Msg m;
    std::wstring p = path;
    auto dot = p.find_last_of(L'.');
    std::wstring ext = (dot != std::wstring::npos) ? p.substr(dot) : L"";
    for (auto& c : ext) c = (wchar_t)towlower(c);
    if (ext == L".png" || ext == L".jpg" || ext == L".jpeg" || ext == L".webp" || ext == L".bmp") {
        m.kind = MsgKind::Image;
    } else if (ext == L".gif") {
        m.kind = MsgKind::Gif;
    } else if (ext == L".mp4" || ext == L".webm" || ext == L".mov" || ext == L".avi" || ext == L".mkv") {
        m.kind = MsgKind::Video;
    } else {
        m.kind = MsgKind::Text;
        m.body = L"[文件] " + path;
        m.from = L"me"; m.time = L"now";
        appendLocalMessage(std::move(m));
        return;
    }
    m.from = L"me";
    m.body = path;
    m.time = L"now";
    appendLocalMessage(std::move(m));
}

// ============== 频道列表 ==============
static void paintChatList(D2DApp& app, float ax, float ay, float aw, float ah) {
    const Palette& pal = palette();
    auto* ctx = app.ctx();
    auto& br = app.brushes();

    prim::fillRect(ctx, ax, ay, aw, ah, br.solid(pal.bg));
    prim::drawLine(ctx, ax + aw, ay, ax + aw, ay + ah,
                   br.solid(pal.divider), 1.0f);

    auto* hdr_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(11.0f),
                                       DWRITE_FONT_WEIGHT_BOLD);
    auto* grp_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(8.0f),
                                       DWRITE_FONT_WEIGHT_BOLD);
    auto* ch_fmt  = app.texts().format(L"Microsoft YaHei UI", ptToDip(9.5f));
    auto* ch_active = app.texts().format(L"Microsoft YaHei UI", ptToDip(9.5f),
                                         DWRITE_FONT_WEIGHT_BOLD);

    // header
    float hy = ay + 12;
    prim::fillRR(ctx, ax + 14, hy - 2, 26, 26, 7.0f, br.solid(pal.primary));
    icons::drawIcon(app, icons::Name::Logo, ax + 14 + 5, hy - 2 + 5, 16, 0xFFFFFFFF);
    prim::drawText_(ctx, L"Launcher Server", hdr_fmt,
                    ax + 50, hy + 2, 200, 22, br.solid(pal.text));
    prim::drawLine(ctx, ax + 8, ay + 44, ax + aw - 8, ay + 44,
                   br.solid(pal.divider), 1.0f);

    float row_y = ay + 50;
    for (auto* gname : kGroups) {
        LayoutRect ghead{ ax + 6, row_y, aw - 12, 22 };
        bool ghov = ghead.contains(g_mouse);
        if (ghov) {
            prim::fillRR(ctx, ghead.x, ghead.y, ghead.w, ghead.h, 4.0f,
                         br.solidA(pal.text, 0.05f));
        }
        bool collapsed = g_group_collapsed[gname];
        prim::drawText_(ctx, collapsed ? L"▸" : L"▾", grp_fmt,
                        ax + 10, row_y + 4, 12, 14,
                        br.solid(pal.text_muted));
        prim::drawText_(ctx, gname, grp_fmt,
                        ax + 26, row_y + 4, 200, 14,
                        br.solid(pal.text_muted));
        const wchar_t* gn = gname;
        hit(ghead, [gn]() { g_group_collapsed[gn] = !g_group_collapsed[gn]; }, true);
        row_y += 24;

        if (collapsed) { row_y += 6; continue; }

        for (auto& c : g_channels) {
            if (wcscmp(c.group, gname) != 0) continue;
            bool active = (c.slug == g_active);
            LayoutRect cr{ ax + 6, row_y, aw - 12, 28 };
            bool hov = cr.contains(g_mouse);
            if (active) {
                prim::fillRR(ctx, cr.x, cr.y, cr.w, cr.h, 6.0f,
                             br.solidA(pal.primary, 0.14f));
            } else if (hov) {
                prim::fillRR(ctx, cr.x, cr.y, cr.w, cr.h, 6.0f,
                             br.solidA(pal.text, 0.04f));
            }
            uint32_t tc = active ? pal.primary : pal.text_muted;
            prim::drawText_(ctx, L"#", grp_fmt,
                            cr.x + 12, cr.y + 6, 14, 16,
                            br.solid(tc));
            prim::drawText_(ctx, c.name, active ? ch_active : ch_fmt,
                            cr.x + 26, cr.y + 7, cr.w - 60, 18,
                            br.solid(active ? pal.text : pal.text_muted));
            std::wstring tgt = c.slug;
            hit(cr, [tgt]() { switchChannel(tgt); }, true);
            row_y += 30;
        }
        row_y += 6;
    }
}

// 纯量高度 — 跟 paintBubble 完全镜像但不画任何 D2D / 不 push hit。
// 用来在真画之前一次过算 total，给 scroll offset 定位。
static float measureBubbleHeight(D2DApp& app, const Msg& m, float maxw, bool prev_same_author) {
    auto* body_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(9.5f));
    // 空 body 直接占 0 高度（与 paintBubble 行为一致）
    if (m.kind == MsgKind::Text && m.body.empty()) return 0;
    if (m.kind == MsgKind::DayDivider) return 30;
    if (m.kind == MsgKind::System)     return 32;
    if (m.kind == MsgKind::Image || m.kind == MsgKind::Gif) {
        float bub_w = 240, bub_h = 180;
        D2D1_SIZE_F sz{ 0, 0 };
        ID2D1Bitmap* bmp = nullptr;
        if (m.kind == MsgKind::Gif) {
            auto* a = app.gifs().fromFile(m.body);
            if (a) { sz.width = (float)a->width; sz.height = (float)a->height; }
        }
        if (sz.width <= 0) {
            bmp = app.images().fromFile(m.body);
            if (bmp) sz = bmp->GetSize();
        }
        if (sz.width > 0 && sz.height > 0) {
            float aspect = sz.height / sz.width;
            float max_w = (std::min)(maxw * 0.55f, 320.0f);
            bub_w = (std::min)(max_w, sz.width);
            bub_h = bub_w * aspect;
            if (bub_h > 240) { bub_h = 240; bub_w = bub_h / aspect; }
        }
        return (prev_same_author ? bub_h : bub_h + 22) + 6;
    }
    if (m.kind == MsgKind::Video) {
        return (prev_same_author ? 140.0f : 162.0f) + 6;
    }
    if (m.kind == MsgKind::Sticker) {
        return (prev_same_author ? 100.0f : 122.0f) + 6;
    }
    // text — 处理 launcher://pack/ link 卡片
    auto find_url = [](const std::wstring& s) -> std::wstring {
        size_t p = s.find(L"launcher://");
        if (p == std::wstring::npos) {
            p = s.find(L"https://");
            if (p == std::wstring::npos) p = s.find(L"http://");
        }
        if (p == std::wstring::npos) return {};
        size_t e = p;
        while (e < s.size() && s[e] > 0x20 && s[e] != L' ') e++;
        return s.substr(p, e - p);
    };
    std::wstring url = find_url(m.body);
    if (!url.empty() && url.compare(0, 15, L"launcher://pack/") == 0) {
        return (prev_same_author ? 88.0f : 110.0f) + 6;
    }
    if (m.body.empty()) {
        // 空消息 — 不算高度（实际 paint 也跳过）
        return prev_same_author ? 0.0f : 22.0f + 6.0f;
    }
    float bub_max_w = (std::min)(maxw * 0.65f, 480.0f);
    DWRITE_TEXT_METRICS tm{};
    app.texts().measure(body_fmt, m.body, bub_max_w - 28, 8192, &tm);
    float bub_h = (std::max)(tm.height + 18, 28.0f);
    if (!url.empty()) bub_h += 4;
    return (prev_same_author ? bub_h : bub_h + 22) + 6;
}

// ============== 单条气泡 ==============
// 自己消息靠右 / 别人靠左。idx = 在 streamFor(g_active) 里的位置，用于消息 hit 注册（右键菜单）。
static float paintBubble(D2DApp& app, const Msg& m, int idx, float x, float y, float maxw,
                         bool prev_same_author) {
    const Palette& pal = palette();
    auto* ctx = app.ctx();
    auto& br = app.brushes();

    // Text 类型 + body 全空 — 整条直接跳过（避免画了头像/作者却没气泡的"幽灵行"）
    if (m.kind == MsgKind::Text && m.body.empty()) {
        return 0;
    }

    if (m.kind == MsgKind::DayDivider) {
        auto* pill_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(7.5f));
        float tw = measureW(app, m.body, pill_fmt) + 24;
        float bx = x + (maxw - tw) * 0.5f;
        prim::fillRR(ctx, bx, y + 6, tw, 18, 9.0f, br.solidA(pal.text, 0.05f));
        prim::drawText_(ctx, m.body, pill_fmt,
                        bx, y + 9, tw, 14,
                        br.solid(pal.text_muted),
                        DWRITE_TEXT_ALIGNMENT_CENTER);
        return 30;
    }
    if (m.kind == MsgKind::System) {
        auto* sys_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(8.5f));
        float tw = measureW(app, m.body, sys_fmt) + 24;
        float bx = x + (maxw - tw) * 0.5f;
        prim::fillRR(ctx, bx, y + 4, tw, 22, 11.0f, br.solidA(pal.text, 0.05f));
        prim::drawText_(ctx, m.body, sys_fmt,
                        bx, y + 8, tw, 16,
                        br.solid(pal.text_muted),
                        DWRITE_TEXT_ALIGNMENT_CENTER);
        return 32;
    }

    bool me = (m.from == L"me");

    auto* author_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(8.0f),
                                          DWRITE_FONT_WEIGHT_BOLD);
    auto* time_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(7.0f));
    auto* body_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(9.5f));

    // 头像 28×28 — me 在右边，别人在左边
    constexpr float ar = 14.0f;
    constexpr float gap = 10.0f;
    float avatar_x = me ? (x + maxw - ar * 2) : x;
    float ay = y + 4;
    if (!prev_same_author) {
        bool drew_real = false;
        // me 用真头像（g_avatar_path BitmapBrush 圆形裁剪）
        if (me && !g_avatar_path.empty()) {
            auto* abmp = app.images().fromFile(g_avatar_path);
            if (abmp) {
                D2D1_BITMAP_BRUSH_PROPERTIES bp = D2D1::BitmapBrushProperties(
                    D2D1_EXTEND_MODE_CLAMP, D2D1_EXTEND_MODE_CLAMP,
                    D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                ComPtr<ID2D1BitmapBrush> bb;
                if (SUCCEEDED(ctx->CreateBitmapBrush(abmp, bp, &bb))) {
                    D2D1_SIZE_F sz = abmp->GetSize();
                    if (sz.width > 0 && sz.height > 0) {
                        float sx = (ar * 2) / sz.width;
                        float sy = (ar * 2) / sz.height;
                        auto mt = D2D1::Matrix3x2F::Scale({sx, sy}, {0, 0})
                                * D2D1::Matrix3x2F::Translation(avatar_x, ay);
                        bb->SetTransform(mt);
                        ctx->FillEllipse(D2D1::Ellipse({avatar_x + ar, ay + ar}, ar, ar), bb.Get());
                        drew_real = true;
                    }
                }
            }
        }
        if (!drew_real) {
            prim::fillCircle(ctx, avatar_x + ar, ay + ar, ar, br.solid(pal.primary));
            // 自己用 nickname 首字，否则用 from 首字（对方 UUID 前 8 字的首字符）
            wchar_t key = me
                ? (g_user.nickname.empty() ? L'?' : g_user.nickname[0])
                : (m.from.empty() ? L'?' : m.from[0]);
            wchar_t initial[2] = { (wchar_t)towupper(key), 0 };
            auto* init_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(8.5f),
                                                DWRITE_FONT_WEIGHT_BOLD);
            prim::drawText_(ctx, initial, init_fmt,
                            avatar_x, ay, ar * 2, ar * 2,
                            br.solid(0xFFFFFFFF),
                            DWRITE_TEXT_ALIGNMENT_CENTER,
                            DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
        // 头像左键看主页（me 自己跳过）
        if (!me && !m.from.empty()) {
            g_avatar_hits.push_back({ { avatar_x, ay, ar * 2, ar * 2 }, m.from });
            std::wstring fcopy = m.from;
            hit({ avatar_x, ay, ar * 2, ar * 2 }, [fcopy](){
                static std::wstring g_pending_peer;
                g_pending_peer = fcopy;
                PostMessageW(GetActiveWindow(), WM_APP + 37, 0,
                             (LPARAM)&g_pending_peer);
            }, true);
        }
    }
    // bub 起点：left/right
    float bub_inner_w_max = (std::min)(maxw - ar * 2 - gap, 480.0f);
    auto bub_x_for = [&](float bub_w) -> float {
        if (me) return avatar_x - gap - bub_w;
        return avatar_x + ar * 2 + gap;
    };
    float bub_y = y + (prev_same_author ? 0 : 22);
    if (!prev_same_author) {
        // author + time 在气泡上方那一行
        // me：和气泡一样靠右；别人：和气泡靠左
        // 自己显示真昵称（不是 "me" 字面量）
        std::wstring author_disp;
        if (me) {
            author_disp = !m.author.empty() ? m.author
                        : (g_user.nickname.empty() ? std::wstring(L"我") : g_user.nickname);
        } else {
            author_disp = m.author.empty() ? m.from : m.author;
        }
        float aw_ = measureW(app, author_disp, author_fmt);
        float tw_ = m.time.empty() ? 0 : (measureW(app, m.time, time_fmt) + 8);
        float meta_w = aw_ + tw_;
        float meta_x = me ? (avatar_x - gap - meta_w) : (avatar_x + ar * 2 + gap);
        prim::drawText_(ctx, author_disp, author_fmt,
                        meta_x, y + 2, aw_ + 4, 14,
                        br.solid(me ? pal.primary : pal.text));
        if (!m.time.empty()) {
            prim::drawText_(ctx, m.time, time_fmt,
                            meta_x + aw_ + 8, y + 4, tw_, 12,
                            br.solid(pal.text_muted));
        }
    }

    // ---------- 媒体气泡 (Image/Gif/Video) ----------
    if (m.kind == MsgKind::Image || m.kind == MsgKind::Gif) {
        float bub_w = 240, bub_h = 180;
        ID2D1Bitmap* draw_bmp = nullptr;
        D2D1_SIZE_F sz = { 0, 0 };

        if (m.kind == MsgKind::Gif) {
            // GIF 多帧 — IWICBitmapDecoder GetFrameCount + /grctlext/Delay
            auto* anim = app.gifs().fromFile(m.body);
            if (anim) {
                draw_bmp = app.gifs().frameAt(anim, stages::g_time_in_stage);
                sz.width = (float)anim->width;
                sz.height = (float)anim->height;
            }
        }
        if (!draw_bmp) {
            // Image 或 GIF 解码失败 → 退到单帧 ID2D1Bitmap
            draw_bmp = app.images().fromFile(m.body);
            if (draw_bmp) sz = draw_bmp->GetSize();
        }

        if (draw_bmp) {
            if (sz.width > 0 && sz.height > 0) {
                float aspect = sz.height / sz.width;
                float max_w = (std::min)(maxw * 0.55f, 320.0f);
                bub_w = (std::min)(max_w, sz.width);
                bub_h = bub_w * aspect;
                if (bub_h > 240) { bub_h = 240; bub_w = bub_h / aspect; }
            }
        }
        float bub_x = bub_x_for(bub_w);
        if (draw_bmp) {
            // 真圆角 mask（之前 PushAxisAlignedClip 只裁矩形 4 角是直的）
            prim::pushLayerRR(ctx, app.factory(), bub_x, bub_y, bub_w, bub_h, 12.0f);
            ctx->DrawBitmap(draw_bmp, D2D1::RectF(bub_x, bub_y, bub_x + bub_w, bub_y + bub_h),
                            1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            prim::popLayer(ctx);
            prim::strokeRR(ctx, bub_x, bub_y, bub_w, bub_h, 12.0f,
                           br.solidA(pal.divider, 0.5f), 1.0f);
        } else {
            prim::fillRR(ctx, bub_x, bub_y, bub_w, bub_h, 12.0f,
                         br.solid(pal.surface));
            prim::drawText_(ctx, m.kind == MsgKind::Gif ? L"[GIF]" : L"[Image]", body_fmt,
                            bub_x, bub_y + bub_h * 0.4f, bub_w, 22,
                            br.solid(pal.text_muted),
                            DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        if (m.kind == MsgKind::Gif) {
            prim::fillRR(ctx, bub_x + bub_w - 36, bub_y + 6, 30, 16, 4,
                         br.solidA(0x000000, 0.55f));
            auto* gif_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(7.0f),
                                               DWRITE_FONT_WEIGHT_BOLD);
            prim::drawText_(ctx, L"GIF", gif_fmt,
                            bub_x + bub_w - 36, bub_y + 7, 30, 14,
                            br.solid(0xFFFFFFFF),
                            DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        g_msg_hits.push_back({ { bub_x, bub_y, bub_w, bub_h }, idx });
        return (prev_same_author ? bub_h : bub_h + 22) + 6;
    }

    if (m.kind == MsgKind::Video) {
        float bub_w = 240, bub_h = 140;
        float bub_x = bub_x_for(bub_w);
        prim::fillRR(ctx, bub_x, bub_y, bub_w, bub_h, 12.0f,
                     br.solid(pal.surface));
        prim::fillCircle(ctx, bub_x + bub_w * 0.5f, bub_y + bub_h * 0.5f, 28,
                         br.solidA(0x000000, 0.65f));
        prim::strokeCircle(ctx, bub_x + bub_w * 0.5f, bub_y + bub_h * 0.5f, 28,
                           br.solid(0xFFFFFFFF), 2.0f);
        icons::drawIcon(app, icons::Name::Play,
                        bub_x + bub_w * 0.5f - 12,
                        bub_y + bub_h * 0.5f - 12, 24,
                        0xFFFFFFFF);
        prim::drawText_(ctx, L"点击播放视频", body_fmt,
                        bub_x, bub_y + bub_h - 24, bub_w, 18,
                        br.solid(pal.text_muted),
                        DWRITE_TEXT_ALIGNMENT_CENTER);
        // 点击 → WebView2 内嵌播放器
        std::wstring src = m.body;
        hit({ bub_x, bub_y, bub_w, bub_h }, [src](){
            static std::wstring g_pending_video;
            g_pending_video = src;
            PostMessageW(GetActiveWindow(), WM_APP + 46,
                         (WPARAM)&g_pending_video, 0);
        }, true);
        g_msg_hits.push_back({ { bub_x, bub_y, bub_w, bub_h }, idx });
        return (prev_same_author ? bub_h : bub_h + 22) + 6;
    }

    if (m.kind == MsgKind::Sticker) {
        float bub_w = 100, bub_h = 100;
        float bub_x = bub_x_for(bub_w);
        // sticker 也支持 GIF
        ID2D1Bitmap* sbmp = nullptr;
        auto sd = m.body.find_last_of(L'.');
        bool s_is_gif = (sd != std::wstring::npos
                         && (m.body.substr(sd) == L".gif"
                             || m.body.substr(sd) == L".GIF"));
        if (s_is_gif) {
            auto* sa = app.gifs().fromFile(m.body);
            if (sa) sbmp = app.gifs().frameAt(sa, stages::g_time_in_stage);
        }
        if (!sbmp) sbmp = app.images().fromFile(m.body);
        if (sbmp) {
            prim::pushLayerRR(ctx, app.factory(), bub_x, bub_y, bub_w, bub_h, 16.0f);
            ctx->DrawBitmap(sbmp, D2D1::RectF(bub_x, bub_y, bub_x + bub_w, bub_y + bub_h),
                            1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            prim::popLayer(ctx);
        } else {
            prim::fillRR(ctx, bub_x, bub_y, bub_w, bub_h, 16.0f,
                         br.solid(pal.surface));
        }
        g_msg_hits.push_back({ { bub_x, bub_y, bub_w, bub_h }, idx });
        return (prev_same_author ? bub_h : bub_h + 22) + 6;
    }

    // ---------- Text bubble ----------
    // 空 body 直接跳过（避免后端 trim 后空白 + 错误 payload 显示成 28-px 小气泡）
    if (m.body.empty()) {
        return prev_same_author ? 0.0f : 22.0f + 6.0f;
    }
    // 如果文本里有 launcher://pack/<short>，特殊渲染为 pack 分享卡片（缩略图 + 标题 + 行动按钮）。
    auto find_url = [](const std::wstring& s) -> std::wstring {
        // launcher:// 优先
        size_t p = s.find(L"launcher://");
        if (p == std::wstring::npos) {
            p = s.find(L"https://");
            if (p == std::wstring::npos) p = s.find(L"http://");
        }
        if (p == std::wstring::npos) return {};
        size_t e = p;
        while (e < s.size() && s[e] > 0x20 && s[e] != L' ') e++;
        return s.substr(p, e - p);
    };
    std::wstring url = find_url(m.body);
    bool is_pack_link = !url.empty() && url.compare(0, 15, L"launcher://pack/") == 0;
    if (is_pack_link) {
        // 抽 short_name
        std::wstring short_w = url.substr(15);
        std::string short_a;
        for (wchar_t c : short_w) if (c) short_a.push_back((char)c);
        // pack-share 卡片：320×88，左边 64×64 缩略图（cache 后的 cover）+ 标题 + 提示
        float bub_w = 320, bub_h = 88;
        float bub_x = bub_x_for(bub_w);
        // 渐变 / 主色描边
        prim::fillRR(ctx, bub_x, bub_y, bub_w, bub_h, 12.0f, br.solid(pal.card));
        prim::strokeRR(ctx, bub_x, bub_y, bub_w, bub_h, 12.0f,
                       br.solidA(pal.primary, 0.4f), 1.5f);
        // 缩略图占位（如果 g_pack_preview 跟当前 short 匹配则用 cover；否则灰）
        ID2D1Bitmap* thumb = nullptr;
        {
            std::lock_guard<std::mutex> lk(sticker::g_pack_preview_mtx);
            if (sticker::g_pack_preview.short_name == short_a
                && !sticker::g_pack_preview.cover_path.empty()) {
                thumb = app.images().fromFile(sticker::g_pack_preview.cover_path);
            }
        }
        if (thumb) {
            prim::pushLayerRR(ctx, app.factory(), bub_x + 12, bub_y + 12, 64, 64, 8.0f);
            ctx->DrawBitmap(thumb, D2D1::RectF(bub_x + 12, bub_y + 12, bub_x + 76, bub_y + 76),
                            1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            prim::popLayer(ctx);
        } else {
            prim::fillRR(ctx, bub_x + 12, bub_y + 12, 64, 64, 8.0f,
                         br.solidA(pal.primary, 0.18f));
            auto* ic_fmt = app.texts().format(L"Segoe UI Emoji", ptToDip(20.0f));
            prim::drawText_(ctx, L"🎴", ic_fmt,
                            bub_x + 12, bub_y + 16, 64, 56,
                            br.solid(pal.primary),
                            DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        // 标题
        auto* tt_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(10.5f),
                                          DWRITE_FONT_WEIGHT_BOLD);
        auto* sub_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(8.5f));
        prim::drawText_(ctx, L"分享的表情包", tt_fmt,
                        bub_x + 88, bub_y + 14, bub_w - 100, 20,
                        br.solid(pal.text));
        std::wstring code_disp = L"launcher://pack/" + short_w;
        if (code_disp.size() > 32) code_disp = code_disp.substr(0, 32) + L"…";
        prim::drawText_(ctx, code_disp, sub_fmt,
                        bub_x + 88, bub_y + 36, bub_w - 100, 18,
                        br.solid(pal.text_muted));
        prim::drawText_(ctx, L"点击查看 / 添加分组 →", sub_fmt,
                        bub_x + 88, bub_y + 58, bub_w - 100, 18,
                        br.solid(pal.primary));

        std::string short_copy = short_a;
        // 第一次见到这个 short → 后台拉 cover 进缓存（不用阻塞 paint）
        static std::unordered_map<std::string, bool> g_pack_thumb_kicked;
        if (!g_pack_thumb_kicked[short_copy]) {
            g_pack_thumb_kicked[short_copy] = true;
            sticker::previewPackByShort(GetActiveWindow(), short_copy);
        }
        hit({ bub_x, bub_y, bub_w, bub_h }, [short_copy](){
            static std::string g_pending_short;
            g_pending_short = short_copy;
            PostMessageW(GetActiveWindow(), WM_APP + 49,
                         (WPARAM)&g_pending_short, 0);
        }, true);
        g_msg_hits.push_back({ { bub_x, bub_y, bub_w, bub_h }, idx });
        return (prev_same_author ? bub_h : bub_h + 22) + 6;
    }

    float bub_max_w = (std::min)(maxw * 0.65f, 480.0f);
    DWRITE_TEXT_METRICS tm{};
    app.texts().measure(body_fmt, m.body, bub_max_w - 28, 8192, &tm);
    float bub_w = tm.width + 28;
    float bub_h = (std::max)(tm.height + 18, 28.0f);
    float bub_x = bub_x_for(bub_w);

    uint32_t bub_bg = me ? pal.primary : pal.card;
    uint32_t bub_fg = me ? 0xFFFFFFFF : pal.text;
    prim::fillRR(ctx, bub_x, bub_y, bub_w, bub_h, 12.0f,
                 br.solid(bub_bg));
    prim::drawText_(ctx, m.body, body_fmt,
                    bub_x + 14, bub_y + 8, bub_w - 28, bub_h - 16,
                    br.solid(bub_fg));

    if (!url.empty()) {
        // 链接气泡下加一个小提示行 + hit 整个气泡 → WebView2 打开
        auto* link_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(7.5f),
                                            DWRITE_FONT_WEIGHT_BOLD);
        prim::drawText_(ctx, L"↗ 点击打开", link_fmt,
                        bub_x + 14, bub_y + bub_h - 14, bub_w - 28, 12,
                        br.solidA(me ? 0xFFFFFF : 0xC96442, 0.7f));
        bub_h += 4;
        std::wstring url_copy = url;
        hit({ bub_x, bub_y, bub_w, bub_h }, [url_copy](){
            static std::wstring g_pending_url;
            g_pending_url = url_copy;
            PostMessageW(GetActiveWindow(), WM_APP + 47,
                         (WPARAM)&g_pending_url, 0);
        }, true);
    }
    g_msg_hits.push_back({ { bub_x, bub_y, bub_w, bub_h }, idx });

    return (prev_same_author ? bub_h : bub_h + 22) + 6;
}

// ============== 异步发消息 ==============
namespace {
struct SendArg {
    std::string session_token;
    std::string chat_id;
    std::string kind;          // text / sticker / image / gif
    std::wstring slug;
    std::wstring body;
    HWND hwnd;
};
struct LinkArg { std::wstring slug; std::wstring body; int64_t mid; };
std::mutex g_link_mtx;
std::vector<LinkArg> g_pending_links;
}

// 主线程 WM_APP+52 调 — 找最近一条 me message 没绑定 server_id 的，写入 mid
void applySendResult() {
    std::vector<LinkArg> arr;
    {
        std::lock_guard<std::mutex> lk(g_link_mtx);
        arr.swap(g_pending_links);
    }
    for (auto& la : arr) {
        auto& msgs = streamFor(la.slug);
        for (auto it = msgs.rbegin(); it != msgs.rend(); ++it) {
            if (it->from == L"me" && it->server_id == 0 && it->body == la.body) {
                it->server_id = la.mid;
                break;
            }
        }
    }
}

static void sendChatMessage(HWND hwnd, const std::wstring& body, const char* kind = "text") {
    if (g_session_token.empty()) return;
    auto* ch = activeChannel();
    if (ch->id.empty()) return;     // 还没拿到 backend uuid
    auto* a = new SendArg{ g_session_token, ch->id, kind, g_active, body, hwnd };
    CreateThread(nullptr, 0, [](LPVOID lp) -> DWORD {
        std::unique_ptr<SendArg> a((SendArg*)lp);
        std::string b = "{\"session_token\":\"" + a->session_token
                      + "\",\"chat_id\":\"" + a->chat_id
                      + "\",\"msg_type\":\"" + a->kind
                      + "\",\"payload\":\"" + net::jsonEscape(a->body) + "\"}";
        auto r = net::postJson(L"/api/chat/send", b);
        if (r.ok()) {
            int64_t mid = net::jsonInt(r.body, "id");
            if (mid > 0) {
                {
                    std::lock_guard<std::mutex> lk(g_link_mtx);
                    g_pending_links.push_back({ a->slug, a->body, mid });
                }
                PostMessageW(a->hwnd, WM_APP + 52, 0, 0);
            }
        }
        return 0;
    }, a, 0, nullptr);
}

// 异步删除消息 — 调后端 chat/delete (软删除)，本地立即移除
void deleteMessage(HWND hwnd, const std::wstring& slug, int64_t server_id) {
    {
        // 本地立即移除
        auto& msgs = streamFor(slug);
        for (auto it = msgs.begin(); it != msgs.end(); ++it) {
            if (it->server_id == server_id) { msgs.erase(it); break; }
        }
    }
    if (server_id == 0 || g_session_token.empty()) return;
    struct A { int64_t mid; HWND h; };
    auto* a = new A{ server_id, hwnd };
    CreateThread(nullptr, 0, [](LPVOID lp) -> DWORD {
        std::unique_ptr<A> a((A*)lp);
        char buf[64]; sprintf_s(buf, "%lld", (long long)a->mid);
        std::string body = "{\"session_token\":\"" + g_session_token
                         + "\",\"message_id\":" + buf + "}";
        auto r = net::postJson(L"/api/chat/delete", body);
        PostMessageW(a->h, WM_APP + 53, r.ok() ? 1 : 0, 0);
        return 0;
    }, a, 0, nullptr);
}

// WS 收到别人删除 — 在所有 stream 里找 server_id 摘掉
void onWsMessageDeleted(int64_t server_id) {
    std::lock_guard<std::mutex> lk(g_streams_mtx);
    for (auto& [slug, msgs] : g_streams) {
        for (auto it = msgs.begin(); it != msgs.end(); ++it) {
            if (it->server_id == server_id) { msgs.erase(it); return; }
        }
    }
}

// 兼容老调用名
static void sendTextMessage(HWND hwnd, const std::wstring& text) {
    sendChatMessage(hwnd, text, "text");
}

// ============== Composer ==============
static void paintComposer(D2DApp& app, float ax, float ay, float aw, float ah) {
    const Palette& pal = palette();
    auto* ctx = app.ctx();
    auto& br = app.brushes();
    prim::fillRect(ctx, ax, ay, aw, ah, br.solid(pal.bg));
    prim::drawLine(ctx, ax, ay, ax + aw, ay,
                   br.solid(pal.divider), 1.0f);

    const float ico_sz = 30.0f;
    float ix = ax + 14.0f;
    float iy = ay + (ah - ico_sz) * 0.5f;
    LayoutRect emoji_btn{ ix, iy, ico_sz, ico_sz };
    bool ehov = emoji_btn.contains(g_mouse);
    if (ehov) {
        prim::fillRR(ctx, ix, iy, ico_sz, ico_sz, 8.0f,
                     br.solid(pal.card));
    }
    icons::drawIcon(app, icons::Name::Smile, ix + 6, iy + 6, 18,
                    ehov ? pal.text : pal.text_muted);
    hit(emoji_btn, [](){
        g_picker_open = !g_picker_open;
        if (g_picker_open) g_picker_t.start(0, 1, 0.22f, 0, curve::easeOutBack);
        else               g_picker_t.start(g_picker_t.value(), 0, 0.18f, 0, curve::easeOutCubic);
    }, true);

    // textarea
    float fx = ix + ico_sz + 10.0f;
    float send_w = 38.0f;
    float fw = aw - (fx - ax) - 14.0f - send_w - 10.0f;
    float fh = ico_sz;
    float fy = iy;
    g_composer.bounds = { fx, fy, fw, fh };

    prim::fillRR(ctx, fx, fy, fw, fh, fh * 0.5f, br.solid(pal.card));
    auto* border = g_focus_composer ? br.solid(pal.primary) : br.solid(pal.divider);
    prim::strokeRR(ctx, fx, fy, fw, fh, fh * 0.5f, border,
                   g_focus_composer ? 1.4f : 1.0f);
    if (g_focus_composer) {
        prim::strokeRR(ctx, fx - 2, fy - 2, fw + 4, fh + 4, fh * 0.5f + 2,
                       br.solidA(pal.primary, 0.10f), 3.0f);
    }

    auto* tx_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(9.5f));
    const float pad_l = 16.0f;
    const float text_y = fy + (fh - 14.0f) * 0.5f;

    if (g_composer.text.empty()) {
        prim::drawText_(ctx, L"写点什么…", tx_fmt,
                        fx + pad_l, text_y, fw - pad_l * 2, 18,
                        br.solid(pal.text_muted));
    } else {
        // 选区
        if (g_focus_composer && g_composer.hasSelection()) {
            float pre_w = measureW(app,
                g_composer.displaySlice(0, g_composer.selStart()), tx_fmt);
            float in_w = measureW(app,
                g_composer.displaySlice(g_composer.selStart(), g_composer.selEnd()), tx_fmt);
            prim::fillRect(ctx,
                           fx + pad_l + pre_w, text_y - 1, in_w, 18,
                           br.solidA(pal.primary, 0.38f));
        }
        prim::drawText_(ctx, g_composer.text, tx_fmt,
                        fx + pad_l, text_y, fw - pad_l * 2, 18,
                        br.solid(pal.text));
    }

    // caret
    if (g_focus_composer && !g_composer.hasSelection()) {
        float pre_w = measureW(app,
            g_composer.displaySlice(0, g_composer.cursor), tx_fmt);
        int phase = (int)(stages::g_time_in_stage * 1000) % 1000;
        if (phase < 500) {
            prim::drawLine(ctx,
                           fx + pad_l + pre_w, fy + 7,
                           fx + pad_l + pre_w, fy + fh - 7,
                           br.solid(pal.primary), 1.5f);
        }
    }

    hit(g_composer.bounds, [](){ g_focus_composer = true; }, true);

    // send btn
    float sx = ax + aw - 14 - send_w;
    float sy = iy + (fh - send_w) * 0.5f;
    bool can_send = !g_composer.text.empty();
    LayoutRect send_btn{ sx, sy, send_w, send_w };
    bool sh_ = send_btn.contains(g_mouse);
    uint32_t sbg = !can_send ? fadeArgb(pal.primary, 0.55f)
                            : (sh_ ? pal.primary_hover : pal.primary);
    prim::fillCircle(ctx, sx + send_w * 0.5f, sy + send_w * 0.5f, send_w * 0.5f,
                     br.solid(sbg));
    icons::drawIcon(app, icons::Name::Send, sx + 10, sy + 10, 18, 0xFFFFFFFF);
    if (can_send) {
        hit(send_btn, []() {
            Msg m;
            m.kind = MsgKind::Text;
            m.from = L"me";
            m.author = L"";
            m.status = L"online";
            m.body = g_composer.text;
            m.time = L"now";
            appendLocalMessage(std::move(m));
            sendTextMessage(GetActiveWindow(), g_composer.text);
            g_composer.text.clear();
            g_composer.cursor = 0;
            g_composer.clearSel();
            g_focus_composer = true;
        }, true);
    }
}

// ============== Pane (header + stream + composer) ==============
static void paintChatPane(D2DApp& app, float ax, float ay, float aw, float ah) {
    const Palette& pal = palette();
    auto* ctx = app.ctx();
    auto& br = app.brushes();
    prim::fillRect(ctx, ax, ay, aw, ah, br.solid(pal.bg));
    g_avatar_hits.clear();   // 帧首清，paintBubble 会填充
    g_msg_hits.clear();      // 帧首清，paintBubble 注册消息体 hit

    // header
    float hdr_h = 56;
    prim::drawLine(ctx, ax, ay + hdr_h, ax + aw, ay + hdr_h,
                   br.solid(pal.divider), 1.0f);

    auto* ch = activeChannel();
    auto* hash_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(14.0f),
                                        DWRITE_FONT_WEIGHT_BOLD);
    auto* name_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(11.0f),
                                        DWRITE_FONT_WEIGHT_BOLD);
    auto* sub_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(8.5f));
    prim::drawText_(ctx, L"#", hash_fmt,
                    ax + 18, ay + 16, 16, 22, br.solid(pal.text_muted));
    prim::drawText_(ctx, ch->name, name_fmt,
                    ax + 36, ay + 14, 200, 22, br.solid(pal.text));
    prim::drawText_(ctx,
                    ch->is_market ? L"社区交易市场（出售 .cfg / 灵敏度配置）" : L"官方频道",
                    sub_fmt,
                    ax + 36, ay + 32, 300, 16, br.solid(pal.text_muted));

    // 右上 search / more 按钮
    float btn_x = ax + aw - 14 - 34 * 2 - 4;
    for (int i = 0; i < 2; ++i) {
        LayoutRect ar{ btn_x, ay + 11, 34, 34 };
        bool hov = ar.contains(g_mouse);
        if (hov) {
            prim::fillRR(ctx, ar.x, ar.y, ar.w, ar.h, 8.0f, br.solid(pal.card));
        }
        icons::Name n = (i == 0) ? icons::Name::Search : icons::Name::More;
        icons::drawIcon(app, n, ar.x + 8, ar.y + 8, 18,
                        hov ? pal.text : pal.text_muted);
        btn_x += 38;
    }

    // stream
    float comp_h = 64;
    float stream_y = ay + hdr_h;
    float stream_h = ah - hdr_h - comp_h;

    // 用 PushAxisAlignedClip 保证消息溢出不画到 composer 上
    ctx->PushAxisAlignedClip(D2D1::RectF(ax, stream_y, ax + aw, stream_y + stream_h),
                             D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    auto& msgs = streamFor(g_active);
    if (msgs.empty()) {
        auto* empty_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(10.0f));
        prim::drawText_(ctx,
            ch->is_market ? L"市场频道 — 切到 Market 标签查看商品" :
                            L"还没消息。说点什么吧～",
            empty_fmt,
            ax, stream_y + stream_h * 0.5f - 12, aw, 24,
            br.solid(pal.text_muted),
            DWRITE_TEXT_ALIGNMENT_CENTER);
        ctx->PopAxisAlignedClip();
        // 写一下 scroll 状态避免 wheel 事件来时 g_scroll[g_active] 不存在
        auto& sc = g_scroll[g_active];
        sc.total_height = 0;
        sc.viewport_h = stream_h;
        sc.offset_from_bottom = 0;
        sc.initialized = true;
        // composer
        paintComposer(app, ax, ay + ah - comp_h, aw, comp_h);
        return;
    }
    float maxw = aw - 32;
    // ----- Pass 1：dry-run 测每条 bubble 高度 + 算 total -----
    std::vector<float> heights(msgs.size(), 0);
    float total = 0;
    for (size_t i = 0; i < msgs.size(); ++i) {
        const Msg& m = msgs[i];
        const Msg* prev = (i > 0) ? &msgs[i - 1] : nullptr;
        bool prev_same = prev
            && prev->kind == MsgKind::Text
            && m.kind == MsgKind::Text
            && prev->from == m.from
            && m.from != L"me";
        heights[i] = measureBubbleHeight(app, m, maxw, prev_same);
        total += heights[i];
    }
    // ----- 滚动状态 -----
    auto& sc = g_scroll[g_active];
    sc.total_height = total;
    sc.viewport_h = stream_h;
    if (!sc.initialized) {
        sc.offset_from_bottom = 0;
        sc.target_offset = 0;
        sc.initialized = true;
    }
    float max_off = (std::max)(0.0f, total - stream_h);
    if (sc.target_offset > max_off) sc.target_offset = max_off;
    if (sc.target_offset < 0) sc.target_offset = 0;
    // 拖动滚动条期间直接同步；否则平滑 lerp 到 target（每帧 18% 趋近 — 连贯但不软）
    if (g_scroll_drag.active && g_mouse_pressed) {
        // 鼠标 y 增加 = 滚动条下移 = offset 减少（更接近底部）
        float dy = (float)g_mouse.y - g_scroll_drag.anchor_mouse_y;
        float track_h = (std::max)(1.0f, g_scroll_drag.bar_track_h);
        float content_per_track = (g_scroll_drag.total_height - g_scroll_drag.viewport_h) / track_h;
        // bar 下移（dy>0）→ offset 减少（向更新消息靠近）
        float new_off = g_scroll_drag.anchor_offset - dy * content_per_track;
        if (new_off > max_off) new_off = max_off;
        if (new_off < 0) new_off = 0;
        sc.target_offset = new_off;
        sc.offset_from_bottom = new_off;
    } else {
        float diff = sc.target_offset - sc.offset_from_bottom;
        if (std::abs(diff) < 0.5f) sc.offset_from_bottom = sc.target_offset;
        else                       sc.offset_from_bottom += diff * 0.22f;
    }
    if (sc.offset_from_bottom > max_off) sc.offset_from_bottom = max_off;
    if (sc.offset_from_bottom < 0) sc.offset_from_bottom = 0;

    float my_top;
    if (total <= stream_h) {
        // 消息没把 viewport 填满 — 顶端开始，不滚
        my_top = stream_y + 12;
    } else {
        // total > viewport：offset_from_bottom 表示从底部往上滚了多少 px
        // offset = 0 → 锁底（最新消息在底部）→ my_top = stream_bottom - total
        // offset = max_off → 顶部（最早消息在顶部）→ my_top = stream_y
        my_top = stream_y + stream_h - total + sc.offset_from_bottom + 12;
    }

    // ----- Pass 2：实际画 + 注册 hit -----
    float my = my_top;
    for (size_t i = 0; i < msgs.size(); ++i) {
        const Msg& m = msgs[i];
        const Msg* prev = (i > 0) ? &msgs[i - 1] : nullptr;
        bool prev_same = prev
            && prev->kind == MsgKind::Text
            && m.kind == MsgKind::Text
            && prev->from == m.from
            && m.from != L"me";
        // 跳过完全在 viewport 之外的 bubble — 既省 D2D 也避免 hit 冲突
        if (my + heights[i] < stream_y || my > stream_y + stream_h) {
            my += heights[i];
            continue;
        }
        paintBubble(app, m, (int)i, ax + 16, my, maxw, prev_same);
        my += heights[i];
    }
    ctx->PopAxisAlignedClip();

    // 右侧滚动条 — 可拖动
    if (total > stream_h) {
        float bar_x = ax + aw - 8;
        float bar_w = 6;     // 加宽 4→6 让拖动更好命中
        float bar_track_y = stream_y + 4;
        float bar_track_h = stream_h - 8;
        float bar_h = (stream_h / total) * bar_track_h;
        if (bar_h < 28) bar_h = 28;
        float t_pos = (max_off > 0) ? (sc.offset_from_bottom / max_off) : 0;
        float bar_y = bar_track_y + (bar_track_h - bar_h) * (1.0f - t_pos);
        // track
        prim::fillRR(ctx, bar_x, bar_track_y, bar_w, bar_track_h, 3.0f,
                     br.solidA(pal.text, 0.05f));
        // thumb — hover/拖动时颜色加深
        LayoutRect bar_rect{ bar_x - 2, bar_y, bar_w + 4, bar_h };
        bool bar_hov = bar_rect.contains(g_mouse) || g_scroll_drag.active;
        prim::fillRR(ctx, bar_x, bar_y, bar_w, bar_h, 3.0f,
                     br.solidA(pal.text, bar_hov ? 0.50f : 0.30f));
        // 注册 thumb 拖动 hit
        float anchor_y = (float)g_mouse.y;
        float anchor_off = sc.offset_from_bottom;
        float track_h_capt = bar_track_h - bar_h;
        float total_capt = total;
        float vp_capt = stream_h;
        hit(bar_rect, [anchor_y, anchor_off, track_h_capt, total_capt, vp_capt](){
            g_scroll_drag.active = true;
            g_scroll_drag.anchor_mouse_y = anchor_y;
            g_scroll_drag.anchor_offset = anchor_off;
            g_scroll_drag.bar_track_h = track_h_capt;
            g_scroll_drag.total_height = total_capt;
            g_scroll_drag.viewport_h = vp_capt;
        }, true);
    }

    // composer
    paintComposer(app, ax, ay + ah - comp_h, aw, comp_h);
}

// ============== Picker ==============
// 200+ 常用 emoji — 不分类，按 group 排（Segoe UI Emoji 都能渲染）。picker 区域加滚动。
const wchar_t* kEmoji[] = {
    // 笑脸
    L"😀",L"😃",L"😄",L"😁",L"😆",L"😅",L"🤣",L"😂",L"🙂",L"🙃",
    L"😉",L"😊",L"😇",L"🥰",L"😍",L"🤩",L"😘",L"😗",L"😚",L"😙",
    L"😋",L"😛",L"😜",L"🤪",L"😝",L"🤑",L"🤗",L"🤭",L"🤫",L"🤔",
    L"🤐",L"🤨",L"😐",L"😑",L"😶",L"😏",L"😒",L"🙄",L"😬",L"🤥",
    L"😌",L"😔",L"😪",L"🤤",L"😴",L"😷",L"🤒",L"🤕",L"🤢",L"🤮",
    // 情绪
    L"🥳",L"😎",L"🤓",L"🧐",L"😕",L"😟",L"🙁",L"😮",L"😯",L"😲",
    L"😳",L"🥺",L"😦",L"😧",L"😨",L"😰",L"😥",L"😢",L"😭",L"😱",
    L"😖",L"😣",L"😞",L"😓",L"😩",L"😫",L"🥱",L"😤",L"😡",L"😠",
    L"🤬",L"😈",L"👿",L"💀",L"💩",L"🤡",L"👹",L"👺",L"👻",L"👽",
    L"👾",L"🤖",
    // 手势
    L"👍",L"👎",L"👊",L"✊",L"🤛",L"🤜",L"👏",L"🙌",L"👐",L"🤲",
    L"🤝",L"🙏",L"✌",L"🤞",L"🤟",L"🤘",L"🤙",L"👌",L"👈",L"👉",
    L"👆",L"👇",L"☝",L"✋",L"🤚",L"🖐",L"🖖",L"👋",L"💪",L"🦾",
    // 心
    L"❤",L"🧡",L"💛",L"💚",L"💙",L"💜",L"🖤",L"🤍",L"🤎",L"💔",
    L"❣",L"💕",L"💞",L"💓",L"💗",L"💖",L"💘",L"💝",L"💟",
    // 动作 / 标记
    L"💯",L"💢",L"💥",L"💫",L"💦",L"💨",L"💣",L"💬",L"💭",L"💤",
    L"🔥",L"🌟",L"⭐",L"✨",L"⚡",L"🌈",L"☀",L"🌙",L"☁",L"❄",
    // 物品 / 食物
    L"🎉",L"🎊",L"🎁",L"🎂",L"🍰",L"🍕",L"🍔",L"🍟",L"🌭",L"🍿",
    L"🍣",L"🍱",L"🍜",L"🍙",L"🍩",L"🍪",L"🍫",L"🍬",L"🍭",L"🍮",
    L"🥤",L"🍻",L"🍺",L"🍷",L"🍸",L"☕",L"🍵",L"🥛",
    // 动物
    L"🐶",L"🐱",L"🐭",L"🐹",L"🐰",L"🦊",L"🐻",L"🐼",L"🐨",L"🐯",
    L"🦁",L"🐮",L"🐷",L"🐸",L"🐵",L"🙈",L"🙉",L"🙊",L"🐒",L"🐔",
    L"🐧",L"🐤",L"🦆",L"🦅",L"🦉",L"🐺",L"🐗",
    // 游戏 / 运动
    L"🎮",L"🕹",L"🎯",L"🎲",L"🎴",L"♟",L"🎳",L"🎱",L"⚽",L"🏀",
    L"🏈",L"⚾",L"🎾",L"🏐",L"🏉",L"🚀",L"💎",L"🎵",L"🎶",L"🌸",
    L"🌹",L"🌺",L"🌻",L"🌷",L"🌴",L"🍀",
};

static void paintPicker(D2DApp& app, float anchor_x, float anchor_y) {
    if (!g_picker_open && g_picker_t.value() < 0.001f) return;
    float t = g_picker_t.value();
    if (t < 0.001f) return;

    const Palette& pal = palette();
    auto* ctx = app.ctx();
    auto& br = app.brushes();

    float pw = 380, ph = 340;          // 加宽给右上 3 个按钮腾位
    float px = anchor_x;
    float py = anchor_y - ph - 8;
    g_picker_origin_x = px; g_picker_origin_y = py;

    prim::drawShadow(ctx, br, px, py, pw, ph, 12.0f, pal.shadow_card_hover, t, 4.0f, 4);
    prim::fillRR(ctx, px, py, pw, ph, 12.0f, br.solidA(pal.card, t));
    prim::strokeRR(ctx, px, py, pw, ph, 12.0f, br.solidA(pal.divider, t));

    auto* tab_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(9.5f),
                                       DWRITE_FONT_WEIGHT_BOLD);
    auto* hint_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(8.0f));

    // ===== 顶部 seg：[表情] [表情包]，右边 [↥导入] [⇣导出] [+新建] =====
    float seg_y = py + 12;
    LayoutRect tab_em{ px + 14, seg_y, 60, 26 };
    LayoutRect tab_pk{ px + 14 + 60 + 6, seg_y, 80, 26 };
    bool em_act = (g_picker_tab == 0);
    // 滑块 — 跟随 active 动画
    float pill_x = px + 14 + g_top_seg_x.value();
    float pill_w = g_top_seg_w.started ? g_top_seg_w.value() : (em_act ? 60.0f : 80.0f);
    if (pill_w > 0)
        prim::fillRR(ctx, pill_x, seg_y, pill_w, 26, 6.0f,
                     br.solidA(pal.primary, 0.18f * t));
    prim::drawText_(ctx, L"表情", tab_fmt,
                    tab_em.x, tab_em.y + 5, tab_em.w, 18,
                    br.solidA(em_act ? pal.primary : pal.text_muted, t),
                    DWRITE_TEXT_ALIGNMENT_CENTER);
    hit(tab_em, [](){
        g_picker_tab = 0;
        retargetTopSeg();
    }, true);
    prim::drawText_(ctx, L"表情包", tab_fmt,
                    tab_pk.x, tab_pk.y + 5, tab_pk.w, 18,
                    br.solidA(g_picker_tab > 0 ? pal.primary : pal.text_muted, t),
                    DWRITE_TEXT_ALIGNMENT_CENTER);
    hit(tab_pk, [](){
        if (g_picker_tab == 0) g_picker_tab = 1;
        retargetTopSeg();
        retargetPackTab();
    }, true);

    // 决定当前 pack 状态（用于按钮 enable / 操作目标）
    auto& packs = sticker::g_packs;
    int active_pack = -1;     // -1 = 在 emoji tab 或没 pack
    if (g_picker_tab > 0) {
        active_pack = g_picker_tab - 1;
        if (active_pack >= (int)packs.size()) active_pack = 0;
    }
    std::string cur_pid;
    if (active_pack >= 0 && active_pack < (int)packs.size()) {
        cur_pid = packs[active_pack].id;
    }

    // ===== 右上 3 按钮 =====
    auto draw_btn = [&](float bx, float by, float bw, float bh,
                        const wchar_t* label, uint32_t color, bool primary,
                        std::function<void()> on_click) {
        LayoutRect r{ bx, by, bw, bh };
        bool hov = r.contains(g_mouse);
        if (primary) {
            prim::fillRR(ctx, bx, by, bw, bh, 6.0f,
                         br.solidA(color, t * (hov ? 1.0f : 0.85f)));
        } else {
            prim::fillRR(ctx, bx, by, bw, bh, 6.0f,
                         br.solidA(color, t * (hov ? 0.18f : 0.08f)));
        }
        prim::drawText_(ctx, label, hint_fmt,
                        bx, by + 5, bw, 16,
                        br.solidA(primary ? 0xFFFFFF : color, t),
                        DWRITE_TEXT_ALIGNMENT_CENTER);
        hit(r, std::move(on_click), true);
    };
    float bw_new = 56, bw_imp = 56, bw_exp = 56;
    float bgap = 6;
    float right_btn_y = seg_y;
    float bx_new = px + pw - 14 - bw_new;
    float bx_exp = bx_new - bgap - bw_exp;
    float bx_imp = bx_exp - bgap - bw_imp;
    // [↥ 导入]
    draw_btn(bx_imp, right_btn_y, bw_imp, 26, L"↥ 导入", pal.text, false, [cur_pid](){
        if (cur_pid.empty()) {
            PostMessageW(GetActiveWindow(), WM_APP + 41, 0, 0);
        } else {
            static std::string g_pending_import_pid;
            g_pending_import_pid = cur_pid;
            PostMessageW(GetActiveWindow(), WM_APP + 34,
                         (WPARAM)&g_pending_import_pid, 0);
        }
    });
    // [⇣ 导出]
    draw_btn(bx_exp, right_btn_y, bw_exp, 26, L"⇣ 导出", pal.text, false, [cur_pid](){
        if (cur_pid.empty()) {
            PostMessageW(GetActiveWindow(), WM_APP + 41, 0, 0);
        } else {
            sticker::exportPackToFolder(GetActiveWindow(), cur_pid);
        }
    });
    // [+ 新建]
    draw_btn(bx_new, right_btn_y, bw_new, 26, L"+ 新建", pal.primary, true, [](){
        g_picker_open = false;
        g_picker_t.start(g_picker_t.value(), 0, 0.18f, 0, curve::easeOutCubic);
        PostMessageW(GetActiveWindow(), WM_APP + 21, 0, 0);
    });

    if (g_picker_tab == 0) {
        // ===== 8 列 emoji grid + 垂直滚动 =====
        int cols = 8;
        int total_n = (int)(sizeof(kEmoji) / sizeof(kEmoji[0]));
        float cell = 38.0f;
        float grid_x = px + 14, grid_y = py + 50;
        // viewport：emoji 区高度 = picker 底部 - grid_y - 12 边距
        float view_h = (py + ph - 12) - grid_y;
        int rows_total = (total_n + cols - 1) / cols;
        float total_h = rows_total * cell;
        g_emoji_grid_h_last = view_h;
        g_emoji_total_h_last = total_h;
        // 钳 scroll
        float max_scroll = (std::max)(0.0f, total_h - view_h);
        if (g_emoji_scroll_y < 0) g_emoji_scroll_y = 0;
        if (g_emoji_scroll_y > max_scroll) g_emoji_scroll_y = max_scroll;

        // clip 到 emoji 区
        ctx->PushAxisAlignedClip(D2D1::RectF(grid_x, grid_y, grid_x + cols * cell, grid_y + view_h),
                                 D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        auto* em_fmt = app.texts().format(L"Segoe UI Emoji", ptToDip(16.0f));
        for (int i = 0; i < total_n; ++i) {
            int row = i / cols, col = i % cols;
            float ex = grid_x + col * cell;
            float ey = grid_y + row * cell - g_emoji_scroll_y;
            // 完全不可见的跳过
            if (ey + cell < grid_y) continue;
            if (ey > grid_y + view_h) break;
            LayoutRect cell_r{ ex, ey, cell, cell };
            bool hov = cell_r.contains(g_mouse);
            if (hov) {
                prim::fillRR(ctx, ex, ey, cell, cell, 6.0f,
                             br.solidA(pal.text, t * 0.06f));
            }
            prim::drawText_(ctx, kEmoji[i], em_fmt,
                            ex, ey + 4, cell, cell - 4,
                            br.solidA(pal.text, t),
                            DWRITE_TEXT_ALIGNMENT_CENTER);
            const wchar_t* e = kEmoji[i];
            hit(cell_r, [e](){
                std::wstring s = e;
                g_composer.replaceSelection(s);
                g_focus_composer = true;
                // 不自动关 picker — 用户可能要连续选
            }, true);
        }
        ctx->PopAxisAlignedClip();
        // 滚动条
        if (max_scroll > 0) {
            float bar_x = grid_x + cols * cell + 2;
            float bar_w = 4;
            float bar_top = grid_y + (g_emoji_scroll_y / total_h) * view_h;
            float bar_h_p = (view_h / total_h) * view_h;
            prim::fillRR(ctx, bar_x, grid_y, bar_w, view_h, 2.0f,
                         br.solidA(pal.text, t * 0.05f));
            prim::fillRR(ctx, bar_x, bar_top, bar_w, bar_h_p, 2.0f,
                         br.solidA(pal.text, t * 0.30f));
        }
    } else {
        // ===== 表情包面板 =====
        // pack 顶部 tab 行 — 横向滑动 + 拖拽排序
        float tab_y = py + 50;
        g_pack_tab_rects.clear();
        // pre-layout: 算每个 tab 的宽度
        std::vector<float> ws(packs.size(), 0);
        for (size_t i = 0; i < packs.size(); ++i) {
            const auto& p = packs[i];
            wchar_t buf[40]; swprintf_s(buf, L"%.10ls", p.name.c_str());
            float bw = measureW(app, buf, hint_fmt) + 16.0f;
            if (bw > 100.0f) bw = 100.0f;
            if (bw < 40.0f) bw = 40.0f;
            ws[i] = bw;
        }
        float gap_tab = 4.0f;
        std::vector<float> xs(packs.size(), 0);
        float bx = px + 14;
        for (size_t i = 0; i < packs.size(); ++i) {
            xs[i] = bx;
            bx += ws[i] + gap_tab;
        }
        // 在 g_pack_tab_rects 缓存所有 tab 位置（拖拽 / 命中）
        for (size_t i = 0; i < packs.size(); ++i) {
            g_pack_tab_rects.push_back({ {xs[i], tab_y, ws[i], 24}, (int)i });
        }
        // ---- 拖拽检测 + 实时重排 ----
        if (g_pack_drag.from >= 0 && g_mouse_pressed) {
            float dx = (float)g_mouse.x - g_pack_drag.start_x;
            if (!g_pack_drag.moved && std::abs(dx) > 6.0f) g_pack_drag.moved = true;
            if (g_pack_drag.moved) {
                // 找到鼠标所处的目标位置
                int target = g_pack_drag.from;
                for (size_t i = 0; i < packs.size(); ++i) {
                    if (g_mouse.x >= xs[i] && g_mouse.x <= xs[i] + ws[i]) {
                        target = (int)i;
                        break;
                    }
                }
                // 如果目标变了 → 立即在 g_packs 里 swap (本地立即响应；松手后云端保存)
                if (target != g_pack_drag.from
                    && target >= 0 && target < (int)packs.size()) {
                    std::lock_guard<std::mutex> lk(sticker::g_packs_mtx);
                    auto& v = sticker::g_packs;
                    if (g_pack_drag.from < (int)v.size() && target < (int)v.size()) {
                        sticker::Pack moving = std::move(v[g_pack_drag.from]);
                        v.erase(v.begin() + g_pack_drag.from);
                        v.insert(v.begin() + target, std::move(moving));
                        g_pack_drag.from = target;
                        g_picker_tab = 1 + target;
                        retargetPackTab();
                    }
                }
            }
        }
        // 滑块（active pill）— 用 tween 平滑
        if (!g_pack_tab_x.started && active_pack >= 0 && active_pack < (int)packs.size()) {
            g_pack_tab_x.start(xs[active_pack], xs[active_pack], 0.001f, 0, curve::easeOutQuint);
            g_pack_tab_w.start(ws[active_pack], ws[active_pack], 0.001f, 0, curve::easeOutQuint);
        }
        if (g_pack_tab_w.value() > 0.5f) {
            prim::fillRR(ctx, g_pack_tab_x.value(), tab_y,
                         g_pack_tab_w.value(), 24, 4.0f,
                         br.solidA(pal.primary, t * 0.18f));
        }
        // 画每个 tab
        for (size_t i = 0; i < packs.size(); ++i) {
            // 拖拽中：源 tab 跟随鼠标
            float draw_x = xs[i];
            if (g_pack_drag.from == (int)i && g_pack_drag.moved) {
                draw_x = g_mouse.x - g_pack_drag.anchor_dx;
                // 不画背景 — 用纯文字 + 半透明高亮
                prim::fillRR(ctx, draw_x, tab_y, ws[i], 24, 4.0f,
                             br.solidA(pal.primary, t * 0.30f));
            }
            wchar_t buf[40]; swprintf_s(buf, L"%.10ls", packs[i].name.c_str());
            bool pa = ((int)i == active_pack);
            uint32_t tcol = pa ? pal.primary : pal.text_muted;
            prim::drawText_(ctx, buf, hint_fmt,
                            draw_x + 4, tab_y + 5, ws[i] - 8, 16,
                            br.solidA(tcol, t),
                            DWRITE_TEXT_ALIGNMENT_CENTER);
            int idx = (int)i;
            // 注意 hit 的是 tab 实际位置（拖动时这个 hit 跟着移）— 让点击到拖到位置上
            LayoutRect r{ draw_x, tab_y, ws[i], 24 };
            float anchor_dx = g_mouse.x - xs[i];
            hit(r, [idx, anchor_dx](){
                // 如果不是拖动结束的 click（左键单击）就切 active
                if (g_pack_drag.from == idx && g_pack_drag.moved) return;
                g_picker_tab = 1 + idx;
                retargetPackTab();
            }, true);
        }

        // ===== 当前 pack 内容 =====
        if (active_pack < 0 || active_pack >= (int)packs.size()) return;
        const auto& cur_pack = packs[active_pack];
        // 创建人小标
        if (!cur_pack.creator_name.empty() || !cur_pack.is_owner) {
            std::wstring tip;
            if (cur_pack.is_owner) tip = L"我创建的";
            else if (!cur_pack.creator_name.empty()) tip = L"by " + cur_pack.creator_name;
            else tip = L"已安装";
            prim::drawText_(ctx, tip, hint_fmt,
                            px + 14, py + 78, pw - 28, 14,
                            br.solidA(pal.text_faint, t));
        }
        if (cur_pack.stickers.empty()) {
            prim::drawText_(ctx,
                cur_pack.name == L"系统 emoji"
                    ? L"切到 表情 标签" : L"还没贴纸 — 拖文件 / 上传 / 安装",
                hint_fmt,
                px + 14, py + 130, pw - 28, 18,
                br.solidA(pal.text_muted, t),
                DWRITE_TEXT_ALIGNMENT_CENTER);
        } else {
            int cols = 5;
            float cell = 60.0f;
            float gx = px + 14, gy = py + 96;
            for (size_t i = 0; i < cur_pack.stickers.size(); ++i) {
                int row = (int)(i / cols), col = (int)(i % cols);
                float ex = gx + col * (cell + 4), ey = gy + row * (cell + 4);
                if (ey + cell > py + ph - 44) break;
                LayoutRect sr{ ex, ey, cell, cell };
                bool sh_ = sr.contains(g_mouse);
                if (sh_) {
                    prim::fillRR(ctx, ex, ey, cell, cell, 6,
                                 br.solidA(pal.primary, t * 0.12f));
                }
                ID2D1Bitmap* sticker_bmp = nullptr;
                std::wstring sp = cur_pack.stickers[i];
                auto sd = sp.find_last_of(L'.');
                bool is_gif = (sd != std::wstring::npos
                               && (sp.substr(sd) == L".gif"
                                   || sp.substr(sd) == L".GIF"));
                if (is_gif) {
                    auto* sa = app.gifs().fromFile(sp);
                    if (sa) sticker_bmp = app.gifs().frameAt(sa, stages::g_time_in_stage);
                }
                if (!sticker_bmp) sticker_bmp = app.images().fromFile(sp);
                if (sticker_bmp) {
                    prim::pushLayerRR(ctx, app.factory(),
                                      ex + 4, ey + 4, cell - 8, cell - 8, 8.0f);
                    ctx->DrawBitmap(sticker_bmp,
                        D2D1::RectF(ex + 4, ey + 4, ex + cell - 4, ey + cell - 4),
                        t, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                    prim::popLayer(ctx);
                }
                std::wstring path = cur_pack.stickers[i];
                bool can_delete = cur_pack.is_owner;
                if (sh_ && can_delete) {
                    LayoutRect xb{ ex + cell - 18, ey + 2, 16, 16 };
                    bool xh = xb.contains(g_mouse);
                    prim::fillCircle(ctx, xb.x + 8, xb.y + 8, 8,
                                     br.solidA(0x000000, t * (xh ? 0.85f : 0.65f)));
                    icons::drawIcon(app, icons::Name::X, xb.x + 2, xb.y + 2, 12,
                                    fadeArgb(0xFFFFFFFF, t));
                    hit(sr, [path](){
                        Msg m;
                        auto sd2 = path.find_last_of(L'.');
                        bool is_g = (sd2 != std::wstring::npos
                                     && (path.substr(sd2) == L".gif"
                                         || path.substr(sd2) == L".GIF"));
                        m.kind = is_g ? MsgKind::Gif : MsgKind::Sticker;
                        m.from = L"me";
                        m.body = path;
                        m.time = L"now";
                        appendLocalMessage(std::move(m));
                        sendChatMessage(GetActiveWindow(), path,
                                        is_g ? "gif" : "sticker");
                        g_picker_open = false;
                        g_picker_t.start(g_picker_t.value(), 0, 0.18f, 0, curve::easeOutCubic);
                    }, true);
                    hit(xb, [path](){
                        static std::wstring g_pending_del;
                        g_pending_del = path;
                        PostMessageW(GetActiveWindow(), WM_APP + 40,
                                     (WPARAM)&g_pending_del, 0);
                    }, true);
                } else {
                    hit(sr, [path](){
                        Msg m;
                        auto sd2 = path.find_last_of(L'.');
                        bool is_g = (sd2 != std::wstring::npos
                                     && (path.substr(sd2) == L".gif"
                                         || path.substr(sd2) == L".GIF"));
                        m.kind = is_g ? MsgKind::Gif : MsgKind::Sticker;
                        m.from = L"me";
                        m.body = path;
                        m.time = L"now";
                        appendLocalMessage(std::move(m));
                        sendChatMessage(GetActiveWindow(), path,
                                        is_g ? "gif" : "sticker");
                        g_picker_open = false;
                        g_picker_t.start(g_picker_t.value(), 0, 0.18f, 0, curve::easeOutCubic);
                    }, true);
                }
            }
        }

        // ===== 操作行：[复制分享链接] [重命名(仅 owner)] [删除(仅 owner)] =====
        if (!cur_pack.is_system && !cur_pack.id.empty()) {
            float oy = py + ph - 36;
            std::string pid = cur_pack.id;
            std::wstring pname = cur_pack.name;
            bool is_owner = cur_pack.is_owner;

            // 分享：永远是「复制分享链接」按钮，点击 → sharePack(pid, true) → 自动复制
            LayoutRect sb{ px + 14, oy, 110, 24 };
            bool s_h = sb.contains(g_mouse);
            prim::fillRR(ctx, sb.x, sb.y, sb.w, sb.h, 4,
                         br.solidA(pal.primary, t * (s_h ? 0.30f : 0.15f)));
            prim::drawText_(ctx, L"⧉ 复制分享链接", hint_fmt,
                            sb.x, sb.y + 5, sb.w, 16,
                            br.solidA(pal.primary, t),
                            DWRITE_TEXT_ALIGNMENT_CENTER);
            hit(sb, [pid](){
                sticker::sharePack(GetActiveWindow(), pid, true);
            }, true);

            float bx2 = px + 14 + 110 + 6;
            if (is_owner) {
                LayoutRect rb{ bx2, oy, 64, 24 };
                bool rh = rb.contains(g_mouse);
                prim::fillRR(ctx, rb.x, rb.y, rb.w, rb.h, 4,
                             br.solidA(pal.text, t * (rh ? 0.10f : 0.05f)));
                prim::drawText_(ctx, L"重命名", hint_fmt,
                                rb.x, rb.y + 5, rb.w, 16,
                                br.solidA(pal.text, t),
                                DWRITE_TEXT_ALIGNMENT_CENTER);
                hit(rb, [pid, pname](){
                    g_picker_open = false;
                    g_picker_t.start(g_picker_t.value(), 0, 0.18f, 0, curve::easeOutCubic);
                    static std::pair<std::string, std::wstring> g_pending;
                    g_pending = { pid, pname };
                    PostMessageW(GetActiveWindow(), WM_APP + 31,
                                 (WPARAM)&g_pending.first, (LPARAM)&g_pending.second);
                }, true);
                bx2 += 64 + 6;
            }
            // 删除：owner = 删自己创建的；非 owner = 卸载（uninstall）
            const wchar_t* del_lbl = is_owner ? L"删除" : L"卸载";
            LayoutRect db{ bx2, oy, 64, 24 };
            bool dh = db.contains(g_mouse);
            prim::fillRR(ctx, db.x, db.y, db.w, db.h, 4,
                         br.solidA(0xE34B4B, t * (dh ? 0.18f : 0.08f)));
            prim::drawText_(ctx, del_lbl, hint_fmt,
                            db.x, db.y + 5, db.w, 16,
                            br.solidA(0xE34B4B, t),
                            DWRITE_TEXT_ALIGNMENT_CENTER);
            hit(db, [pid, pname, is_owner](){
                static std::pair<std::string, std::wstring> g_pending_del;
                g_pending_del = { pid, pname };
                if (is_owner) {
                    PostMessageW(GetActiveWindow(), WM_APP + 32,
                                 (WPARAM)&g_pending_del.first, (LPARAM)&g_pending_del.second);
                } else {
                    PostMessageW(GetActiveWindow(), WM_APP + 51,
                                 (WPARAM)&g_pending_del.first, (LPARAM)&g_pending_del.second);
                }
            }, true);
        }
    }
}

void paintChatView(D2DApp& app, float ax, float ay, float aw, float ah) {
    float lw = 240.0f;
    paintChatList(app, ax, ay, lw, ah);
    paintChatPane(app, ax + lw + 1, ay, aw - lw - 1, ah);

    // picker 在 composer 上面浮起
    float comp_h = 64;
    paintPicker(app, ax + lw + 1 + 14, ay + ah - comp_h);
}

// ============== 事件 ==============
bool onMouseLDown(HWND /*hwnd*/, POINT dip) {
    // pack tab 拖拽起点 — 在 picker 打开 + 表情包 tab + 命中某个 tab 时初始化拖动状态
    g_pack_drag = PackDrag{};
    if (g_picker_open && g_picker_tab > 0) {
        for (auto& pt : g_pack_tab_rects) {
            if (pt.r.contains(dip)) {
                g_pack_drag.from = pt.idx;
                g_pack_drag.over = pt.idx;
                g_pack_drag.anchor_dx = (float)(dip.x - pt.r.x);
                g_pack_drag.start_x = (float)dip.x;
                g_pack_drag.moved = false;
                break;
            }
        }
    }
    bool consumed = dispatchClick(dip);
    if (g_focus_composer && !g_composer.bounds.contains(dip)) {
        g_focus_composer = false;
    }
    if (g_picker_open && !consumed) {
        g_picker_open = false;
        g_picker_t.start(g_picker_t.value(), 0, 0.18f, 0, curve::easeOutCubic);
    }
    return consumed;
}

bool onMouseRDown(HWND hwnd, POINT dip) {
    // 优先：消息体右键 → 弹消息菜单（回复 / 复制 / 添加到表情）
    auto& msgs = streamFor(g_active);
    for (auto it = g_msg_hits.rbegin(); it != g_msg_hits.rend(); ++it) {
        if (it->rect.contains(dip)) {
            int idx = it->idx;
            if (idx < 0 || idx >= (int)msgs.size()) return false;
            // 跳过 system / day divider
            const Msg& m = msgs[idx];
            if (m.kind == MsgKind::System || m.kind == MsgKind::DayDivider) return false;
            struct Pl { POINT pt; int idx; };
            static Pl g_pending_msg_menu;
            g_pending_msg_menu = { dip, idx };
            PostMessageW(hwnd, WM_APP + 50, (WPARAM)&g_pending_msg_menu, 0);
            return true;
        }
    }
    // 其次：头像右键 → 看主页
    for (auto it = g_avatar_hits.rbegin(); it != g_avatar_hits.rend(); ++it) {
        if (it->rect.contains(dip)) {
            auto* p = new std::wstring(it->from);
            PostMessageW(hwnd, WM_APP + 37, 0, (LPARAM)p);
            return true;
        }
    }
    return false;
}

bool onMouseLUp(HWND /*hwnd*/, POINT /*dip*/) {
    // pack 拖拽抬起 — 真正提交顺序由 paintPicker 在拖动时即时本地排过；这里只发后端
    if (g_pack_drag.from >= 0 && g_pack_drag.moved) {
        std::vector<std::string> ids;
        {
            std::lock_guard<std::mutex> lk(sticker::g_packs_mtx);
            for (auto& p : sticker::g_packs) {
                if (!p.id.empty()) ids.push_back(p.id);
            }
        }
        sticker::reorderPacks(GetActiveWindow(), ids);
    }
    g_pack_drag = PackDrag{};
    g_scroll_drag.active = false;
    return false;
}

void onChar(HWND hwnd, wchar_t c, bool ctrl) {
    if (!g_focus_composer) return;
    g_composer.onChar(c, ctrl, hwnd);
}

void onKey(HWND hwnd, int vk, bool shift, bool ctrl) {
    if (!g_focus_composer) return;
    if (vk == VK_RETURN) {
        if (!g_composer.text.empty()) {
            Msg m;
            m.kind = MsgKind::Text;
            m.from = L"me";
            m.body = g_composer.text;
            m.time = L"now";
            appendLocalMessage(std::move(m));
            sendTextMessage(hwnd, g_composer.text);
            g_composer.text.clear();
            g_composer.cursor = 0;
            g_composer.clearSel();
        }
        return;
    }
    if (vk == VK_ESCAPE) { g_focus_composer = false; return; }
    g_composer.onKey(vk, shift, ctrl);
}

// ============== 启动后异步 fetch official channels ==============
namespace {
struct OfficialArg { HWND h; };
std::mutex g_official_mtx;
std::vector<std::pair<std::wstring, std::string>> g_pending_official;  // slug → uuid
}

void fetchOfficialChannels(HWND notify) {
    if (g_session_token.empty()) return;
    auto* a = new OfficialArg{ notify };
    CreateThread(nullptr, 0, [](LPVOID lp) -> DWORD {
        std::unique_ptr<OfficialArg> a((OfficialArg*)lp);
        std::string path = "/api/chat/official?session_token=" + g_session_token;
        std::wstring wpath(path.begin(), path.end());
        auto resp = net::request(L"GET", wpath.c_str(), {}, L"");
        if (!resp.ok()) return 0;
        // 简易解析 [{"id":"...","slug":"...","title":"...","group_label":"...","write_role":"..."}]
        std::vector<std::pair<std::wstring, std::string>> tmp;
        size_t p = 0;
        while (true) {
            auto open_brace = resp.body.find('{', p);
            if (open_brace == std::string::npos) break;
            auto close_brace = resp.body.find('}', open_brace);
            if (close_brace == std::string::npos) break;
            std::string obj = resp.body.substr(open_brace, close_brace - open_brace + 1);
            std::string id = net::jsonStr(obj, "id");
            std::string slug = net::jsonStr(obj, "slug");
            if (!id.empty() && !slug.empty()) {
                int n = MultiByteToWideChar(CP_UTF8, 0, slug.c_str(), -1, nullptr, 0);
                std::wstring wslug(n > 0 ? n - 1 : 0, 0);
                if (n > 0) MultiByteToWideChar(CP_UTF8, 0, slug.c_str(), -1, wslug.data(), n);
                tmp.emplace_back(std::move(wslug), id);
            }
            p = close_brace + 1;
        }
        {
            std::lock_guard<std::mutex> lk(g_official_mtx);
            g_pending_official = std::move(tmp);
        }
        PostMessageW(a->h, WM_APP + 5, 0, 0);
        return 0;
    }, a, 0, nullptr);
}

// 由 main thread WM_APP+5 调用
void applyOfficialResult() {
    std::lock_guard<std::mutex> lk(g_official_mtx);
    for (auto& [slug, id] : g_pending_official) {
        for (auto& c : g_channels) {
            if (slug == c.slug) { c.id = id; break; }
        }
    }
    g_pending_official.clear();
}

}  // namespace launcher::d2d::chat
