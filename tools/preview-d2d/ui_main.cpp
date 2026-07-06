// Main view 容器 + 各 view 渲染 — 1:1 复刻 GDI+ Preview 主框架。
//
// Topbar / Sidebar / AccountDropdown 严格按 styles.css token 实现；各 view 内容简化但
// 视觉骨架对齐 GDI+ Preview。Chat 和 Modals 在独立文件。

#include "ui_main.h"
#include "hwid.h"
#include "icons.h"
#include "palette.h"
#include "user_state.h"
#include "auth.h"
#include "hit.h"
#include "chat.h"
#include "modals.h"
#include "fetch.h"
#include "persist.h"
#include "toast.h"
#include "ws_user.h"
#include "i18n.h"
#include "game_assets.h"
#include "webview.h"
#include "render/primitives.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <commdlg.h>
#include <ShlObj.h>
#include <wrl/client.h>

#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")

namespace launcher::d2d::ui {

bool   g_account_dropdown = false;
bool   g_status_fold_open = false;
Tween  g_dropdown_t;
Tween  g_status_fold_t;
Tween  g_seg_lang_x, g_seg_lang_w, g_seg_theme_x, g_seg_theme_w;

namespace {

struct MenuEntry { stages::View view; const char* label_key; icons::Name icon; };
constexpr MenuEntry kMenu[] = {
    { stages::View::Home,     "menu.home",     icons::Name::Home },
    { stages::View::Lunching, "menu.lunching", icons::Name::Library },
    { stages::View::Chat,     "menu.chat",     icons::Name::Chat },
    { stages::View::Market,   "menu.market",   icons::Name::Cart },
    { stages::View::Cloud,    "menu.cloud",    icons::Name::Cloud },
    { stages::View::Settings, "menu.settings", icons::Name::Settings },
};

std::wstring lowerExt(const std::wstring& path) {
    auto dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos) return L"";
    std::wstring ext = path.substr(dot);
    for (auto& c : ext) {
        if (c >= L'A' && c <= L'Z') c = (wchar_t)(c - L'A' + L'a');
    }
    return ext;
}

bool supportedAvatarExt(const std::wstring& path) {
    std::wstring ext = lowerExt(path);
    return ext == L".png" || ext == L".jpg" || ext == L".jpeg" || ext == L".gif";
}

float measureW(D2DApp& app, std::wstring_view s, IDWriteTextFormat* fmt) {
    if (s.empty() || !fmt) return 0.0f;
    DWRITE_TEXT_METRICS m{};
    if (!app.texts().measure(fmt, s, 8192.0f, 256.0f, &m)) return 0.0f;
    return m.width;
}

std::wstring fitText(D2DApp& app, const std::wstring& s,
                     IDWriteTextFormat* fmt, float max_w) {
    if (s.empty() || max_w <= 4.0f || measureW(app, s, fmt) <= max_w) return s;
    const std::wstring ell = L"…";
    float ell_w = measureW(app, ell, fmt);
    if (ell_w >= max_w) return ell;
    size_t lo = 0, hi = s.size();
    while (lo < hi) {
        size_t mid = (lo + hi + 1) / 2;
        std::wstring candidate = s.substr(0, mid) + ell;
        if (measureW(app, candidate, fmt) <= max_w) lo = mid;
        else hi = mid - 1;
    }
    return s.substr(0, lo) + ell;
}

bool drawCoverCircle(D2DApp& app, ID2D1Bitmap* bmp,
                     float x, float y, float r, float opacity) {
    if (!bmp) return false;
    auto* ctx = app.ctx();
    D2D1_SIZE_F sz = bmp->GetSize();
    if (sz.width <= 0 || sz.height <= 0) return false;
    float dest = r * 2.0f;
    float scale = (std::max)(dest / sz.width, dest / sz.height);
    float draw_w = sz.width * scale;
    float draw_h = sz.height * scale;
    float dx = x + (dest - draw_w) * 0.5f;
    float dy = y + (dest - draw_h) * 0.5f;
    D2D1_BITMAP_BRUSH_PROPERTIES bp = D2D1::BitmapBrushProperties(
        D2D1_EXTEND_MODE_CLAMP, D2D1_EXTEND_MODE_CLAMP,
        D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    Microsoft::WRL::ComPtr<ID2D1BitmapBrush> bb;
    if (FAILED(ctx->CreateBitmapBrush(bmp, bp, &bb))) return false;
    auto mt = D2D1::Matrix3x2F::Scale({scale, scale}, {0, 0})
            * D2D1::Matrix3x2F::Translation(dx, dy);
    bb->SetTransform(mt);
    bb->SetOpacity(opacity);
    ctx->FillEllipse(D2D1::Ellipse({x + r, y + r}, r, r), bb.Get());
    return true;
}

// fade(c, op): apply alpha multiplier to ARGB hex
inline uint32_t fadeArgb(uint32_t argb, float op) {
    uint32_t a = (argb >> 24) & 0xFFu;
    a = (uint32_t)(a * op + 0.5f);
    if (a > 255) a = 255;
    return (a << 24) | (argb & 0xFFFFFFu);
}

void drawAvatarPill(D2DApp& app, float ax, float ay, float ar, float op) {
    auto* ctx = app.ctx();
    auto& br = app.brushes();
    const Palette& pal = palette();

    if (!g_avatar_path.empty()) {
        auto* bmp = app.images().fromFile(g_avatar_path, (uint32_t)(ar * 2.0f + 0.5f));
        if (bmp) {
            if (drawCoverCircle(app, bmp, ax, ay, ar, op)) return;
        }
    }
    // 占位：主色圆 + 首字母
    prim::fillCircle(ctx, ax + ar, ay + ar, ar, br.solidA(pal.primary, op));
    auto* fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(ar > 18.0f ? 14.0f : 8.5f),
                                   DWRITE_FONT_WEIGHT_BOLD);
    wchar_t init[2] = { (wchar_t)towupper(g_user.nickname.empty() ? L'?' : g_user.nickname[0]), 0 };
    prim::drawText_(ctx, init, fmt, ax, ay, ar * 2, ar * 2,
                    br.solidA(0xFFFFFF, op),
                    DWRITE_TEXT_ALIGNMENT_CENTER,
                    DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
}

void paintTopbar(D2DApp& app, float W) {
    const Palette& pal = palette();
    auto* ctx = app.ctx();
    auto& br = app.brushes();
    float ty = -kTopbarH * (1.0f - stages::g_topbar_y.value());

    prim::fillRect(ctx, 0.0f, ty, W, kTopbarH, br.solid(pal.bg));
    prim::drawLine(ctx, 0.0f, ty + kTopbarH, W, ty + kTopbarH,
                   br.solid(pal.divider), 1.0f);

    auto* title_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(10.0f),
                                         DWRITE_FONT_WEIGHT_NORMAL);
    prim::drawText_(ctx, L"Launcher", title_fmt,
                    20.0f, ty + 14.0f, 200.0f, 24.0f,
                    br.solid(pal.text));

    // 右上 user-trigger pill
    float pill_h = 32.0f;
    float ar = 12.0f;
    float pill_pad_l = 12.0f, pill_pad_r = 4.0f, pill_gap = 10.0f;

    auto* nick_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(10.0f));
    std::wstring top_name = fitText(app, g_user.nickname, nick_fmt, 160.0f);
    float name_w = measureW(app, top_name, nick_fmt) + 4.0f;
    float pill_w = pill_pad_l + name_w + pill_gap + ar * 2 + pill_pad_r;
    float pill_x = W - 16.0f - pill_w;
    float pill_y = ty + (kTopbarH - pill_h) * 0.5f;

    bool pill_hover = LayoutRect{ pill_x, pill_y, pill_w, pill_h }.contains(g_mouse);
    if (pill_hover) {
        prim::fillRR(ctx, pill_x, pill_y, pill_w, pill_h, 16.0f,
                     br.solidA(pal.text, 0.04f));
    }
    prim::drawText_(ctx, top_name, nick_fmt,
                    pill_x + pill_pad_l, pill_y + 8.0f, name_w, 16.0f,
                    br.solid(pal.text_muted));

