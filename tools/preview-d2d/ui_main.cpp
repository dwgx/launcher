// Main view 容器 + 各 view 渲染 — 1:1 复刻 GDI+ Preview 主框架。
//
// Topbar / Sidebar / AccountDropdown 严格按 styles.css token 实现；各 view 内容简化但
// 视觉骨架对齐 GDI+ Preview。Chat 和 Modals 在独立文件。

#include "ui_main.h"
#include "icons.h"
#include "palette.h"
#include "user_state.h"
#include "auth.h"
#include "hit.h"
#include "render/primitives.h"

#include <algorithm>
#include <cstdio>

namespace launcher::d2d::ui {

bool   g_account_dropdown = false;
bool   g_status_fold_open = false;
Tween  g_dropdown_t;
Tween  g_status_fold_t;
Tween  g_seg_lang_x, g_seg_lang_w, g_seg_theme_x, g_seg_theme_w;

namespace {

struct MenuEntry { stages::View view; const wchar_t* label; icons::Name icon; };
constexpr MenuEntry kMenu[] = {
    { stages::View::Home,     L"主页",    icons::Name::Home },
    { stages::View::Lunching, L"启动",    icons::Name::Library },
    { stages::View::Chat,     L"聊天",    icons::Name::Chat },
    { stages::View::Market,   L"市场",    icons::Name::Cart },
    { stages::View::Cloud,    L"云端",    icons::Name::Cloud },
    { stages::View::Settings, L"设置",    icons::Name::Settings },
};

float measureW(D2DApp& app, std::wstring_view s, IDWriteTextFormat* fmt) {
    if (s.empty() || !fmt) return 0.0f;
    DWRITE_TEXT_METRICS m{};
    if (!app.texts().measure(fmt, s, 8192.0f, 256.0f, &m)) return 0.0f;
    return m.width;
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
        auto* bmp = app.images().fromFile(g_avatar_path);
        if (bmp) {
            ctx->PushAxisAlignedClip(D2D1::RectF(ax, ay, ax + ar * 2, ay + ar * 2),
                                     D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            // 圆形裁剪用 layer 更准；矩形 clip 配 fillCircle 之后画 bitmap 简化处理
            ctx->DrawBitmap(bmp, D2D1::RectF(ax, ay, ax + ar * 2, ay + ar * 2),
                            op, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            ctx->PopAxisAlignedClip();
            return;
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
    float name_w = measureW(app, g_user.nickname, nick_fmt) + 4.0f;
    float pill_w = pill_pad_l + name_w + pill_gap + ar * 2 + pill_pad_r;
    float pill_x = W - 16.0f - pill_w;
    float pill_y = ty + (kTopbarH - pill_h) * 0.5f;

    bool pill_hover = LayoutRect{ pill_x, pill_y, pill_w, pill_h }.contains(g_mouse);
    if (pill_hover) {
        prim::fillRR(ctx, pill_x, pill_y, pill_w, pill_h, 16.0f,
                     br.solidA(pal.text, 0.04f));
    }
    prim::drawText_(ctx, g_user.nickname, nick_fmt,
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
            prim::fillRR(ctx, item.x - 10.0f, item.y + 9.0f, 3.0f, item.h - 18.0f, 1.5f,
                         br.solid(pal.primary));
        }
        uint32_t tc = active ? pal.primary : (hover ? pal.text : pal.text_muted);

        icons::drawIcon(app, m.icon, item.x + 12.0f, item.y + 9.0f, 20.0f, tc);
        prim::drawText_(ctx, m.label, item_fmt,
                        item.x + 44.0f, item.y + 11.0f, item.w - 50.0f, 18.0f,
                        br.solid(tc));

        stages::View target = m.view;
        hit(item, [target]() { switchView(target); }, true);
        my += 42.0f;
    }
}

void paintAccountDropdown(D2DApp& app, float W) {
    if (!g_account_dropdown && g_dropdown_t.value() < 0.001f) return;
    const Palette& pal = palette();
    auto* ctx = app.ctx();
    auto& br = app.brushes();

    float t = g_dropdown_t.value();
    if (t < 0.001f) return;

    float dw = 240.0f;
    float fold_extra = g_status_fold_t.value() * 124.0f;
    float dh = 240.0f + fold_extra;
    float dx = W - 6.0f - dw;
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
    prim::drawText_(ctx, g_user.nickname, nick_fmt,
                    dx + 50, dy + 12, dw - 60, 20,
                    br.solidA(pal.text, t));
    prim::drawText_(ctx, g_user.email, email_fmt,
                    dx + 50, dy + 28, dw - 60, 14,
                    br.solidA(pal.text_muted, t));

    prim::drawLine(ctx, dx + 8, dy + 50, dx + dw - 8, dy + 50,
                   br.solidA(pal.divider, t), 1.0f);

    // status fold trigger
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
                // 后端 sync — 留给 main 异步处理
            }, true);
            iy += 24.0f * ft;
        }
        if (ft > 0.5f) {
            prim::drawLine(ctx, dx + 8, iy + 4, dx + dw - 8, iy + 4,
                           br.solidA(pal.divider, t), 1.0f);
        }
        iy += 8;
    }

