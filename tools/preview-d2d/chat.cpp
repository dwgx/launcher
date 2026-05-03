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

static std::unordered_map<std::wstring, std::vector<Msg>> g_streams;
static std::unordered_map<std::wstring, bool> g_group_collapsed;
static std::mutex g_streams_mtx;

// 头像 hit 表 — paintChatPane 帧首清空，paintBubble 填充，WM_RBUTTONDOWN 命中
struct AvatarHit { LayoutRect rect; std::wstring from; };
static std::vector<AvatarHit> g_avatar_hits;

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
    // 第一次切到这个频道 → 异步拉历史
    if (!g_history_loaded[slug]) {
        g_history_loaded[slug] = true;
        fetchHistory(GetActiveWindow(), slug);
    }
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
        // 简单解析 [{"kind":"text","from":"...","author":"...","body":"...","time":"..."}, ...]
        std::vector<Msg> msgs;
        size_t pos = 0;
        while (true) {
            auto ob = r.body.find('{', pos);
            if (ob == std::string::npos) break;
            auto cb = r.body.find('}', ob);
            if (cb == std::string::npos) break;
            std::string obj = r.body.substr(ob, cb - ob + 1);
            Msg m;
            std::string kind = net::jsonStr(obj, "kind");
            if (kind == "sticker") m.kind = MsgKind::Sticker;
            else if (kind == "image") m.kind = MsgKind::Image;
            else if (kind == "gif") m.kind = MsgKind::Gif;
            else if (kind == "video") m.kind = MsgKind::Video;
            else if (kind == "system") m.kind = MsgKind::System;
            else m.kind = MsgKind::Text;
            m.from = utf8wHist(net::jsonStr(obj, "from"));
            m.author = utf8wHist(net::jsonStr(obj, "author"));
            m.body = utf8wHist(net::jsonStr(obj, "body"));
            m.time = utf8wHist(net::jsonStr(obj, "time"));
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
        streamFor(g_active).push_back(std::move(m));
        return;
    }
    m.from = L"me";
    m.body = path;
    m.time = L"now";
    streamFor(g_active).push_back(std::move(m));
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