    float ax = pill_x + pill_pad_l + name_w + pill_gap;
    float ay = pill_y + (pill_h - ar * 2) * 0.5f;
    drawAvatarPill(app, ax, ay, ar, 1.0f);

    // 状态 dot + 描边环
    uint32_t st = statusColor(g_status);
    prim::fillCircle(ctx, ax + ar * 2 - 3.5f, ay + ar * 2 - 3.5f, 3.5f,
                     br.solid(st));
    prim::strokeCircle(ctx, ax + ar * 2 - 3.5f, ay + ar * 2 - 3.5f, 3.5f,
                       br.solid(pal.bg), 2.0f);

    // 整个 pill 区域 hit
    LayoutRect hit_area{ pill_x, ty, pill_w + 16.0f, kTopbarH };
    hit(hit_area, [](){
        g_account_dropdown = !g_account_dropdown;
        if (g_account_dropdown) g_dropdown_t.start(g_dropdown_t.value(), 1.0f, 0.18f, 0, curve::easeOutCubic);
        else                    g_dropdown_t.start(g_dropdown_t.value(), 0.0f, 0.15f, 0, curve::easeOutCubic);
    }, true);
}

void paintSidebar(D2DApp& app, float H) {
    const Palette& pal = palette();
    auto* ctx = app.ctx();
    auto& br = app.brushes();
    float sx = -kSidebarW * (1.0f - stages::g_sidebar_x.value());

    prim::fillRect(ctx, sx, kTopbarH, kSidebarW, H - kTopbarH, br.solid(pal.sidebar_bg));
    prim::drawLine(ctx, sx + kSidebarW, kTopbarH, sx + kSidebarW, H,
                   br.solid(pal.divider), 1.0f);

    auto* item_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(10.5f));

    float my = kTopbarH + 14.0f;
    for (auto& m : kMenu) {
        bool active = (m.view == stages::g_view);
        LayoutRect item{ sx + 10.0f, my, kSidebarW - 20.0f, 38.0f };
        bool hover = item.contains(g_mouse);

        if (hover && !active) {
            prim::fillRR(ctx, item.x, item.y, item.w, item.h, 8.0f,
                         br.solidA(pal.primary, 0.08f));
        }
        if (active) {
            // 左侧 3px 主色指示条
            prim::fillRR(ctx, sx + 1.0f, item.y + 9.0f, 3.0f, item.h - 18.0f, 1.5f,
                         br.solid(pal.primary));
        }
        uint32_t tc = active ? pal.primary : (hover ? pal.text : pal.text_muted);

        icons::drawIcon(app, m.icon, item.x + 12.0f, item.y + 9.0f, 20.0f, tc);
        std::wstring lbl = trW(m.label_key);
        prim::drawText_(ctx, lbl, item_fmt,
                        item.x + 44.0f, item.y + 11.0f, item.w - 50.0f, 18.0f,
                        br.solid(tc));
        if (m.view == stages::View::Chat && chat::hasUnreadAnnouncements()) {
            prim::fillCircle(ctx, item.x + item.w - 16.0f, item.y + 19.0f, 4.0f,
                             br.solid(pal.primary));
            prim::strokeCircle(ctx, item.x + item.w - 16.0f, item.y + 19.0f, 4.0f,
                               br.solid(pal.sidebar_bg), 1.5f);
        }

        stages::View target = m.view;
        hit(item, [target]() { switchView(target); }, true);
        my += 42.0f;
    }
}