    struct Item { const wchar_t* label; icons::Name icon; std::function<void()> click; bool danger; };
    Item items[] = {
        { L"个人资料", icons::Name::User, [](){
            switchView(stages::View::Profile);
            g_account_dropdown = false;
            g_dropdown_t.start(g_dropdown_t.value(), 0.0f, 0.15f, 0, curve::easeOutCubic);
        }, false },
        { L"登录历史", icons::Name::History, [](){
            // History modal 留 Step 7
            g_account_dropdown = false;
            g_dropdown_t.start(g_dropdown_t.value(), 0.0f, 0.15f, 0, curve::easeOutCubic);
        }, false },
        { L"修改密码", icons::Name::Shield, [](){
            // ChangePw modal 留 Step 7
            g_account_dropdown = false;
            g_dropdown_t.start(g_dropdown_t.value(), 0.0f, 0.15f, 0, curve::easeOutCubic);
        }, false },
        { L"退出登录", icons::Name::Logout, [](){
            // 简化：清 session + 回到 Auth
            g_session_token.clear();
            g_user_id.clear();
            g_account_dropdown = false;
            g_dropdown_t.start(g_dropdown_t.value(), 0.0f, 0.15f, 0, curve::easeOutCubic);
            auth::g_form.username.text.clear(); auth::g_form.username.cursor = 0;
            auth::g_form.password.text.clear(); auth::g_form.password.cursor = 0;
            auth::g_form.invite.text.clear();   auth::g_form.invite.cursor   = 0;
            auth::g_form.focus = 0;
            auth::g_form.error_msg.clear();
            stages::g_stage = stages::Stage::Auth;
            stages::g_auth_card_op.start(0.0f, 1.0f, 0.40f, 0.05f, curve::easeOutCubic);
            stages::g_auth_card_y.start(12.0f, 0.0f, 0.45f, 0.05f, curve::easeOutQuint);
        }, true },
    };
    auto* item_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(9.0f));
    for (auto& it : items) {
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
        prim::drawText_(ctx, it.label, item_fmt,
                        r.x + 32, r.y + 8, r.w - 40, 18,
                        br.solidA(tc, t));
        hit(r, it.click, true);
        iy += 32;
    }
}