// ============== 单条气泡 ==============
static float paintBubble(D2DApp& app, const Msg& m, float x, float y, float maxw,
                         bool prev_same_author) {
    const Palette& pal = palette();
    auto* ctx = app.ctx();
    auto& br = app.brushes();

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

    // 头像 28×28 + 注册右键命中
    float ar = 14.0f;
    float ay = y + 4;
    if (!prev_same_author) {
        prim::fillCircle(ctx, x + ar, ay + ar, ar, br.solid(pal.primary));
        wchar_t initial[2] = { (wchar_t)towupper(m.from.empty() ? L'?' : m.from[0]), 0 };
        auto* init_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(8.5f),
                                            DWRITE_FONT_WEIGHT_BOLD);
        prim::drawText_(ctx, initial, init_fmt,
                        x, ay, ar * 2, ar * 2,
                        br.solid(0xFFFFFFFF),
                        DWRITE_TEXT_ALIGNMENT_CENTER,
                        DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        // 注册头像 hit (左键看主页 — me 自己除外；右键也看主页)
        if (m.from != L"me" && !m.from.empty()) {
            g_avatar_hits.push_back({ { x, ay, ar * 2, ar * 2 }, m.from });
            std::wstring fcopy = m.from;
            hit({ x, ay, ar * 2, ar * 2 }, [fcopy](){
                // 左键 → 看主页（PostMessage WM_APP+37）
                static std::wstring g_pending_peer;
                g_pending_peer = fcopy;
                PostMessageW(GetActiveWindow(), WM_APP + 37, 0,
                             (LPARAM)&g_pending_peer);
            }, true);
        }
    }
    float bub_x = x + ar * 2 + 10;
    float bub_y = y + (prev_same_author ? 0 : 22);
    if (!prev_same_author) {
        prim::drawText_(ctx,
                        m.author.empty() ? m.from.c_str() : m.author.c_str(),
                        author_fmt,
                        bub_x, y + 2, 200, 14,
                        br.solid(me ? pal.primary : pal.text));
        if (!m.time.empty()) {
            float aw_ = measureW(app, m.author.empty() ? m.from : m.author, author_fmt);
            prim::drawText_(ctx, m.time, time_fmt,
                            bub_x + aw_ + 8, y + 4, 80, 12,
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
        return (prev_same_author ? bub_h : bub_h + 22) + 6;
    }

    if (m.kind == MsgKind::Video) {
        float bub_w = 240, bub_h = 140;
        prim::fillRR(ctx, bub_x, bub_y, bub_w, bub_h, 12.0f,
                     br.solid(pal.surface));
        // ▶
        prim::fillCircle(ctx, bub_x + bub_w * 0.5f, bub_y + bub_h * 0.5f, 24,
                         br.solidA(0x000000, 0.55f));
        icons::Name play = icons::Name::Play;
        // 简单画三角
        auto* white = br.solid(0xFFFFFFFF);
        ctx->FillEllipse(D2D1::Ellipse(
            D2D1::Point2F(bub_x + bub_w * 0.5f, bub_y + bub_h * 0.5f), 4, 4), white);
        prim::drawText_(ctx, L"▶ 视频", body_fmt,
                        bub_x, bub_y + bub_h - 24, bub_w, 18,
                        br.solid(pal.text_muted),
                        DWRITE_TEXT_ALIGNMENT_CENTER);
        return (prev_same_author ? bub_h : bub_h + 22) + 6;
    }

    if (m.kind == MsgKind::Sticker) {
        float bub_w = 100, bub_h = 100;
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
            // sticker 圆角更大
            prim::pushLayerRR(ctx, app.factory(), bub_x, bub_y, bub_w, bub_h, 16.0f);
            ctx->DrawBitmap(sbmp, D2D1::RectF(bub_x, bub_y, bub_x + bub_w, bub_y + bub_h),
                            1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            prim::popLayer(ctx);
        } else {
            prim::fillRR(ctx, bub_x, bub_y, bub_w, bub_h, 16.0f,
                         br.solid(pal.surface));
        }
        return (prev_same_author ? bub_h : bub_h + 22) + 6;
    }

    // ---------- Text bubble ----------
    float bub_max_w = (std::min)(maxw * 0.65f, 480.0f);
    DWRITE_TEXT_METRICS tm{};
    app.texts().measure(body_fmt, m.body, bub_max_w - 28, 8192, &tm);
    float bub_w = tm.width + 28;
    float bub_h = (std::max)(tm.height + 18, 28.0f);

    uint32_t bub_bg = me ? pal.primary : pal.card;
    uint32_t bub_fg = me ? 0xFFFFFFFF : pal.text;
    prim::fillRR(ctx, bub_x, bub_y, bub_w, bub_h, 12.0f,
                 br.solid(bub_bg));
    prim::drawText_(ctx, m.body, body_fmt,
                    bub_x + 14, bub_y + 8, bub_w - 28, bub_h - 16,
                    br.solid(bub_fg));

    return (prev_same_author ? bub_h : bub_h + 22) + 6;
}

// ============== 异步发消息 ==============
namespace {
struct SendArg {
    std::string session_token;
    std::string chat_id;
    std::string kind;          // text / sticker / image / gif
    std::wstring body;
    HWND hwnd;
};
}

static void sendChatMessage(HWND hwnd, const std::wstring& body, const char* kind = "text") {
    if (g_session_token.empty()) return;
    auto* ch = activeChannel();
    if (ch->id.empty()) return;     // 还没拿到 backend uuid
    auto* a = new SendArg{ g_session_token, ch->id, kind, body, hwnd };
    CreateThread(nullptr, 0, [](LPVOID lp) -> DWORD {
        std::unique_ptr<SendArg> a((SendArg*)lp);
        std::string b = "{\"session_token\":\"" + a->session_token
                      + "\",\"chat_id\":\"" + a->chat_id
                      + "\",\"kind\":\"" + a->kind
                      + "\",\"text\":\"" + net::jsonEscape(a->body) + "\"}";
        net::postJson(L"/api/chat/send", b);
        return 0;
    }, a, 0, nullptr);
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
            // 1. 本地立即显示
            auto& s = streamFor(g_active);
            Msg m;
            m.kind = MsgKind::Text;
            m.from = L"me";
            m.author = L"";
            m.status = L"online";
            m.body = g_composer.text;
            m.time = L"now";
            s.push_back(std::move(m));
            // 2. 真发后端（如果有 session）
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
    }
    float my = stream_y + 12;
    float maxw = aw - 32;
    for (size_t i = 0; i < msgs.size(); ++i) {
        const Msg& m = msgs[i];
        const Msg* prev = (i > 0) ? &msgs[i - 1] : nullptr;
        bool prev_same = prev
            && prev->kind == MsgKind::Text
            && m.kind == MsgKind::Text
            && prev->from == m.from
            && m.from != L"me";
        if (my > stream_y + stream_h) break;
        my += paintBubble(app, m, ax + 16, my, maxw, prev_same);
    }
    ctx->PopAxisAlignedClip();

    // composer
    paintComposer(app, ax, ay + ah - comp_h, aw, comp_h);
}

// ============== Picker ==============
const wchar_t* kEmoji[] = {
    L"😀",L"😁",L"😂",L"🤣",L"😄",L"😅",L"😉",L"😊",
    L"😎",L"😍",L"🥰",L"🙃",L"🙂",L"🤩",L"🤔",L"😐",
    L"😴",L"😌",L"😜",L"🤪",L"🥳",L"🥺",L"😢",L"😭",
    L"💀",L"👻",L"🤖",L"👍",L"👎",L"👏",L"🙏",L"💪",
    L"🔥",L"💯",L"🎮",L"🍣",L"🌸",L"⭐",L"🚀",L"💖",
};

static void paintPicker(D2DApp& app, float anchor_x, float anchor_y) {
    if (!g_picker_open && g_picker_t.value() < 0.001f) return;
    float t = g_picker_t.value();
    if (t < 0.001f) return;

    const Palette& pal = palette();
    auto* ctx = app.ctx();
    auto& br = app.brushes();

    float pw = 360, ph = 320;
    float px = anchor_x;
    float py = anchor_y - ph - 8;

    prim::drawShadow(ctx, br, px, py, pw, ph, 12.0f, pal.shadow_card_hover, t, 4.0f, 4);
    prim::fillRR(ctx, px, py, pw, ph, 12.0f, br.solidA(pal.card, t));
    prim::strokeRR(ctx, px, py, pw, ph, 12.0f, br.solidA(pal.divider, t));

    // 顶部 tab：表情 / 表情包
    auto* tab_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(9.5f),
                                       DWRITE_FONT_WEIGHT_BOLD);
    auto* hint_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(8.0f));
    LayoutRect tab_em{ px + 14, py + 12, 60, 26 };
    LayoutRect tab_pk{ px + 80, py + 12, 80, 26 };
    bool em_act = (g_picker_tab == 0);
    if (em_act) {
        prim::fillRR(ctx, tab_em.x, tab_em.y, tab_em.w, tab_em.h, 6.0f,
                     br.solidA(pal.primary, 0.18f * t));
    }
    prim::drawText_(ctx, L"表情", tab_fmt,
                    tab_em.x, tab_em.y + 5, tab_em.w, 18,
                    br.solidA(em_act ? pal.primary : pal.text_muted, t),
                    DWRITE_TEXT_ALIGNMENT_CENTER);
    hit(tab_em, [](){ g_picker_tab = 0; }, true);

    if (!em_act) {
        prim::fillRR(ctx, tab_pk.x, tab_pk.y, tab_pk.w, tab_pk.h, 6.0f,
                     br.solidA(pal.primary, 0.18f * t));
    }
    prim::drawText_(ctx, L"表情包", tab_fmt,
                    tab_pk.x, tab_pk.y + 5, tab_pk.w, 18,
                    br.solidA(g_picker_tab > 0 ? pal.primary : pal.text_muted, t),
                    DWRITE_TEXT_ALIGNMENT_CENTER);
    hit(tab_pk, [](){ g_picker_tab = 1; }, true);

    if (g_picker_tab == 0) {
        // 8 列 emoji grid
        int cols = 8;
        int total = (int)(sizeof(kEmoji) / sizeof(kEmoji[0]));
        float cell = 36.0f;
        float grid_x = px + 14, grid_y = py + 50;
        auto* em_fmt = app.texts().format(L"Segoe UI Emoji", ptToDip(16.0f));
        for (int i = 0; i < total; ++i) {
            int row = i / cols, col = i % cols;
            float ex = grid_x + col * cell, ey = grid_y + row * cell;
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
            }, true);
        }
    } else {
        // 表情包面板：tab head + 当前 pack stickers grid
        auto& packs = sticker::g_packs;
        // 顶部 pack tabs（横向滑动）
        float bx = px + 14;
        float by = py + 50;
        int active_pack = (g_picker_tab >= 1 && g_picker_tab - 1 < (int)packs.size())
            ? g_picker_tab - 1 : 0;
        if (active_pack >= (int)packs.size()) active_pack = 0;
        for (int i = 0; i < (int)packs.size() && i < 8; ++i) {
            const auto& p = packs[i];
            wchar_t buf[32];
            swprintf_s(buf, L"%.10ls", p.name.c_str());
            // bw 给充足宽度避免 DirectWrite 自动换行（之前 60 限太紧"系统 emoji"换行）
            float bw = measureW(app, buf, hint_fmt) + 16.0f;
            if (bw > 100.0f) bw = 100.0f;
            if (bw < 40.0f) bw = 40.0f;
            LayoutRect tab_r{ bx, by, bw, 24 };
            bool ph = tab_r.contains(g_mouse);
            bool pa = (i == active_pack);
            if (pa || ph) {
                prim::fillRR(ctx, bx, by, bw, 24, 4.0f,
                             br.solidA(pal.primary, t * (pa ? 0.18f : 0.08f)));
            }
            prim::drawText_(ctx, buf, hint_fmt,
                            bx + 4, by + 5, bw - 8, 16,
                            br.solidA(pa ? pal.primary : pal.text_muted, t),
                            DWRITE_TEXT_ALIGNMENT_CENTER);
            int idx = i;
            hit(tab_r, [idx](){ g_picker_tab = 1 + idx; }, true);
            bx += bw + 4;
            if (bx > px + pw - 80) break;
        }
        // 当前 pack id（用于导入到正确的 pack）
        std::string cur_pid;
        if (active_pack >= 0 && active_pack < (int)packs.size()) {
            cur_pid = packs[active_pack].id;
        }

        // + 新建
        LayoutRect newp{ px + pw - 64, py + 50, 50, 24 };
        bool nh = newp.contains(g_mouse);
        prim::fillRR(ctx, newp.x, newp.y, newp.w, newp.h, 4,
                     br.solidA(pal.primary, t * (nh ? 1.0f : 0.85f)));
        prim::drawText_(ctx, L"+ 新建", hint_fmt,
                        newp.x, newp.y + 5, newp.w, 16,
                        br.solidA(0xFFFFFF, t),
                        DWRITE_TEXT_ALIGNMENT_CENTER);
        hit(newp, [](){
            g_picker_open = false;
            g_picker_t.start(g_picker_t.value(), 0, 0.18f, 0, curve::easeOutCubic);
            PostMessageW(GetActiveWindow(), WM_APP + 21, 0, 0);
        }, true);

        // ↥ 导入文件夹 — 始终显示。"我的表情"(无 id) 时给提示
        {
            LayoutRect imp{ px + pw - 64 - 60, py + 50, 56, 24 };
            bool ih = imp.contains(g_mouse);
            prim::fillRR(ctx, imp.x, imp.y, imp.w, imp.h, 4,
                         br.solidA(pal.text, t * (ih ? 0.18f : 0.08f)));
            prim::drawText_(ctx, L"↥ 导入", hint_fmt,
                            imp.x, imp.y + 5, imp.w, 16,
                            br.solidA(pal.text, t),
                            DWRITE_TEXT_ALIGNMENT_CENTER);
            std::string pid = cur_pid;
            hit(imp, [pid](){
                if (pid.empty()) {
                    // "我的表情" 或系统 — 提示先选/建分组
                    PostMessageW(GetActiveWindow(), WM_APP + 41, 0, 0);
                } else {
                    static std::string g_pending_import_pid;
                    g_pending_import_pid = pid;
                    PostMessageW(GetActiveWindow(), WM_APP + 34,
                                 (WPARAM)&g_pending_import_pid, 0);
                }
            }, true);
        }

        // grid 5 列 sticker
        const auto& cur_pack = packs[active_pack];
        if (cur_pack.stickers.empty()) {
            prim::drawText_(ctx,
                cur_pack.name == L"系统 emoji"
                    ? L"切到 表情 标签" : L"还没贴纸 — 拖文件 / 上传 / 安装",
                hint_fmt,
                px + 14, py + 110, pw - 28, 18,
                br.solidA(pal.text_muted, t),
                DWRITE_TEXT_ALIGNMENT_CENTER);
        } else {
            int cols = 5;
            float cell = 60.0f;
            float gx = px + 14, gy = py + 84;
            for (size_t i = 0; i < cur_pack.stickers.size(); ++i) {
                int row = (int)(i / cols), col = (int)(i % cols);
                float ex = gx + col * (cell + 4), ey = gy + row * (cell + 4);
                if (ey + cell > py + ph - 8) break;
                LayoutRect sr{ ex, ey, cell, cell };
                bool sh_ = sr.contains(g_mouse);
                if (sh_) {
                    prim::fillRR(ctx, ex, ey, cell, cell, 6,
                                 br.solidA(pal.primary, t * 0.12f));
                }
                // sticker 也可能是 GIF — 优先 GifCache
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
                    // grid cell 圆角裁剪
                    prim::pushLayerRR(ctx, app.factory(),
                                      ex + 4, ey + 4, cell - 8, cell - 8, 8.0f);
                    ctx->DrawBitmap(sticker_bmp,
                        D2D1::RectF(ex + 4, ey + 4, ex + cell - 4, ey + cell - 4),
                        t, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                    prim::popLayer(ctx);
                }
                std::wstring path = cur_pack.stickers[i];
                // hover 时右上角 ✕ 删除按钮
                if (sh_) {
                    LayoutRect xb{ ex + cell - 18, ey + 2, 16, 16 };
                    bool xh = xb.contains(g_mouse);
                    prim::fillCircle(ctx, xb.x + 8, xb.y + 8, 8,
                                     br.solidA(0x000000, t * (xh ? 0.85f : 0.65f)));
                    icons::drawIcon(app, icons::Name::X, xb.x + 2, xb.y + 2, 12,
                                    fadeArgb(0xFFFFFFFF, t));
                    // ✕ 优先 hit（注册顺序：先 sticker，再 ✕，dispatch reverse 后 ✕ 优先）
                    hit(sr, [path](){
                        Msg m;
                        // GIF 路径用 Gif kind，其他用 Sticker
                        auto sd2 = path.find_last_of(L'.');
                        bool is_g = (sd2 != std::wstring::npos
                                     && (path.substr(sd2) == L".gif"
                                         || path.substr(sd2) == L".GIF"));
                        m.kind = is_g ? MsgKind::Gif : MsgKind::Sticker;
                        m.from = L"me";
                        m.body = path;
                        m.time = L"now";
                        streamFor(g_active).push_back(std::move(m));
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
                        streamFor(g_active).push_back(std::move(m));
                        sendChatMessage(GetActiveWindow(), path,
                                        is_g ? "gif" : "sticker");
                        g_picker_open = false;
                        g_picker_t.start(g_picker_t.value(), 0, 0.18f, 0, curve::easeOutCubic);
                    }, true);
                }
            }
        }

        // 操作行：分享 / 重命名 / 删除（仅非系统 + 有 id 时）
        if (active_pack > 0 && !cur_pack.id.empty()) {
            float oy = py + ph - 36;
            std::string pid = cur_pack.id;
            std::wstring pname = cur_pack.name;
            bool pub = cur_pack.is_public;

            LayoutRect rb{ px + 14, oy, 80, 24 };
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
                // PostMessage 给 main 让 main 调 modal::openRenamePack
                static std::pair<std::string, std::wstring> g_pending;
                g_pending = { pid, pname };
                PostMessageW(GetActiveWindow(), WM_APP + 31,
                             (WPARAM)&g_pending.first, (LPARAM)&g_pending.second);
            }, true);

            LayoutRect sb{ px + 100, oy, 80, 24 };
            bool s_h = sb.contains(g_mouse);
            prim::fillRR(ctx, sb.x, sb.y, sb.w, sb.h, 4,
                         br.solidA(pub ? 0x4ADE80 : pal.text, t * (s_h ? 0.18f : 0.08f)));
            prim::drawText_(ctx, pub ? L"已分享 ✓" : L"分享", hint_fmt,
                            sb.x, sb.y + 5, sb.w, 16,
                            br.solidA(pub ? 0x4ADE80 : pal.text, t),
                            DWRITE_TEXT_ALIGNMENT_CENTER);
            hit(sb, [pid, pub](){
                sticker::sharePack(GetActiveWindow(), pid, !pub);
            }, true);

            LayoutRect db{ px + 186, oy, 80, 24 };
            bool dh = db.contains(g_mouse);
            prim::fillRR(ctx, db.x, db.y, db.w, db.h, 4,
                         br.solidA(0xE34B4B, t * (dh ? 0.18f : 0.08f)));
            prim::drawText_(ctx, L"删除", hint_fmt,
                            db.x, db.y + 5, db.w, 16,
                            br.solidA(0xE34B4B, t),
                            DWRITE_TEXT_ALIGNMENT_CENTER);
            hit(db, [pid, pname](){
                static std::pair<std::string, std::wstring> g_pending_del;
                g_pending_del = { pid, pname };
                PostMessageW(GetActiveWindow(), WM_APP + 32,
                             (WPARAM)&g_pending_del.first, (LPARAM)&g_pending_del.second);
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
    bool consumed = dispatchClick(dip);
    // composer focus 自动 dismiss — 点 composer 之外（且未命中 hits）就 unfocus
    // bounds 检查 + composer hit 自己会重设 focus = true，所以这里只在没 consumed 时清
    if (g_focus_composer && !g_composer.bounds.contains(dip)) {
        g_focus_composer = false;
    }
    // picker 自动 dismiss — 点击没命中 picker 内部任何 hit (即 consumed=false 表示
    // 点击的是空白区域，picker 区域内的 hits 也会 consume)
    if (g_picker_open && !consumed) {
        g_picker_open = false;
        g_picker_t.start(g_picker_t.value(), 0, 0.18f, 0, curve::easeOutCubic);
    }
    return consumed;
}

bool onMouseRDown(HWND hwnd, POINT dip) {
    // 倒序找命中头像，命中就 PostMessage WM_APP+37 with std::wstring* from
    for (auto it = g_avatar_hits.rbegin(); it != g_avatar_hits.rend(); ++it) {
        if (it->rect.contains(dip)) {
            auto* p = new std::wstring(it->from);
            PostMessageW(hwnd, WM_APP + 37, 0, (LPARAM)p);
            return true;
        }
    }
    return false;
}

void onChar(HWND hwnd, wchar_t c, bool ctrl) {
    if (!g_focus_composer) return;
    g_composer.onChar(c, ctrl, hwnd);
}

void onKey(HWND hwnd, int vk, bool shift, bool ctrl) {
    if (!g_focus_composer) return;
    if (vk == VK_RETURN) {
        // Enter 发送
        if (!g_composer.text.empty()) {
            auto& s = streamFor(g_active);
            Msg m;
            m.kind = MsgKind::Text;
            m.from = L"me";
            m.body = g_composer.text;
            m.time = L"now";
            s.push_back(std::move(m));
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