void paintAccountDropdown(D2DApp& app, float W) {
    // modal 打开时强制关 dropdown — 避免两层浮窗叠加（用户图1 反馈）
    if (modal::anyOpen() && g_account_dropdown) {
        g_account_dropdown = false;
        g_status_fold_open = false;
        g_dropdown_t.start(g_dropdown_t.value(), 0.0f, 0.15f, 0, curve::easeOutCubic);
        g_status_fold_t.start(g_status_fold_t.value(), 0.0f, 0.15f, 0, curve::easeOutCubic);
    }
    if (!g_account_dropdown && g_dropdown_t.value() < 0.001f) return;
    const Palette& pal = palette();
    auto* ctx = app.ctx();
    auto& br = app.brushes();

    float t = g_dropdown_t.value();
    if (t < 0.001f) return;

    float dw = 240.0f;
    // dropdown 高度严格按内容算：
    //   8 padding + 50 header + 32 status trigger + 32 status_text 行
    //   + fold (4 行 × 24 + 8 分割) × ft
    //   + 4 items × 32 + 8 退出登录前分割线 + 8 bottom padding
    // 收起 fold = 266；展开 = 370
    float fold_part = g_status_fold_t.value() * (4 * 24.0f + 8.0f);
    float dh = 8 + 50 + 32 + 32 + fold_part + 4 * 32.0f + 8 + 8;
    float dx = W - 16.0f - dw;
    float dy = kTopbarH + 4.0f - 6.0f * (1.0f - t);

    // 阴影 + 卡身
    prim::drawShadow(ctx, br, dx, dy, dw, dh, 12.0f, pal.shadow_card, t, 4.0f, 2);
    prim::fillRR(ctx, dx, dy, dw, dh, 12.0f, br.solidA(pal.card, t));
    prim::strokeRR(ctx, dx, dy, dw, dh, 12.0f, br.solidA(pal.divider, t));

    // header — avatar + nickname + email
    float ar = 14.0f;
    drawAvatarPill(app, dx + 12, dy + 12, ar, t);

    auto* nick_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(10.5f),
                                        DWRITE_FONT_WEIGHT_BOLD);
    auto* email_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(7.5f));
    std::wstring dropdown_name = fitText(app, g_user.nickname, nick_fmt, dw - 64.0f);
    prim::drawText_(ctx, dropdown_name, nick_fmt,
                    dx + 50, dy + 12, dw - 60, 20,
                    br.solidA(pal.text, t));
    std::wstring dropdown_email = fitText(app, g_user.email, email_fmt, dw - 64.0f);
    prim::drawText_(ctx, dropdown_email, email_fmt,
                    dx + 50, dy + 28, dw - 60, 14,
                    br.solidA(pal.text_muted, t));

    prim::drawLine(ctx, dx + 8, dy + 50, dx + dw - 8, dy + 50,
                   br.solidA(pal.divider, t), 1.0f);

    // status fold trigger — 显当前状态 + 状态消息
    float iy = dy + 58;
    LayoutRect strigger{ dx + 6, iy, dw - 12, 30 };
    bool shov = strigger.contains(g_mouse);
    if (shov) {
        prim::fillRR(ctx, strigger.x, strigger.y, strigger.w, strigger.h, 6.0f,
                     br.solidA(pal.text, t * 0.08f));
    }
    prim::fillCircle(ctx, dx + 18.0f, iy + 16.0f, 4.0f,
                     br.solidA(statusColor(g_status), t));

    auto* st_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(9.0f));
    prim::drawText_(ctx, statusLabel(g_status), st_fmt,
                    dx + 30, iy + 8, dw - 60, 18,
                    br.solidA(pal.text, t));
    prim::drawText_(ctx, g_status_fold_open ? L"▴" : L"▾", st_fmt,
                    dx + dw - 22, iy + 8, 14, 18,
                    br.solidA(pal.text_muted, t));
    hit(strigger, [](){
        g_status_fold_open = !g_status_fold_open;
        g_status_fold_t.start(g_status_fold_t.value(),
                              g_status_fold_open ? 1.0f : 0.0f,
                              0.22f, 0, curve::easeOutCubic);
    }, true);
    iy += 32;

    // 状态消息行 — 点击编辑（48 字内自定义文字）
    LayoutRect status_text_row{ dx + 6, iy, dw - 12, 28 };
    bool stm_h = status_text_row.contains(g_mouse);
    if (stm_h) {
        prim::fillRR(ctx, status_text_row.x, status_text_row.y,
                     status_text_row.w, status_text_row.h, 6.0f,
                     br.solidA(pal.text, t * 0.06f));
    }
    auto* sm_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(8.5f));
    icons::drawIcon(app, icons::Name::Edit, dx + 14, iy + 6, 14,
                    fadeArgb(pal.text_muted, t));
    if (g_user.status_text.empty()) {
        std::wstring add = trW("acc.add_status");
        prim::drawText_(ctx, add, sm_fmt,
                        dx + 32, iy + 7, dw - 60, 16,
                        br.solidA(pal.text_faint, t));
    } else {
        std::wstring status_text = fitText(app, g_user.status_text, sm_fmt, dw - 68.0f);
        prim::drawText_(ctx, status_text, sm_fmt,
                        dx + 32, iy + 7, dw - 60, 16,
                        br.solidA(pal.text_muted, t));
    }
    hit(status_text_row, [](){
        modal::openEditStatusText();
        g_account_dropdown = false;
        g_dropdown_t.start(g_dropdown_t.value(), 0.0f, 0.15f, 0, curve::easeOutCubic);
    }, true);
    iy += 32;

    // status fold body
    if (g_status_fold_t.value() > 0.001f) {
        float ft = g_status_fold_t.value();
        UserStatus statuses[] = { UserStatus::Online, UserStatus::Busy,
                                  UserStatus::Away, UserStatus::Sleep, UserStatus::Offline };
        for (auto s : statuses) {
            if (s == g_status) continue;
            float row_h = 24.0f * ft;
            if (row_h < 4.0f) continue;
            LayoutRect r{ dx + 14, iy, dw - 28, row_h };
            bool hov = r.contains(g_mouse);
            if (hov) {
                prim::fillRR(ctx, r.x, r.y, r.w, r.h, 4.0f,
                             br.solidA(pal.text, t * ft * 0.08f));
            }
            prim::fillCircle(ctx, dx + 21.0f, iy + 8.0f * ft + 3.0f, 3.0f,
                             br.solidA(statusColor(s), t * ft));
            prim::drawText_(ctx, statusLabel(s), st_fmt,
                            dx + 32, iy + 4.0f * ft, dw - 60, 18,
                            br.solidA(pal.text, t * ft));
            UserStatus target = s;
            hit(r, [target]() {
                g_status = target;
                g_status_fold_open = false;
                g_status_fold_t.start(g_status_fold_t.value(), 0.0f, 0.18f, 0, curve::easeOutCubic);
                fetch::statusSync(statusKey(target));
            }, true);
            iy += 24.0f * ft;
        }
        if (ft > 0.5f) {
            prim::drawLine(ctx, dx + 8, iy + 4, dx + dw - 8, iy + 4,
                           br.solidA(pal.divider, t), 1.0f);
        }
        iy += 8;
    }

    struct Item { const char* label_key; icons::Name icon; std::function<void()> click; bool danger; };
    Item items[] = {
        { "acc.profile", icons::Name::User, [](){
            switchView(stages::View::Profile);
            g_account_dropdown = false;
            g_dropdown_t.start(g_dropdown_t.value(), 0.0f, 0.15f, 0, curve::easeOutCubic);
        }, false },
        { "acc.history", icons::Name::History, [](){
            modal::openHistory();
            g_account_dropdown = false;
            g_dropdown_t.start(g_dropdown_t.value(), 0.0f, 0.15f, 0, curve::easeOutCubic);
        }, false },
        { "acc.password", icons::Name::Shield, [](){
            modal::openChangePw();
            g_account_dropdown = false;
            g_dropdown_t.start(g_dropdown_t.value(), 0.0f, 0.15f, 0, curve::easeOutCubic);
        }, false },
        { "acc.signout", icons::Name::Logout, [](){
            modal::openConfirm(trW("logout.title"), trW("logout.confirm"),
                [](){
                    fetch::logout(g_session_token);
                    ws::stop();
                    persist::clearCreds();
                    g_session_token.clear();
                    g_user_id.clear();
                    auth::g_form.username.text.clear(); auth::g_form.username.cursor = 0;
                    auth::g_form.password.text.clear(); auth::g_form.password.cursor = 0;
                    auth::g_form.invite.text.clear();   auth::g_form.invite.cursor   = 0;
                    auth::g_form.focus = 0;
                    auth::g_form.error_msg.clear();
                    // 缩窗 + 复位 tween — 关键修复 (之前直接切 Stage::Auth 但窗口还是 1100×720)
                    stages::enterAuthFromLogout();
                    toast::show(trW("logout.done"));
                },
                trW("logout.yes"), trW("logout.cancel"), true);
            g_account_dropdown = false;
            g_dropdown_t.start(g_dropdown_t.value(), 0.0f, 0.15f, 0, curve::easeOutCubic);
        }, true },
    };
    auto* item_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(9.0f));
    bool last_was_danger = false;
    for (size_t ii = 0; ii < sizeof(items) / sizeof(items[0]); ++ii) {
        auto& it = items[ii];
        // 在 danger 项之前补一条分割线，把"退出登录"跟其他普通项视觉上分开
        if (it.danger && !last_was_danger) {
            prim::drawLine(ctx, dx + 12, iy + 4, dx + dw - 12, iy + 4,
                           br.solidA(pal.divider, t), 1.0f);
            iy += 8;
        }
        LayoutRect r{ dx + 6, iy, dw - 12, 30 };
        bool hover = r.contains(g_mouse);
        if (hover) {
            uint32_t hbg = it.danger ? 0xE34B4B : pal.text;
            float ha = it.danger ? 0.11f : 0.08f;
            prim::fillRR(ctx, r.x, r.y, r.w, r.h, 6.0f,
                         br.solidA(hbg, t * ha));
        }
        uint32_t tc = it.danger ? 0xE34B4B : pal.text;
        icons::drawIcon(app, it.icon, r.x + 10, r.y + 7, 16, fadeArgb(tc, t));
        std::wstring lbl = trW(it.label_key);
        prim::drawText_(ctx, lbl, item_fmt,
                        r.x + 32, r.y + 8, r.w - 40, 18,
                        br.solidA(tc, t));
        hit(r, it.click, true);
        iy += 32;
        last_was_danger = it.danger;
    }
}

void registerDropdownDismissHits(float W, float H) {
    if (!g_account_dropdown && g_dropdown_t.value() < 0.001f) return;
    float dw = 240.0f;
    float fold_part = g_status_fold_t.value() * (4 * 24.0f + 8.0f);
    float dh = 8 + 50 + 32 + 32 + fold_part + 4 * 32.0f + 8 + 8;
    float dx = W - 16.0f - dw;
    float dy = kTopbarH;
    auto dismiss = [](){
        g_account_dropdown = false;
        g_status_fold_open = false;
        g_dropdown_t.start(g_dropdown_t.value(), 0.0f, 0.15f, 0, curve::easeOutCubic);
        g_status_fold_t.start(g_status_fold_t.value(), 0.0f, 0.15f, 0, curve::easeOutCubic);
    };
    hit(LayoutRect{ 0, 0, dx, kTopbarH }, dismiss, true);
    hit(LayoutRect{ 0, kTopbarH, dx, H - kTopbarH }, dismiss, true);
    hit(LayoutRect{ dx + dw, kTopbarH, W - (dx + dw), H - kTopbarH }, dismiss, true);
    hit(LayoutRect{ dx, dy + dh, dw, H - (dy + dh) }, dismiss, true);
}

// ============================== Views ==============================

float fadeOp() {
    float op = stages::g_view_fade.started ? stages::g_view_fade.value() : 1.0f;
    return stages::g_main_opacity.value() * op;
}

