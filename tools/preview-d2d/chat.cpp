// Chat view 实现 — 见 chat.h。GDI+ 等价 tools/preview/chat_view.inl。

#include "chat.h"
#include "icons.h"
#include "palette.h"
#include "user_state.h"
#include "net.h"
#include "hit.h"
#include "stages.h"
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

static std::unordered_map<std::wstring, std::vector<Msg>> g_streams;
static std::unordered_map<std::wstring, bool> g_group_collapsed;
static std::mutex g_streams_mtx;

std::vector<Msg>& streamFor(const std::wstring& slug) {
    auto it = g_streams.find(slug);
    if (it == g_streams.end()) {
        it = g_streams.emplace(slug, std::vector<Msg>{}).first;
    }
    return it->second;
}

void switchChannel(const std::wstring& slug) {
    g_active = slug;
    g_focus_composer = false;
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

void tick(float /*dt*/) {
    // typing dots / unread badge anim 等留 Step polish
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

    // 文字 wrap 宽度
    float bub_max_w = (std::min)(maxw * 0.65f, 480.0f);
    DWRITE_TEXT_METRICS tm{};
    app.texts().measure(body_fmt, m.body, bub_max_w - 28, 8192, &tm);
    float bub_w = tm.width + 28;
    float bub_h = (std::max)(tm.height + 18, 28.0f);

    // 头像 28×28（占位 — 后续接通真头像）
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
    }

    float bub_x = x + ar * 2 + 10;
    float bub_y = y + (prev_same_author ? 0 : 22);

    // 作者 + 时间（同 author 连续消息不重复显示）
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

    // 气泡 bg
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
    std::wstring text;
    HWND hwnd;
};
}

static void sendTextMessage(HWND hwnd, const std::wstring& text) {
    if (g_session_token.empty()) return;
    auto* ch = activeChannel();
    if (ch->id.empty()) return;     // 还没拿到 backend uuid
    auto* a = new SendArg{ g_session_token, ch->id, text, hwnd };
    CreateThread(nullptr, 0, [](LPVOID lp) -> DWORD {
        std::unique_ptr<SendArg> a((SendArg*)lp);
        std::string body = "{\"session_token\":\"" + a->session_token
                         + "\",\"chat_id\":\"" + a->chat_id
                         + "\",\"kind\":\"text\",\"text\":\"" + net::jsonEscape(a->text) + "\"}";
        net::postJson(L"/api/chat/send", body);
        return 0;
    }, a, 0, nullptr);
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
        // emoji picker 留下一轮（GDI+ 那边复杂）
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

void paintChatView(D2DApp& app, float ax, float ay, float aw, float ah) {
    float lw = 240.0f;
    paintChatList(app, ax, ay, lw, ah);
    paintChatPane(app, ax + lw + 1, ay, aw - lw - 1, ah);
}

// ============== 事件 ==============
bool onMouseLDown(HWND /*hwnd*/, POINT dip) {
    return dispatchClick(dip);
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