void registerDropdownDismissHits(float W, float H) {
    if (!g_account_dropdown && g_dropdown_t.value() < 0.001f) return;
    float dw = 240.0f;
    float dh = 240.0f + g_status_fold_t.value() * 124.0f;
    float dx = W - 6.0f - dw;
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

    // greet + sub
    wchar_t greet[256];
    swprintf_s(greet, L"你好，%ls", g_user.nickname.c_str());
    prim::drawText_(ctx, greet, h1,
                    vx, ty, aw - 64, 36,
                    br.solidA(pal.text, op));
    prim::drawText_(ctx, L"欢迎回来", sub,
                    vx, ty + 36, aw - 64, 18,
                    br.solidA(pal.text_muted, op));

    // profile-card 240 × auto, 主视觉
    float cx = vx, cy = vy + 80;
    float cw = aw - 64, ch = 140;
    prim::drawShadow(ctx, br, cx, cy, cw, ch, 12.0f, pal.shadow_card, op, 2.0f, 3);
    prim::fillRR(ctx, cx, cy, cw, ch, 12.0f, br.solidA(pal.card, op));

    // 头像 72×72
    float ar = 36.0f;
    drawAvatarPill(app, cx + 22, cy + 22, ar, op);

    // 状态 dot
    prim::fillCircle(ctx, cx + 22 + ar * 2 - 9, cy + 22 + ar * 2 - 9, 6.0f,
                     br.solidA(statusColor(g_status), op));
    prim::strokeCircle(ctx, cx + 22 + ar * 2 - 9, cy + 22 + ar * 2 - 9, 6.0f,
                       br.solidA(pal.card, op), 3.0f);

    float fx = cx + 22 + ar * 2 + 18;
    prim::drawText_(ctx, g_user.nickname, h1,
                    fx, cy + 24, cw - (fx - cx) - 22, 30,
                    br.solidA(pal.text, op));
    prim::drawText_(ctx, statusLabel(g_status), sub,
                    fx, cy + 56, cw - (fx - cx) - 22, 18,
                    br.solidA(statusColor(g_status), op));

    // 3 stat 卡（订阅 / 时间 / PC 名）
    float sw_ = (cw - 40) / 3.0f;
    float sy_ = cy + ch + 20;
    struct Stat { const wchar_t* label; const wchar_t* val; bool primary; };
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t tbuf[32];
    swprintf_s(tbuf, L"%02d:%02d", st.wHour, st.wMinute);
    wchar_t pcname[64] = {0}; DWORD pcsz = 64;
    GetComputerNameW(pcname, &pcsz);
    Stat stats[] = {
        { L"当前时间", tbuf, false },
        { L"本机名",   pcname, false },
        { L"订阅",     g_user.expires.c_str(), true },
    };
    float spx = cx;
    for (auto& s : stats) {
        prim::drawShadow(ctx, br, spx, sy_, sw_, 84, 12.0f, pal.shadow_card, op * 0.7f, 2.0f, 2);
        if (s.primary) {
            // 主色渐变占位 — 简化为主色 fill
            prim::fillRR(ctx, spx, sy_, sw_, 84, 12.0f, br.solidA(pal.primary, op));
            prim::drawText_(ctx, s.label, meta_fmt,
                            spx + 16, sy_ + 14, sw_ - 32, 14,
                            br.solidA(0xFFFFFF, op * 0.85f));
            prim::drawText_(ctx, s.val, val_fmt,
                            spx + 16, sy_ + 38, sw_ - 32, 28,
                            br.solidA(0xFFFFFF, op));
        } else {
            prim::fillRR(ctx, spx, sy_, sw_, 84, 12.0f, br.solidA(pal.card, op));
            prim::drawText_(ctx, s.label, meta_fmt,
                            spx + 16, sy_ + 14, sw_ - 32, 14,
                            br.solidA(pal.text_muted, op));
            prim::drawText_(ctx, s.val, val_fmt,
                            spx + 16, sy_ + 38, sw_ - 32, 28,
                            br.solidA(pal.text, op));
        }
        spx += sw_ + 20;
    }

    // tags chips
    auto* chip_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(8.5f));
    prim::drawText_(ctx, L"我的标签", sub,
                    cx, sy_ + 100, cw, 18,
                    br.solidA(pal.text_muted, op));
    float chipx = cx, chipy = sy_ + 124;
    std::vector<std::wstring> tags;
    {
        std::lock_guard<std::mutex> lk(g_user_tags_mtx);
        tags = g_user_tags;
    }
    if (tags.empty()) {
        // 默认占位标签
        tags = { L"CS2", L"Premier 18k", L"东京机房" };
    }
    for (auto& t : tags) {
        float tw = measureW(app, t, chip_fmt) + 24;
        prim::fillRR(ctx, chipx, chipy, tw, 26, 13,
                     br.solidA(pal.surface, op));
        prim::strokeRR(ctx, chipx, chipy, tw, 26, 13,
                       br.solidA(pal.divider, op));
        prim::drawText_(ctx, t, chip_fmt,
                        chipx + 12, chipy + 5, tw - 24, 16,
                        br.solidA(pal.text, op));
        chipx += tw + 8;
        if (chipx > cx + cw - 80) break;
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
    prim::drawText_(ctx, L"启动游戏", h1,
                    ax + 32, ay + 28, aw - 64, 36,
                    br.solidA(pal.text, op));

    // CS2 game-card 240×140
    float cx = ax + 32, cy = ay + 80;
    prim::drawShadow(ctx, br, cx, cy, 240, 140, 12.0f, pal.shadow_card_hover, op, 4.0f, 4);
    prim::fillRR(ctx, cx, cy, 240, 140, 12.0f, br.solidA(0xC96442, op));
    auto* gn_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(16.0f),
                                      DWRITE_FONT_WEIGHT_BOLD);
    prim::drawText_(ctx, L"Counter-Strike 2", gn_fmt,
                    cx + 16, cy + 16, 240 - 32, 28,
                    br.solidA(0xFFFFFF, op));
    prim::drawText_(ctx, L"点击启动", sub,
                    cx + 16, cy + 110, 240 - 32, 18,
                    br.solidA(0xFFFFFF, op * 0.85f));
    // 中央 ▶
    float pcx = cx + 120, pcy = cy + 70;
    prim::fillCircle(ctx, pcx, pcy, 22, br.solidA(0xFFFFFF, op * 0.85f));
    icons::drawIcon(app, icons::Name::Play, pcx - 10, pcy - 10, 20, 0xFF1F1E1D);

    hit(LayoutRect{ cx, cy, 240, 140 }, [](){
        // CS2 modal 留 Step 7
    }, true);
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
    prim::drawText_(ctx, L"市场", h1,
                    ax + 32, ay + 28, aw - 64, 36,
                    br.solidA(pal.text, op));
    prim::drawText_(ctx, L"CS2 .cfg / 配置 / 模板", sub,
                    ax + 32, ay + 64, aw - 64, 22,
                    br.solidA(pal.text_muted, op));
    // 占位 listing 卡
    for (int i = 0; i < 3; ++i) {
        float cy = ay + 100 + i * 96;
        prim::drawShadow(ctx, br, ax + 32, cy, aw - 64, 80, 12.0f, pal.shadow_card, op, 2.0f, 2);
        prim::fillRR(ctx, ax + 32, cy, aw - 64, 80, 12.0f, br.solidA(pal.card, op));
        wchar_t buf[64];
        swprintf_s(buf, L"商品 #%d", i + 1);
        prim::drawText_(ctx, buf, sub,
                        ax + 56, cy + 20, aw - 96, 22,
                        br.solidA(pal.text, op));
        prim::drawText_(ctx, L"接 /api/market/listings 后填充", sub,
                        ax + 56, cy + 44, aw - 96, 22,
                        br.solidA(pal.text_muted, op));
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
    prim::drawText_(ctx, L"云端", h1,
                    ax + 32, ay + 28, aw - 64, 36,
                    br.solidA(pal.text, op));
    // 大空状态卡
    float cx = ax + 32, cy = ay + 80, cw = aw - 64, ch = ah - 120;
    prim::fillRR(ctx, cx, cy, cw, ch, 16.0f, br.solidA(pal.surface, op));
    prim::strokeRR(ctx, cx, cy, cw, ch, 16.0f, br.solidA(pal.divider, op));
    icons::drawIcon(app, icons::Name::Cloud, cx + cw * 0.5f - 28, cy + ch * 0.4f - 28, 56,
                    fadeArgb(pal.text_faint, op));
    prim::drawText_(ctx, L"云端同步暂未启用", sub,
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
    prim::drawText_(ctx, L"设置", h1,
                    ax + 32, ay + 28, aw - 64, 36,
                    br.solidA(pal.text, op));

    float vx = ax + 32, sy_ = ay + 80;

    // 主题 seg control
    prim::drawText_(ctx, L"主题", lab_fmt,
                    vx, sy_, 200, 18, br.solidA(pal.text_muted, op));
    sy_ += 24;
    float seg_w = 240, seg_h = 36;
    prim::fillRR(ctx, vx, sy_, seg_w, seg_h, 8.0f, br.solidA(pal.surface, op));
    bool theme_dark = g_dark;
    float pill_w_t = seg_w * 0.5f;
    float pill_x = vx + (theme_dark ? pill_w_t : 0);
    prim::fillRR(ctx, pill_x + 2, sy_ + 2, pill_w_t - 4, seg_h - 4, 6.0f,
                 br.solidA(pal.card, op));
    prim::drawText_(ctx, L"亮", sub,
                    vx, sy_ + 9, pill_w_t, 18,
                    br.solidA(theme_dark ? pal.text_muted : pal.text, op),
                    DWRITE_TEXT_ALIGNMENT_CENTER);
    prim::drawText_(ctx, L"暗", sub,
                    vx + pill_w_t, sy_ + 9, pill_w_t, 18,
                    br.solidA(theme_dark ? pal.text : pal.text_muted, op),
                    DWRITE_TEXT_ALIGNMENT_CENTER);
    hit(LayoutRect{ vx, sy_, pill_w_t, seg_h }, [](){ g_dark = false; }, true);
    hit(LayoutRect{ vx + pill_w_t, sy_, pill_w_t, seg_h }, [](){ g_dark = true; }, true);
    sy_ += seg_h + 28;

    // 关于
    prim::drawText_(ctx, L"关于", lab_fmt,
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
    prim::drawText_(ctx, L"个人资料", h1,
                    ax + 18, ay + 16, 200, 26,
                    br.solidA(pal.text, op));

    float cx = ax + 18, cy = ay + 50;
    float cw = aw - 36, ch = ah - 70;
    prim::drawShadow(ctx, br, cx, cy, cw, ch, 12.0f, pal.shadow_card, op, 2.0f, 3);
    prim::fillRR(ctx, cx, cy, cw, ch, 12.0f, br.solidA(pal.card, op));

    auto field = [&](float fy, const wchar_t* label, const wchar_t* val, bool editable) {
        prim::drawText_(ctx, label, lab_fmt,
                        cx + 16, fy, 100, 14, br.solidA(pal.text_muted, op));
        LayoutRect box{ cx + 90, fy - 4, cw - 110, 26 };
        prim::fillRR(ctx, box.x, box.y, box.w, box.h, 5.0f,
                     br.solidA(editable ? pal.surface : pal.bg, op));
        prim::drawText_(ctx, val, val_fmt,
                        box.x + 8, box.y + 4, box.w - 16, 18,
                        br.solidA(editable ? pal.text : pal.text_faint, op));
    };
    field(cy + 22, L"UID",       g_user.uid.c_str(),      false);
    field(cy + 56, L"用户名",    g_user.username.c_str(), false);
    field(cy + 90, L"昵称",      g_user.nickname.c_str(), true);
    field(cy + 124, L"邮箱",     g_user.email.c_str(),    false);
    field(cy + 158, L"订阅到期", g_user.expires.c_str(),  false);

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
    prim::drawText_(ctx, L"上传头像", btn_fmt,
                    up.x, up.y + up_lift, up.w, up.h,
                    br.solidA(0xFFFFFF, op),
                    DWRITE_TEXT_ALIGNMENT_CENTER,
                    DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    hit(up, [](){
        // 简化：openfilename + 上传留 Step 7 modal 时一起做
    }, true);

    LayoutRect pw{ cx + 156, by, 130, 28 };
    bool pw_hov = pw.contains(g_mouse);
    prim::fillRR(ctx, pw.x, pw.y, pw.w, pw.h, 6.0f,
                 br.solidA(pw_hov ? pal.bg : pal.card, op));
    prim::strokeRR(ctx, pw.x, pw.y, pw.w, pw.h, 6.0f,
                   br.solidA(pal.divider, op));
    prim::drawText_(ctx, L"修改密码", btn_fmt,
                    pw.x, pw.y, pw.w, pw.h,
                    br.solidA(pal.text, op),
                    DWRITE_TEXT_ALIGNMENT_CENTER,
                    DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    hit(pw, [](){
        // ChangePw modal 留 Step 7
    }, true);
}

void paintChatViewPlaceholder(D2DApp& app, float ax, float ay, float aw, float ah) {
    const Palette& pal = palette();
    auto* ctx = app.ctx();
    auto& br = app.brushes();
    float op = fadeOp();
    auto* h1 = app.texts().format(L"Microsoft YaHei UI", ptToDip(22.0f),
                                  DWRITE_FONT_WEIGHT_BOLD);
    auto* sub = app.texts().format(L"Microsoft YaHei UI", ptToDip(10.0f));
    prim::drawText_(ctx, L"聊天", h1,
                    ax + 32, ay + 28, aw - 64, 36, br.solidA(pal.text, op));
    prim::drawText_(ctx, L"Chat view 1759 行 GDI+ 移植中（list/bubble/composer/picker），下一 batch 接通", sub,
                    ax + 32, ay + 72, aw - 64, 22,
                    br.solidA(pal.text_muted, op));
}

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
        case stages::View::Chat:     paintChatViewPlaceholder(app, ax, ay, aw, ah); break;
        case stages::View::Market:   paintMarketView(app, ax, ay, aw, ah);   break;
        case stages::View::Cloud:    paintCloudView(app, ax, ay, aw, ah);    break;
        case stages::View::Settings: paintSettingsView(app, ax, ay, aw, ah); break;
        case stages::View::Profile:  paintProfileView(app, ax, ay, aw, ah);  break;
    }

    // dropdown 在最上层
    paintAccountDropdown(app, W);
    registerDropdownDismissHits(W, H);
}

}  // namespace launcher::d2d::ui