void paintHomeView(D2DApp& app, float ax, float ay, float aw, float ah) {
    const Palette& pal = palette();
    auto* ctx = app.ctx();
    auto& br = app.brushes();
    float op = fadeOp();
    if (op <= 0.001f) return;

    auto* h1 = app.texts().format(L"Microsoft YaHei UI", ptToDip(22.0f),
                                  DWRITE_FONT_WEIGHT_BOLD);
    auto* sub = app.texts().format(L"Microsoft YaHei UI", ptToDip(10.0f));
    auto* meta_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(8.0f));
    auto* val_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(11.0f),
                                       DWRITE_FONT_WEIGHT_BOLD);

    float vx = ax + 32, vy = ay + 28;
    float ty = vy + (1.0f - op) * 8.0f;

    // greet + sub - i18n
    {
        std::wstring greet_tpl = trW("home.greet");
        // 替换 {nickname}
        auto p = greet_tpl.find(L"{nickname}");
        if (p != std::wstring::npos) greet_tpl.replace(p, 10, g_user.nickname);
        prim::drawText_(ctx, greet_tpl, h1,
                        vx, ty, aw - 64, 36,
                        br.solidA(pal.text, op));
    }
    prim::drawText_(ctx, trW("home.subtitle"), sub,
                    vx, ty + 36, aw - 64, 18,
                    br.solidA(pal.text_muted, op));

    // profile-card 240 × auto, 主视觉
    float cx = vx, cy = vy + 80;
    float cw = aw - 64, ch = 140;
    prim::drawShadow(ctx, br, cx, cy, cw, ch, 12.0f, pal.shadow_card, op, 2.0f, 3);
    prim::fillRR(ctx, cx, cy, cw, ch, 12.0f, br.solidA(pal.card, op));

    // 每日签到按钮 — 卡片右上角，primary 样式（同 profile 上传按钮）。
    // 位置避开 fx=cx+22+ar*2+18 起的名字/状态文字。是否已签到由服务器 granted 决定，
    // 不用客户端本地日期门控（时区差异会与服务器不一致）。
    {
        LayoutRect ci{ cx + cw - 112, cy + 20, 92, 30 };
        bool ci_hov = ci.contains(g_mouse) && !fetch::g_checkin_inflight;
        prim::fillRR(ctx, ci.x, ci.y, ci.w, ci.h, 8.0f,
                     br.solidA(ci_hov ? pal.primary_hover : pal.primary,
                               op * (fetch::g_checkin_inflight ? 0.5f : 1.0f)));
        auto* ci_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(9.0f),
                                          DWRITE_FONT_WEIGHT_BOLD);
        prim::drawText_(ctx, trW("home.checkin"), ci_fmt,
                        ci.x, ci.y, ci.w, ci.h,
                        br.solidA(0xFFFFFF, op),
                        DWRITE_TEXT_ALIGNMENT_CENTER,
                        DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        if (!fetch::g_checkin_inflight && !g_session_token.empty()) {
            hit(ci, [](){
                fetch::g_checkin_inflight = true;
                fetch::checkin(GetActiveWindow());
            }, true);
        }
    }

    // 头像 72×72
    float ar = 36.0f;
    drawAvatarPill(app, cx + 22, cy + 22, ar, op);

    // 状态 dot
    prim::fillCircle(ctx, cx + 22 + ar * 2 - 9, cy + 22 + ar * 2 - 9, 6.0f,
                     br.solidA(statusColor(g_status), op));
    prim::strokeCircle(ctx, cx + 22 + ar * 2 - 9, cy + 22 + ar * 2 - 9, 6.0f,
                       br.solidA(pal.card, op), 3.0f);

    float fx = cx + 22 + ar * 2 + 18;
    std::wstring home_name = fitText(app, g_user.nickname, h1, cw - (fx - cx) - 22.0f);
    prim::drawText_(ctx, home_name, h1,
                    fx, cy + 20, cw - (fx - cx) - 22, 32,
                    br.solidA(pal.text, op));
    // status 行下移到 cy+62：22pt 名字行高约 30，避免长名描边与 Online 文字相挤
    prim::drawText_(ctx, statusLabel(g_status), sub,
                    fx, cy + 62, cw - (fx - cx) - 22, 18,
                    br.solidA(statusColor(g_status), op));

    // 3 stat 卡（订阅 / 时间 / PC 名）— 时间精确到秒 + 时区 + VPN/国家
    float sw_ = (cw - 40) / 3.0f;
    float sy_ = cy + ch + 20;
    struct Stat { std::wstring label; std::wstring val; bool primary; };
    SYSTEMTIME st{};
    GetLocalTime(&st);
    // 计算时区（系统时区 - UTC offset）
    TIME_ZONE_INFORMATION tzi{};
    GetTimeZoneInformation(&tzi);
    int tz_min = -tzi.Bias;     // Bias 是 UTC-Local，所以 -Bias = local offset
    wchar_t tbuf[64];
    if (g_geo_country[0] != 0) {
        // 如果有 VPN/国家信息，附加显示
        swprintf_s(tbuf, L"%02d:%02d:%02d  UTC%+d  %ls",
                   st.wHour, st.wMinute, st.wSecond, tz_min / 60,
                   g_geo_country);
    } else {
        swprintf_s(tbuf, L"%02d:%02d:%02d  UTC%+d",
                   st.wHour, st.wMinute, st.wSecond, tz_min / 60);
    }
    // 设备公开 tag — 双重 SHA-256 + pepper salt 后衍生 24-hex 5-segment
    // 跟服务端验证用同一个 hash 函数（不可反推真 HWID）
    std::string dt = getDeviceTag();
    int dtn = MultiByteToWideChar(CP_UTF8, 0, dt.c_str(), -1, nullptr, 0);
    std::wstring dtw(dtn > 0 ? dtn - 1 : 0, 0);
    if (dtn > 0) MultiByteToWideChar(CP_UTF8, 0, dt.c_str(), -1, dtw.data(), dtn);
    Stat stats[] = {
        { trW("home.time"),         tbuf, false },
        { trW("home.device_tag"),   dtw, false },
        { trW("home.subscription"), g_user.expires, true },
    };
    float spx = cx;
    for (auto& s : stats) {
        prim::drawShadow(ctx, br, spx, sy_, sw_, 84, 12.0f, pal.shadow_card, op * 0.7f, 2.0f, 2);
        if (s.primary) {
            prim::fillRR(ctx, spx, sy_, sw_, 84, 12.0f, br.solidA(pal.primary, op));
            prim::drawText_(ctx, s.label.c_str(), meta_fmt,
                            spx + 16, sy_ + 14, sw_ - 32, 14,
                            br.solidA(0xFFFFFF, op * 0.85f));
            prim::drawText_(ctx, s.val.c_str(), val_fmt,
                            spx + 16, sy_ + 38, sw_ - 32, 28,
                            br.solidA(0xFFFFFF, op));
        } else {
            prim::fillRR(ctx, spx, sy_, sw_, 84, 12.0f, br.solidA(pal.card, op));
            prim::drawText_(ctx, s.label.c_str(), meta_fmt,
                            spx + 16, sy_ + 14, sw_ - 32, 14,
                            br.solidA(pal.text_muted, op));
            // 时间字段 + 机器码字段都用小字号（要装下时区或 5-段 hex）
            float val_size = ptToDip(11.0f);
            if (s.label == trW("home.time"))       val_size = ptToDip(9.5f);
            else if (s.label == trW("home.device_tag")) val_size = ptToDip(8.0f);
            auto* val_fmt2 = app.texts().format(L"Microsoft YaHei UI", val_size,
                                                 DWRITE_FONT_WEIGHT_BOLD);
            // 机器码很长（12 段 hex），时间可能带时区+国家后缀 — 都强制单行 + 末尾省略，
            // 避免在卡片里折成两行错位。
            std::wstring val_disp = s.val;
            if (s.label == trW("home.device_tag") || s.label == trW("home.time")) {
                val_disp = fitText(app, s.val, val_fmt2, sw_ - 32);
            }
            prim::drawTextNoWrap(ctx, val_disp.c_str(), val_fmt2,
                            spx + 16, sy_ + 38, sw_ - 32, 28,
                            br.solidA(pal.text, op));
        }
        spx += sw_ + 20;
    }

    // tags chips
    auto* chip_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(8.5f));
    prim::drawText_(ctx, trW("home.tags"), sub,
                    cx, sy_ + 100, cw, 18,
                    br.solidA(pal.text_muted, op));
    float chipx = cx, chipy = sy_ + 124;
    std::vector<std::wstring> tags;
    {
        std::lock_guard<std::mutex> lk(g_user_tags_mtx);
        tags = g_user_tags;
    }
    bool empty_real = tags.empty() && g_session_token.empty();
    if (empty_real) {
        tags = { L"CS2", L"Premier 18k", trW("home.sample_tag_room") };
    }
    for (auto& tag : tags) {
        float max_chip_w = (std::max)(96.0f, cw - 54.0f);
        float target_chip_w = (std::min)((std::max)(measureW(app, tag, chip_fmt) + 24.0f, 72.0f),
                                         max_chip_w);
        std::wstring tag_disp = fitText(app, tag, chip_fmt, target_chip_w - 24.0f);
        float tw = (std::min)((std::max)(measureW(app, tag_disp, chip_fmt) + 24.0f, 72.0f),
                              max_chip_w);
        LayoutRect chip_rect{ chipx, chipy, tw, 26 };
        bool chov = chip_rect.contains(g_mouse);
        prim::fillRR(ctx, chipx, chipy, tw, 26, 13,
                     br.solidA(chov ? pal.bg : pal.surface, op));
        prim::strokeRR(ctx, chipx, chipy, tw, 26, 13,
                       br.solidA(pal.divider, op));
        prim::drawText_(ctx, tag_disp, chip_fmt,
                        chipx + 12, chipy + 5, tw - 24, 16,
                        br.solidA(pal.text, op));
        if (chov && !empty_real) {
            // hover 显示 ✕ 删除
            float xx = chipx + tw - 14;
            icons::drawIcon(app, icons::Name::X, xx - 4, chipy + 7, 12,
                            fadeArgb(pal.text_muted, op));
            std::wstring tcopy = tag;
            hit(LayoutRect{ xx - 6, chipy + 4, 18, 18 }, [tcopy]() {
                fetch::removeTag(GetActiveWindow(), tcopy);
            }, true);
        }
        chipx += tw + 8;
        if (chipx > cx + cw - 80) break;
    }
    // + 添加 chip — 文案随语言变长（英 "+ Add tag" 比中 "+ 添加" 宽），
    // 用 ceil 后的实测宽 + 左右 12px padding，并对内部 draw 关掉换行，避免裁成两行。
    if (!empty_real) {
        std::wstring add = trW("home.add_tag");
        float pad = 12.0f;
        float text_w = std::ceil(measureW(app, add, chip_fmt)) + 1.0f;
        float aw_ = text_w + pad * 2;
        LayoutRect addr{ chipx, chipy, aw_, 26 };
        bool ahov = addr.contains(g_mouse);
        prim::strokeRR(ctx, chipx, chipy, aw_, 26, 13,
                       br.solidA(pal.primary, op * (ahov ? 1.0f : 0.6f)));
        chip_fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        prim::drawText_(ctx, add, chip_fmt,
                        chipx + pad, chipy + 5, text_w, 16,
                        br.solidA(pal.primary, op));
        chip_fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        hit(addr, [](){ modal::openAddTag(); }, true);
    }
}

// ---- Lunch-view CS2 cover video (WebView2 <video>) ----------------------
// 本 view 是 webview 的第三个消费者（另两个：modals.cpp 的 CS2 modal 与通用
// video/web modal）。webview 是全局单例，故用一个 tracker 记录「当前 navigate
// 的内容」，避免每帧重复 navigate，也能在 modal 改过 URL 后检测到需要重载。
// 离开 Lunching view 或有 modal 抢占时必须 show(false) 并 invalidate tracker。
namespace {

// tracker 值：空 = 未由本 view 导航过（或已失效，下帧需重新 navigate）。
std::wstring g_lunch_video_nav;

// 本地路径 → file:/// URL（与 modals.cpp fileToFileUrl 等价；各自 anon ns）。
std::wstring lunchFileToFileUrl(const std::wstring& path) {
    std::wstring url = L"file:///";
    for (wchar_t c : path) url.push_back(c == L'\\' ? L'/' : c);
    return url;
}

// cover-tile 版 <video>：muted + playsinline + loop + autoplay，去掉 controls，
// object-fit:cover 铺满圆角瓦片。src 已是 file:/// 或 http(s):// URL。
std::wstring buildTileVideoHtml(const std::wstring& src) {
    std::wstring h = L"<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
                     L"<style>html,body{margin:0;padding:0;background:#000;height:100vh;overflow:hidden;}"
                     L"video{width:100%;height:100%;object-fit:cover;display:block;}</style></head>"
                     L"<body><video src=\"";
    h += src;
    h += L"\" autoplay muted loop playsinline></video></body></html>";
    return h;
}

}  // anon

// 离开 Lunching view / modal 抢占时调用：隐藏 webview 并失效 tracker，
// 下次回到 Lunching 会重新 navigate 视频进瓦片。
void hideLunchVideo() {
    if (!g_lunch_video_nav.empty()) {
        webview::show(false);
        g_lunch_video_nav.clear();
    }
}

void paintLunchingView(D2DApp& app, float ax, float ay, float aw, float ah) {
    const Palette& pal = palette();
    auto* ctx = app.ctx();
    auto& br = app.brushes();
    float op = fadeOp();
    if (op <= 0.001f) return;

    auto* h1 = app.texts().format(L"Microsoft YaHei UI", ptToDip(22.0f),
                                  DWRITE_FONT_WEIGHT_BOLD);
    auto* sub = app.texts().format(L"Microsoft YaHei UI", ptToDip(9.5f));
    prim::drawText_(ctx, trW("launch.title"), h1,
                    ax + 32, ay + 28, aw - 64, 36,
                    br.solidA(pal.text, op));

    // CS2 game-card 240×140 — 真 cover 缩略图
    float cx = ax + 32, cy = ay + 80;
    LayoutRect game_card{ cx, cy, 240, 140 };
    bool gc_hov = game_card.contains(g_mouse);
    float lift = gc_hov ? -2.0f : 0.0f;
    prim::drawShadow(ctx, br, cx, cy + lift, 240, 140, 12.0f,
                     pal.shadow_card_hover, op, gc_hov ? 5.0f : 4.0f, 4);

    // CS2 cover：优先 WebView2 <video> 铺在瓦片上；否则回退静态 cs2_header.jpg。
    // wantVideo 条件：runtime 可用 + 有 mp4 文件 + view 淡入基本完成（op>0.95，
    // 与 CS2 modal 的 t>0.95 门控一致，保证 bounds 稳定）+ 没有 modal 抢占 webview
    //（CS2 modal 正是从本瓦片打开的，两者共用同一个全局 webview 单例）。
    auto cs2_video = cs2VideoPath();
    bool wantVideo = webview::runtimeAvailable() && !cs2_video.empty()
                     && op > 0.95f && !modal::anyOpen();
    bool videoShown = false;
    if (wantVideo) {
        webview::ensure(app.hwnd());
        if (webview::isReady()) {
            float scale = app.dpi() / 96.0f;
            int wl = (int)(cx * scale);
            int wt = (int)((cy + lift) * scale);
            int wr = (int)((cx + 240) * scale);
            int wb = (int)((cy + lift + 140) * scale);
            webview::setBounds(wl, wt, wr, wb);
            // 只在内容变化时 navigate（tracker 记录本 view 最后导航的 src）。
            std::wstring src = lunchFileToFileUrl(cs2_video);
            if (g_lunch_video_nav != src) {
                webview::navigateHtml(buildTileVideoHtml(src));
                g_lunch_video_nav = src;
            }
            webview::show(true);
            videoShown = true;
        }
    }

    if (!videoShown) {
        // webview 未铺（无 runtime / 无 mp4 / 淡入中 / modal 抢占）→ 隐藏视频，
        // 画静态 cs2_header.jpg 封面兜底，绝不空白。
        hideLunchVideo();
        auto cs2_path = cs2HeaderPath();
        auto* cover = cs2_path.empty() ? nullptr : app.images().fromFile(cs2_path, 480);
        if (cover) {
            // 真圆角 mask（之前 PushAxisAlignedClip 让 4 角是直的）
            prim::pushLayerRR(ctx, app.factory(), cx, cy + lift, 240, 140, 12.0f);
            D2D1_SIZE_F sz = cover->GetSize();
            float scale = (std::max)(240.0f / sz.width, 140.0f / sz.height);
            float dw = sz.width * scale, dh = sz.height * scale;
            float dx = cx + (240 - dw) * 0.5f, dy = cy + lift + (140 - dh) * 0.5f;
            ctx->DrawBitmap(cover, D2D1::RectF(dx, dy, dx + dw, dy + dh),
                            op, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            prim::fillRect(ctx, cx, cy + lift + 80, 240, 60,
                           br.solidA(0x000000, op * 0.55f));
            prim::popLayer(ctx);
        } else {
            prim::fillRR(ctx, cx, cy + lift, 240, 140, 12.0f, br.solidA(0xC96442, op));
        }
    } else {
        // 视频铺满瓦片上方（z-order 高于 D2D），底部再压一条渐变让标题文字可读。
        prim::fillRect(ctx, cx, cy + lift + 80, 240, 60,
                       br.solidA(0x000000, op * 0.55f));
    }

    auto* gn_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(15.0f),
                                      DWRITE_FONT_WEIGHT_BOLD);
    prim::drawText_(ctx, L"Counter-Strike 2", gn_fmt,
                    cx + 16, cy + lift + 96, 240 - 32, 24,
                    br.solidA(0xFFFFFF, op));
    prim::drawText_(ctx, trW("launch.click_hint"), sub,
                    cx + 16, cy + lift + 118, 240 - 32, 16,
                    br.solidA(0xFFFFFF, op * 0.78f));
    // 中央 ▶ (hover 才显)
    if (gc_hov) {
        float pcx = cx + 120, pcy = cy + lift + 60;
        prim::fillCircle(ctx, pcx, pcy, 22, br.solidA(0x000000, op * 0.55f));
        prim::strokeCircle(ctx, pcx, pcy, 22, br.solidA(0xFFFFFF, op), 1.5f);
        icons::drawIcon(app, icons::Name::Play, pcx - 9, pcy - 9, 18, fadeArgb(0xFFFFFFFF, op));
    }

    hit(game_card, [](){ modal::openCS2(); }, true);
}

void paintMarketView(D2DApp& app, float ax, float ay, float aw, float ah) {
    const Palette& pal = palette();
    auto* ctx = app.ctx();
    auto& br = app.brushes();
    float op = fadeOp();
    if (op <= 0.001f) return;
    auto* h1 = app.texts().format(L"Microsoft YaHei UI", ptToDip(22.0f),
                                  DWRITE_FONT_WEIGHT_BOLD);
    auto* sub = app.texts().format(L"Microsoft YaHei UI", ptToDip(10.0f));
    auto* item_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(11.0f),
                                        DWRITE_FONT_WEIGHT_BOLD);
    auto* meta = app.texts().format(L"Microsoft YaHei UI", ptToDip(8.5f));

    prim::drawText_(ctx, trW("market.title"), h1,
                    ax + 32, ay + 28, aw - 64, 36,
                    br.solidA(pal.text, op));
    prim::drawText_(ctx, trW("market.sub"), sub,
                    ax + 32, ay + 64, aw - 64, 22,
                    br.solidA(pal.text_muted, op));

    std::vector<fetch::Listing> listings;
    bool market_loaded = false;
    {
        std::lock_guard<std::mutex> lk(fetch::g_market_mtx);
        listings = fetch::g_market_listings;
        market_loaded = fetch::g_market_loaded;
    }
    if (listings.empty()) {
        prim::drawText_(ctx, market_loaded ? trW("market.empty") : trW("market.loading"), sub,
                        ax + 32, ay + 100, aw - 64, 22,
                        br.solidA(pal.text_muted, op));
        return;
    }
    int n = (int)listings.size();
    int per_row = 2;
    float card_w = (aw - 96) / per_row;
    float card_h = 110;
    for (int i = 0; i < n; ++i) {
        int row = i / per_row, col = i % per_row;
        float cx = ax + 32 + col * (card_w + 16);
        float cy = ay + 100 + row * (card_h + 16);
        if (cy > ay + ah) break;
        LayoutRect cr{ cx, cy, card_w, card_h };
        bool hov = cr.contains(g_mouse);
        prim::drawShadow(ctx, br, cx, cy + (hov ? -1.0f : 0.0f),
                         card_w, card_h, 12.0f, pal.shadow_card,
                         op, hov ? 3.0f : 2.0f, hov ? 3 : 2);
        prim::fillRR(ctx, cx, cy + (hov ? -1.0f : 0.0f),
                     card_w, card_h, 12.0f, br.solidA(pal.card, op));
        prim::drawText_(ctx, listings[i].title, item_fmt,
                        cx + 16, cy + 16, card_w - 32, 22,
                        br.solidA(pal.text, op));
        prim::drawText_(ctx, listings[i].summary, meta,
                        cx + 16, cy + 42, card_w - 32, 36,
                        br.solidA(pal.text_muted, op));
        std::wstring price_str = trW("market.price");
        if (auto pn = price_str.find(L"{n}"); pn != std::wstring::npos)
            price_str.replace(pn, 3, std::to_wstring(listings[i].price));
        if (auto ps = price_str.find(L"{seller}"); ps != std::wstring::npos)
            price_str.replace(ps, 8, listings[i].seller.substr(0, 16));
        prim::drawText_(ctx, price_str, meta,
                        cx + 16, cy + card_h - 26, card_w - 32, 18,
                        br.solidA(pal.primary, op));
        // 点卡片打开详情 modal（购买 + 评价）。id 是 std::string，按值捕获安全。
        hit(cr, [id = listings[i].id](){ modal::openMarketDetail(id); }, true);
    }
}

void paintCloudView(D2DApp& app, float ax, float ay, float aw, float ah) {
    const Palette& pal = palette();
    auto* ctx = app.ctx();
    auto& br = app.brushes();
    float op = fadeOp();
    if (op <= 0.001f) return;
    auto* h1 = app.texts().format(L"Microsoft YaHei UI", ptToDip(22.0f),
                                  DWRITE_FONT_WEIGHT_BOLD);
    auto* sub = app.texts().format(L"Microsoft YaHei UI", ptToDip(10.0f));
    prim::drawText_(ctx, trW("menu.cloud"), h1,
                    ax + 32, ay + 28, aw - 64, 36,
                    br.solidA(pal.text, op));
    // 大空状态卡
    float cx = ax + 32, cy = ay + 80, cw = aw - 64, ch = ah - 120;
    prim::fillRR(ctx, cx, cy, cw, ch, 16.0f, br.solidA(pal.surface, op));
    prim::strokeRR(ctx, cx, cy, cw, ch, 16.0f, br.solidA(pal.divider, op));
    icons::drawIcon(app, icons::Name::Cloud, cx + cw * 0.5f - 28, cy + ch * 0.4f - 28, 56,
                    fadeArgb(pal.text_faint, op));
    prim::drawText_(ctx, trW("cloud.disabled"), sub,
                    cx, cy + ch * 0.4f + 36, cw, 22,
                    br.solidA(pal.text_muted, op),
                    DWRITE_TEXT_ALIGNMENT_CENTER);
}

void paintSettingsView(D2DApp& app, float ax, float ay, float aw, float ah) {
    const Palette& pal = palette();
    auto* ctx = app.ctx();
    auto& br = app.brushes();
    float op = fadeOp();
    if (op <= 0.001f) return;
    auto* h1 = app.texts().format(L"Microsoft YaHei UI", ptToDip(22.0f),
                                  DWRITE_FONT_WEIGHT_BOLD);
    auto* sub = app.texts().format(L"Microsoft YaHei UI", ptToDip(10.0f));
    auto* lab_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(9.0f),
                                       DWRITE_FONT_WEIGHT_BOLD);
    prim::drawText_(ctx, trW("menu.settings"), h1,
                    ax + 32, ay + 28, aw - 64, 36,
                    br.solidA(pal.text, op));

    float vx = ax + 32, sy_ = ay + 80;

    // 主题 seg control — pill 滑块 tween
    prim::drawText_(ctx, trW("settings.theme"), lab_fmt,
                    vx, sy_, 200, 18, br.solidA(pal.text_muted, op));
    sy_ += 24;
    float seg_w = 240, seg_h = 36;
    prim::fillRR(ctx, vx, sy_, seg_w, seg_h, 8.0f, br.solidA(pal.surface, op));
    float pill_w_t = seg_w * 0.5f;
    float theme_target = g_dark ? pill_w_t : 0.0f;
    if (!g_seg_theme_x.started) {
        g_seg_theme_x.start(theme_target, theme_target, 0.001f, 0, curve::easeOutQuint);
    } else if (std::abs(g_seg_theme_x.to - theme_target) > 0.5f) {
        g_seg_theme_x.start(g_seg_theme_x.value(), theme_target, 0.30f, 0, curve::easeOutQuint);
    }
    float theme_pill_x = vx + g_seg_theme_x.value();
    prim::fillRR(ctx, theme_pill_x + 2, sy_ + 2, pill_w_t - 4, seg_h - 4, 6.0f,
                 br.solidA(pal.card, op));
    prim::drawText_(ctx, trW("settings.theme_light"), sub,
                    vx, sy_ + 9, pill_w_t, 18,
                    br.solidA(g_dark ? pal.text_muted : pal.text, op),
                    DWRITE_TEXT_ALIGNMENT_CENTER);
    prim::drawText_(ctx, trW("settings.theme_dark"), sub,
                    vx + pill_w_t, sy_ + 9, pill_w_t, 18,
                    br.solidA(g_dark ? pal.text : pal.text_muted, op),
                    DWRITE_TEXT_ALIGNMENT_CENTER);
    hit(LayoutRect{ vx, sy_, pill_w_t, seg_h },
        [](){ g_dark = false; persist::saveTheme(false); }, true);
    hit(LayoutRect{ vx + pill_w_t, sy_, pill_w_t, seg_h },
        [](){ g_dark = true; persist::saveTheme(true); }, true);
    sy_ += seg_h + 28;

    // 语言 seg control (3 段) — pill 滑块 tween
    prim::drawText_(ctx, trW("settings.language"), lab_fmt,
                    vx, sy_, 200, 18, br.solidA(pal.text_muted, op));
    sy_ += 24;
    float lseg_w = 360;
    prim::fillRR(ctx, vx, sy_, lseg_w, seg_h, 8.0f, br.solidA(pal.surface, op));
    int cur_lang = (int)g_lang;
    float lp_w = lseg_w / 3.0f;
    float lang_target = cur_lang * lp_w;
    if (!g_seg_lang_x.started) {
        g_seg_lang_x.start(lang_target, lang_target, 0.001f, 0, curve::easeOutQuint);
    } else if (std::abs(g_seg_lang_x.to - lang_target) > 0.5f) {
        g_seg_lang_x.start(g_seg_lang_x.value(), lang_target, 0.32f, 0, curve::easeOutQuint);
    }
    float lang_pill_x = vx + g_seg_lang_x.value();
    prim::fillRR(ctx, lang_pill_x + 2, sy_ + 2, lp_w - 4, seg_h - 4, 6.0f,
                 br.solidA(pal.card, op));
    const wchar_t* langs[3] = { L"English", L"简体中文", L"日本語" };
    for (int i = 0; i < 3; ++i) {
        prim::drawText_(ctx, langs[i], sub,
                        vx + i * lp_w, sy_ + 9, lp_w, 18,
                        br.solidA(i == cur_lang ? pal.text : pal.text_muted, op),
                        DWRITE_TEXT_ALIGNMENT_CENTER);
        int li = i;
        hit(LayoutRect{ vx + i * lp_w, sy_, lp_w, seg_h },
            [li](){ g_lang = (Lang)li; persist::saveLang(li); }, true);
    }
    sy_ += seg_h + 28;

    // 关于
    prim::drawText_(ctx, trW("settings.about"), lab_fmt,
                    vx, sy_, 200, 18, br.solidA(pal.text_muted, op));
    sy_ += 24;
    prim::drawText_(ctx, L"Launcher  v0.1.0  ·  D2D + DComp pipeline", sub,
                    vx, sy_, aw - 64, 22,
                    br.solidA(pal.text, op));
    sy_ += 22;
    prim::drawText_(ctx, L"© 2026 dwgx", sub,
                    vx, sy_, aw - 64, 22,
                    br.solidA(pal.text_muted, op));
}

void paintProfileView(D2DApp& app, float ax, float ay, float aw, float ah) {
    const Palette& pal = palette();
    auto* ctx = app.ctx();
    auto& br = app.brushes();
    float op = fadeOp();
    if (op <= 0.001f) return;
    auto* h1 = app.texts().format(L"Microsoft YaHei UI", ptToDip(16.0f),
                                  DWRITE_FONT_WEIGHT_BOLD);
    auto* lab_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(7.5f));
    auto* val_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(9.0f),
                                       DWRITE_FONT_WEIGHT_BOLD);
    prim::drawText_(ctx, trW("profile.title"), h1,
                    ax + 18, ay + 16, 200, 26,
                    br.solidA(pal.text, op));

    float cx = ax + 18, cy = ay + 50;
    float cw = aw - 36, ch = ah - 70;
    prim::drawShadow(ctx, br, cx, cy, cw, ch, 12.0f, pal.shadow_card, op, 2.0f, 3);
    prim::fillRR(ctx, cx, cy, cw, ch, 12.0f, br.solidA(pal.card, op));

    auto field = [&](float fy, const std::wstring& label,
                     const std::wstring& val, bool editable) {
        prim::drawText_(ctx, label, lab_fmt,
                        cx + 16, fy, 100, 14, br.solidA(pal.text_muted, op));
        LayoutRect box{ cx + 90, fy - 4, cw - 110, 26 };
        prim::fillRR(ctx, box.x, box.y, box.w, box.h, 5.0f,
                     br.solidA(editable ? pal.surface : pal.bg, op));
        std::wstring val_disp = fitText(app, val, val_fmt, box.w - 16.0f);
        prim::drawText_(ctx, val_disp, val_fmt,
                        box.x + 8, box.y + 4, box.w - 16, 18,
                        br.solidA(editable ? pal.text : pal.text_faint, op));
    };
    field(cy + 22,  trW("profile.uid"),      g_user.uid,      false);
    field(cy + 56,  trW("profile.username"), g_user.username, false);
    field(cy + 90,  trW("profile.nickname"), g_user.nickname, true);
    field(cy + 124, trW("profile.email"),    g_user.email,    false);
    field(cy + 158, trW("profile.expires"),  g_user.expires,  false);

    // 昵称字段可编辑 — 命中值框（与 field lambda 的 box 几何一致）打开改昵称 modal。
    {
        LayoutRect nick_box{ cx + 90, (cy + 90) - 4, cw - 110, 26 };
        hit(nick_box, [](){ modal::openEditNickname(); }, true);
    }

    // 个人签名 — 大 textarea + 编辑按钮
    prim::drawText_(ctx, trW("profile.bio"), lab_fmt,
                    cx + 16, cy + 196, 100, 14, br.solidA(pal.text_muted, op));
    LayoutRect bio_box{ cx + 16, cy + 214, cw - 32, 80 };
    prim::fillRR(ctx, bio_box.x, bio_box.y, bio_box.w, bio_box.h, 8.0f,
                 br.solidA(pal.surface, op));
    if (g_user.bio.empty()) {
        prim::drawText_(ctx, trW("profile.bio_empty"), val_fmt,
                        bio_box.x + 12, bio_box.y + 8, bio_box.w - 24, 64,
                        br.solidA(pal.text_faint, op));
    } else {
        prim::drawText_(ctx, g_user.bio, val_fmt,
                        bio_box.x + 12, bio_box.y + 8, bio_box.w - 24, 64,
                        br.solidA(pal.text, op));
    }
    hit(bio_box, [](){ modal::openEditBio(); }, true);

    // 上传头像 + 改密码 按钮
    float by = cy + ch - 40;
    LayoutRect up{ cx + 16, by, 130, 28 };
    bool up_hov = up.contains(g_mouse);
    bool up_press = up_hov && g_mouse_pressed;
    float up_lift = up_hov && !up_press ? -1.0f : (up_press ? 1.0f : 0.0f);
    prim::fillRR(ctx, up.x, up.y + up_lift, up.w, up.h, 6.0f,
                 br.solidA(up_hov ? pal.primary_hover : pal.primary, op));
    auto* btn_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(9.0f),
                                       DWRITE_FONT_WEIGHT_BOLD);
    prim::drawText_(ctx, trW("profile.upload_avatar"), btn_fmt,
                    up.x, up.y + up_lift, up.w, up.h,
                    br.solidA(0xFFFFFF, op),
                    DWRITE_TEXT_ALIGNMENT_CENTER,
                    DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    hit(up, [](){
        OPENFILENAMEW ofn{};
        static wchar_t fnbuf[MAX_PATH] = {0};
        fnbuf[0] = 0;
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = GetActiveWindow();
        std::wstring filter = trW("profile.upload_avatar");
        filter.push_back(L'\0');
        filter += L"*.png;*.jpg;*.jpeg;*.gif";
        filter.push_back(L'\0');
        filter.push_back(L'\0');
        ofn.lpstrFilter = filter.c_str();
        ofn.lpstrFile = fnbuf;
        ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (GetOpenFileNameW(&ofn)) {
            if (!supportedAvatarExt(fnbuf)) {
                std::wstring msg = trW("profile.upload_avatar") + std::wstring(L": PNG/JPG/GIF");
                toast::show(msg);
                return;
            }
            // 复制到 LOCALAPPDATA + 设 g_avatar_path
            wchar_t base[MAX_PATH] = {0};
            if (SHGetSpecialFolderPathW(nullptr, base, CSIDL_LOCAL_APPDATA, FALSE)) {
                std::wstring dir = std::wstring(base) + L"\\Launcher";
                CreateDirectoryW(dir.c_str(), nullptr);
                dir += L"\\avatars";
                CreateDirectoryW(dir.c_str(), nullptr);
                dir += L"\\self";
                CreateDirectoryW(dir.c_str(), nullptr);
                std::wstring src = fnbuf;
                auto dot = src.find_last_of(L'.');
                std::wstring ext = (dot != std::wstring::npos) ? src.substr(dot) : L".png";
                std::wstring stem;
                for (char c : g_user_id) {
                    bool ok = (c >= '0' && c <= '9')
                           || (c >= 'a' && c <= 'z')
                           || (c >= 'A' && c <= 'Z')
                           || c == '-' || c == '_';
                    stem.push_back(ok ? (wchar_t)c : L'_');
                }
                if (stem.empty()) stem = L"unknown";
                std::wstring dst = dir + L"\\" + stem + ext;
                if (CopyFileW(fnbuf, dst.c_str(), FALSE)) {
                    toast::show(trW("common.loading"));
                    fetch::uploadAvatar(GetActiveWindow(), dst);
                }
            }
        }
    }, true);

    LayoutRect pw{ cx + 156, by, 130, 28 };
    bool pw_hov = pw.contains(g_mouse);
    prim::fillRR(ctx, pw.x, pw.y, pw.w, pw.h, 6.0f,
                 br.solidA(pw_hov ? pal.bg : pal.card, op));
    prim::strokeRR(ctx, pw.x, pw.y, pw.w, pw.h, 6.0f,
                   br.solidA(pal.divider, op));
    prim::drawText_(ctx, trW("profile.change_pw"), btn_fmt,
                    pw.x, pw.y, pw.w, pw.h,
                    br.solidA(pal.text, op),
                    DWRITE_TEXT_ALIGNMENT_CENTER,
                    DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    hit(pw, [](){ modal::openChangePw(); }, true);
}

// Chat view 走 chat::paintChatView

}  // anon

void switchView(stages::View v) {
    if (v == stages::g_view) return;
    stages::g_view = v;
    stages::g_view_fade.start(0.0f, 1.0f, 0.25f, 0, curve::easeOutQuint);
}

void tickMain(float dt) {
    g_dropdown_t.tick(dt);
    g_status_fold_t.tick(dt);
    g_seg_lang_x.tick(dt);
    g_seg_lang_w.tick(dt);
    g_seg_theme_x.tick(dt);
    g_seg_theme_w.tick(dt);
    chat::tick(dt);
    modal::tickAll(dt);
}

void paintMain(D2DApp& app, float W, float H) {
    hitClear();
    const Palette& pal = palette();
    app.ctx()->Clear(argbToColorF(pal.bg));

    paintTopbar(app, W);
    paintSidebar(app, H);

    float ax = kSidebarW, ay = kTopbarH;
    float aw = W - kSidebarW, ah = H - kTopbarH;
    switch (stages::g_view) {
        case stages::View::Home:     paintHomeView(app, ax, ay, aw, ah);     break;
        case stages::View::Lunching: paintLunchingView(app, ax, ay, aw, ah); break;
        case stages::View::Chat:     chat::paintChatView(app, ax, ay, aw, ah); break;
        case stages::View::Market:   paintMarketView(app, ax, ay, aw, ah);   break;
        case stages::View::Cloud:    paintCloudView(app, ax, ay, aw, ah);    break;
        case stages::View::Settings: paintSettingsView(app, ax, ay, aw, ah); break;
        case stages::View::Profile:  paintProfileView(app, ax, ay, aw, ah);  break;
    }

    // 非 Lunching view 时隐藏 CS2 瓦片视频，别让 webview HWND 漏到其他页面。
    // （cheap：tracker 为空时直接 no-op。）Lunching view 内的 modal 抢占已由
    // paintLunchingView 里的 anyOpen() 分支处理。
    if (stages::g_view != stages::View::Lunching) hideLunchVideo();

    // dropdown 在 view 之上
    paintAccountDropdown(app, W);
    registerDropdownDismissHits(W, H);

    // modals 在最顶层
    modal::paintCS2Modal(app, W, H);
    modal::paintMarketDetailModal(app, W, H);
    modal::paintChangePwModal(app, W, H);
    modal::paintConfirmModal(app, W, H);
    modal::paintHistoryModal(app, W, H);
    modal::paintAddTagModal(app, W, H);
    modal::paintCreatePackModal(app, W, H);
    modal::paintRenamePackModal(app, W, H);
    modal::paintUserProfileModal(app, W, H);
    modal::paintEditStatusTextModal(app, W, H);
    modal::paintEditBioModal(app, W, H);
    modal::paintEditNicknameModal(app, W, H);
    modal::paintMuteUserModal(app, W, H);
    // WebView2 modal 在最顶（CS2 modal 已经直接 webview，这是通用浏览器/视频）
    modal::paintWebViewModal(app, W, H);

    // pack 预览 modal
    modal::paintPackPreviewModal(app, W, H);
    // 搜索 modal — Ctrl+F
    modal::paintSearchModal(app, W, H);
    // 消息右键菜单 — 在所有 modal 之上、toast 之下
    modal::paintMsgContextMenu(app, W, H);
    modal::paintUserContextMenu(app, W, H);

    chat::paintAnnouncementModal(app, W, H);

    // toast 在最最顶层
    toast::paint(app, W, H);
}

}  // namespace launcher::d2d::ui
