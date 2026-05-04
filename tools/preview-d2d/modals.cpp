// Modals 实现 — 见 modals.h；简化版，完整版见 GDI+ tools/preview/modals.inl。

#include "modals.h"
#include "icons.h"
#include "palette.h"
#include "user_state.h"
#include "net.h"
#include "hit.h"
#include "stages.h"
#include "fetch.h"
#include "steam.h"
#include "webview.h"
#include "game_assets.h"
#include "chat.h"
#include "sticker.h"
#include "toast.h"
#include "i18n.h"
#include "render/primitives.h"

#include <algorithm>
#include <array>
#include <memory>
#include <mutex>
#include <cstdio>

namespace launcher::d2d::modal {

ChangePwState     g_change_pw;
ConfirmState      g_confirm;
CS2State          g_cs2;
HistoryState      g_history;
AddTagState       g_addtag;
CreatePackState   g_createpack;
RenamePackState   g_renamepack;
UserProfileState  g_user_profile;
EditStatusTextState g_edit_status;
EditBioState      g_edit_bio;
WebViewModalState g_webview_modal;
MsgContextMenuState g_msg_menu;
PackPreviewState  g_pack_preview_modal;

namespace {

float measureW(D2DApp& app, std::wstring_view s, IDWriteTextFormat* fmt) {
    if (s.empty() || !fmt) return 0.0f;
    DWRITE_TEXT_METRICS m{};
    if (!app.texts().measure(fmt, s, 8192.0f, 256.0f, &m)) return 0.0f;
    return m.width;
}

inline uint32_t fadeArgb(uint32_t argb, float op) {
    uint32_t a = (argb >> 24) & 0xFFu;
    a = (uint32_t)(a * op + 0.5f);
    if (a > 255) a = 255;
    return (a << 24) | (argb & 0xFFFFFFu);
}

// 公共 dim 背景
void paintDim(D2DApp& app, float W, float H, float t) {
    const Palette& pal = palette();
    app.ctx()->FillRectangle(D2D1::RectF(0, 0, W, H),
                             app.brushes().solidA(0x000000, 0.40f * t));
    (void)pal;
}

// 通用按钮 — primary 主色 + 立体 press
void drawPrimaryBtn(D2DApp& app, float x, float y, float w, float h,
                    const wchar_t* label, float op,
                    std::function<void()> click, bool danger = false) {
    const Palette& pal = palette();
    auto* ctx = app.ctx();
    auto& br = app.brushes();
    LayoutRect r{ x, y, w, h };
    bool hov = r.contains(g_mouse);
    bool press = hov && g_mouse_pressed;
    float lift = hov && !press ? -1.0f : (press ? 1.0f : 0.0f);
    uint32_t color = danger ? 0xE34B4B : pal.primary;
    uint32_t hover_color = danger ? 0xF06868 : pal.primary_hover;
    prim::drawShadow(ctx, br, x, y + lift + (press ? 0 : 2), w, h, 10.0f,
                     color, op * (press ? 0.12f : 0.28f),
                     press ? 1.0f : 4.0f, press ? 1 : 3);
    prim::fillRR(ctx, x, y + lift, w, h, 10.0f,
                 br.solidA(hov ? hover_color : color, op));
    auto* fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(11.0f),
                                   DWRITE_FONT_WEIGHT_BOLD);
    prim::drawText_(ctx, label, fmt, x, y + lift, w, h,
                    br.solidA(0xFFFFFF, op),
                    DWRITE_TEXT_ALIGNMENT_CENTER,
                    DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    hit(r, std::move(click), true);
}

// 通用 ghost 按钮 — 边框 + 透明 bg
void drawGhostBtn(D2DApp& app, float x, float y, float w, float h,
                  const wchar_t* label, float op,
                  std::function<void()> click) {
    const Palette& pal = palette();
    auto* ctx = app.ctx();
    auto& br = app.brushes();
    LayoutRect r{ x, y, w, h };
    bool hov = r.contains(g_mouse);
    prim::fillRR(ctx, x, y, w, h, 10.0f,
                 br.solidA(hov ? pal.bg : pal.card, op));
    prim::strokeRR(ctx, x, y, w, h, 10.0f, br.solidA(pal.divider, op));
    auto* fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(11.0f),
                                   DWRITE_FONT_WEIGHT_BOLD);
    prim::drawText_(ctx, label, fmt, x, y, w, h,
                    br.solidA(pal.text, op),
                    DWRITE_TEXT_ALIGNMENT_CENTER,
                    DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    hit(r, std::move(click), true);
}

// 通用 InputBox 渲染（短版本，用于 modal field）
void drawField(D2DApp& app, InputBox& box, float x, float y, float w, float h,
               const wchar_t* placeholder, bool focused, float op) {
    const Palette& pal = palette();
    auto* ctx = app.ctx();
    auto& br = app.brushes();
    box.bounds = { x, y, w, h };

    prim::fillRR(ctx, x, y, w, h, 8.0f, br.solidA(pal.bg, op));
    prim::strokeRR(ctx, x, y, w, h, 8.0f,
                   br.solidA(focused ? pal.primary : pal.divider, op),
                   focused ? 1.5f : 1.0f);
    if (focused) {
        prim::strokeRR(ctx, x - 2, y - 2, w + 4, h + 4, 10.0f,
                       br.solidA(pal.primary, op * 0.15f), 4.0f);
    }
    auto* fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(10.0f));
    if (box.text.empty()) {
        prim::drawText_(ctx, placeholder, fmt, x + 12, y + 9, w - 24, 18,
                        br.solidA(pal.text_muted, op));
    } else {
        if (focused && box.hasSelection()) {
            float pre_w = measureW(app, box.displaySlice(0, box.selStart()), fmt);
            float in_w = measureW(app, box.displaySlice(box.selStart(), box.selEnd()), fmt);
            prim::fillRect(ctx, x + 12 + pre_w, y + 8, in_w, 18,
                           br.solidA(pal.primary, op * 0.38f));
        }
        prim::drawText_(ctx, box.display(), fmt, x + 12, y + 9, w - 24, 18,
                        br.solidA(pal.text, op));
    }
    if (focused && !box.hasSelection()) {
        float pre_w = measureW(app, box.displaySlice(0, box.cursor), fmt);
        int phase = (int)(stages::g_time_in_stage * 1000) % 1000;
        if (phase < 500) {
            // caret 高度固定 18px (跟字号匹配)，不跟 ih 走 — 防止大 textarea
            // (h=140) 时 caret 画 110px 长竖线
            float cx = x + 12 + pre_w;
            float cy_top = y + 10;
            float cy_bot = cy_top + 18.0f;
            if (cy_bot > y + h - 4) cy_bot = y + h - 4;
            prim::drawLine(ctx, cx, cy_top, cx, cy_bot,
                           br.solidA(pal.primary, op), 1.5f);
        }
    }
}

// === ChangePw 真后端 ===
struct ChangePwArg {
    std::wstring old_pw, new_pw;
    HWND hwnd;
};
std::mutex g_pw_mtx;
std::wstring g_pw_pending_error;

}  // anon

void tickAll(float dt) {
    g_change_pw.t.tick(dt);
    g_confirm.t.tick(dt);
    g_cs2.t.tick(dt);
    g_history.t.tick(dt);
    g_addtag.t.tick(dt);
    g_addtag.input.float_t.tick(dt);
    g_createpack.t.tick(dt);
    g_createpack.input.float_t.tick(dt);
    g_renamepack.t.tick(dt);
    g_renamepack.input.float_t.tick(dt);
    g_user_profile.t.tick(dt);
    g_edit_status.t.tick(dt);
    g_edit_status.input.float_t.tick(dt);
    g_edit_bio.t.tick(dt);
    g_edit_bio.input.float_t.tick(dt);
    g_webview_modal.t.tick(dt);
    g_msg_menu.t.tick(dt);
    g_pack_preview_modal.t.tick(dt);
}

bool anyOpen() {
    return g_change_pw.open || g_confirm.open || g_cs2.open || g_history.open
        || g_addtag.open || g_createpack.open || g_renamepack.open
        || g_user_profile.open || g_edit_status.open || g_edit_bio.open
        || g_webview_modal.open || g_msg_menu.open || g_pack_preview_modal.open;
}

// ============== ChangePw ==============
void openChangePw() {
    g_change_pw.open = true;
    g_change_pw.old_pw.password = true;
    g_change_pw.new_pw.password = true;
    g_change_pw.repeat_pw.password = true;
    g_change_pw.old_pw.text.clear();
    g_change_pw.new_pw.text.clear();
    g_change_pw.repeat_pw.text.clear();
    g_change_pw.focus = 0;
    g_change_pw.error_msg.clear();
    g_change_pw.t.start(0, 1, 0.22f, 0, curve::easeOutCubic);
}

static void closeChangePw() {
    g_change_pw.t.start(g_change_pw.t.value(), 0, 0.18f, 0, curve::easeOutCubic);
    g_change_pw.open = false;
}

static void submitChangePw(HWND hwnd) {
    if (g_change_pw.busy) return;
    if (g_change_pw.old_pw.text.empty() || g_change_pw.new_pw.text.empty()) {
        g_change_pw.error_msg = L"密码不能为空"; return;
    }
    if (g_change_pw.new_pw.text != g_change_pw.repeat_pw.text) {
        g_change_pw.error_msg = L"两次新密码不一致"; return;
    }
    if (g_change_pw.new_pw.text.size() < 6) {
        g_change_pw.error_msg = L"新密码至少 6 位"; return;
    }
    g_change_pw.error_msg.clear();
    g_change_pw.busy = true;
    auto* a = new ChangePwArg{ g_change_pw.old_pw.text, g_change_pw.new_pw.text, hwnd };
    CreateThread(nullptr, 0, [](LPVOID lp) -> DWORD {
        std::unique_ptr<ChangePwArg> a((ChangePwArg*)lp);
        std::string body = "{\"session_token\":\"" + g_session_token
                         + "\",\"old_password\":\"" + net::jsonEscape(a->old_pw)
                         + "\",\"new_password\":\"" + net::jsonEscape(a->new_pw) + "\"}";
        auto resp = net::postJson(L"/api/profile/password", body);
        {
            std::lock_guard<std::mutex> lk(g_pw_mtx);
            if (resp.ok()) g_pw_pending_error.clear();
            else {
                std::string m = net::jsonStr(resp.body, "error");
                if (m.empty()) m = "改密码失败";
                int n = MultiByteToWideChar(CP_UTF8, 0, m.c_str(), -1, nullptr, 0);
                std::wstring w(n > 0 ? n - 1 : 0, 0);
                if (n > 0) MultiByteToWideChar(CP_UTF8, 0, m.c_str(), -1, w.data(), n);
                g_pw_pending_error = w;
            }
        }
        PostMessageW(a->hwnd, WM_APP + 4, resp.ok() ? 1 : 0, 0);
        return 0;
    }, a, 0, nullptr);
}

void onChangePwResult(bool success) {
    std::lock_guard<std::mutex> lk(g_pw_mtx);
    g_change_pw.busy = false;
    if (success) {
        // 修改成功 — 强制重登
        g_session_token.clear();
        g_user_id.clear();
        closeChangePw();
        stages::g_stage = stages::Stage::Auth;
        stages::g_auth_card_op.start(0, 1, 0.40f, 0.05f, curve::easeOutCubic);
        stages::g_auth_card_y.start(12, 0, 0.45f, 0.05f, curve::easeOutQuint);
    } else {
        g_change_pw.error_msg = g_pw_pending_error.empty()
            ? L"改密码失败" : g_pw_pending_error;
    }
}

void paintChangePwModal(D2DApp& app, float W, float H) {
    if (!g_change_pw.open && g_change_pw.t.value() < 0.001f) return;
    float t = g_change_pw.t.value();
    if (t < 0.001f) return;
    paintDim(app, W, H, t);

    const Palette& pal = palette();
    auto* ctx = app.ctx();
    auto& br = app.brushes();

    float cw = 380, ch = 360;
    float cx = (W - cw) * 0.5f, cy = (H - ch) * 0.5f;
    prim::drawShadow(ctx, br, cx, cy, cw, ch, 16.0f, pal.shadow_card_hover, t, 6.0f, 4);
    prim::fillRR(ctx, cx, cy, cw, ch, 16.0f, br.solidA(pal.card, t));

    auto* h1 = app.texts().format(L"Microsoft YaHei UI", ptToDip(15.0f),
                                  DWRITE_FONT_WEIGHT_BOLD);
    auto* sub = app.texts().format(L"Microsoft YaHei UI", ptToDip(9.5f));
    prim::drawText_(ctx, L"修改密码", h1,
                    cx + 30, cy + 28, cw - 60, 24,
                    br.solidA(pal.text, t));
    prim::drawText_(ctx, L"修改成功后将被踢回登录", sub,
                    cx + 30, cy + 56, cw - 60, 18,
                    br.solidA(pal.text_muted, t));

    float fy = cy + 90;
    drawField(app, g_change_pw.old_pw, cx + 30, fy, cw - 60, 40,
              L"当前密码", g_change_pw.focus == 0, t);
    fy += 50;
    drawField(app, g_change_pw.new_pw, cx + 30, fy, cw - 60, 40,
              L"新密码 (至少 6 位)", g_change_pw.focus == 1, t);
    fy += 50;
    drawField(app, g_change_pw.repeat_pw, cx + 30, fy, cw - 60, 40,
              L"重复新密码", g_change_pw.focus == 2, t);
    fy += 56;

    hit(g_change_pw.old_pw.bounds, [](){ g_change_pw.focus = 0;
        g_change_pw.new_pw.clearSel(); g_change_pw.repeat_pw.clearSel(); }, true);
    hit(g_change_pw.new_pw.bounds, [](){ g_change_pw.focus = 1;
        g_change_pw.old_pw.clearSel(); g_change_pw.repeat_pw.clearSel(); }, true);
    hit(g_change_pw.repeat_pw.bounds, [](){ g_change_pw.focus = 2;
        g_change_pw.old_pw.clearSel(); g_change_pw.new_pw.clearSel(); }, true);

    // 错误
    if (!g_change_pw.error_msg.empty()) {
        prim::fillRR(ctx, cx + 30, fy - 4, cw - 60, 26, 8.0f,
                     br.solidA(0xE34B4B, t * 0.11f));
        prim::drawText_(ctx, g_change_pw.error_msg, sub,
                        cx + 40, fy, cw - 80, 18,
                        br.solidA(0xFF8A80, t));
        fy += 32;
    }

    // 按钮
    float by = cy + ch - 56;
    drawGhostBtn(app, cx + 30, by, 130, 38, L"取消", t,
                 [](){ closeChangePw(); });
    drawPrimaryBtn(app, cx + cw - 30 - 200, by, 200, 38,
                   g_change_pw.busy ? L"提交中…" : L"提交", t,
                   [hwnd = GetActiveWindow()](){ submitChangePw(hwnd); });
}

// ============== Confirm ==============
void openConfirm(const std::wstring& title, const std::wstring& msg,
                 std::function<void()> on_yes,
                 const std::wstring& yes_label,
                 const std::wstring& no_label,
                 bool danger) {
    g_confirm.open = true;
    g_confirm.title = title;
    g_confirm.msg = msg;
    g_confirm.yes_label = yes_label;
    g_confirm.no_label = no_label;
    g_confirm.on_yes = std::move(on_yes);
    g_confirm.danger = danger;
    g_confirm.t.start(0, 1, 0.22f, 0, curve::easeOutCubic);
}
static void closeConfirm() {
    g_confirm.t.start(g_confirm.t.value(), 0, 0.18f, 0, curve::easeOutCubic);
    g_confirm.open = false;
}

void paintConfirmModal(D2DApp& app, float W, float H) {
    if (!g_confirm.open && g_confirm.t.value() < 0.001f) return;
    float t = g_confirm.t.value();
    if (t < 0.001f) return;
    paintDim(app, W, H, t);

    const Palette& pal = palette();
    auto* ctx = app.ctx();
    auto& br = app.brushes();

    float cw = 360, ch = 200;
    float cx = (W - cw) * 0.5f, cy = (H - ch) * 0.5f;
    prim::drawShadow(ctx, br, cx, cy, cw, ch, 16.0f, pal.shadow_card_hover, t, 6.0f, 4);
    prim::fillRR(ctx, cx, cy, cw, ch, 16.0f, br.solidA(pal.card, t));

    auto* h1 = app.texts().format(L"Microsoft YaHei UI", ptToDip(13.0f),
                                  DWRITE_FONT_WEIGHT_BOLD);
    auto* sub = app.texts().format(L"Microsoft YaHei UI", ptToDip(10.0f));
    prim::drawText_(ctx, g_confirm.title, h1,
                    cx + 30, cy + 26, cw - 60, 24,
                    br.solidA(pal.text, t));
    prim::drawText_(ctx, g_confirm.msg, sub,
                    cx + 30, cy + 56, cw - 60, 60,
                    br.solidA(pal.text_muted, t));

    float by = cy + ch - 52;
    drawGhostBtn(app, cx + 30, by, 120, 36,
                 g_confirm.no_label.c_str(), t,
                 [](){ closeConfirm(); });
    drawPrimaryBtn(app, cx + cw - 30 - 160, by, 160, 36,
                   g_confirm.yes_label.c_str(), t,
                   [](){
                       auto cb = g_confirm.on_yes;
                       closeConfirm();
                       if (cb) cb();
                   }, g_confirm.danger);
}

// ============== CS2 ==============
void openCS2() {
    g_cs2.open = true;
    g_cs2.t.start(0, 1, 0.30f, 0, curve::easeOutQuint);
    // 启动 WebView2 嵌入 Steam store widget（含 mp4 视频自动播放）
    if (webview::ensure(GetActiveWindow())) {
        webview::navigate(L"https://store.steampowered.com/widget/730/embed/");
    }
}
static void closeCS2() {
    g_cs2.t.start(g_cs2.t.value(), 0, 0.20f, 0, curve::easeOutQuint);
    g_cs2.open = false;
    webview::show(false);   // 立即隐藏，stop 视频继续覆盖窗口
}

void paintCS2Modal(D2DApp& app, float W, float H) {
    if (!g_cs2.open && g_cs2.t.value() < 0.001f) return;
    float t = g_cs2.t.value();
    if (t < 0.001f) return;
    paintDim(app, W, H, t);

    const Palette& pal = palette();
    auto* ctx = app.ctx();
    auto& br = app.brushes();

    float cw = 720, ch = 560;       // 加大让 store widget 装下
    float cx = (W - cw) * 0.5f, cy = (H - ch) * 0.5f;
    prim::drawShadow(ctx, br, cx, cy, cw, ch, 16.0f, pal.shadow_card_hover, t, 6.0f, 5);
    prim::fillRR(ctx, cx, cy, cw, ch, 16.0f, br.solidA(pal.card, t));

    // 标题栏 44px — ✕ 在右上角；WebView2 子窗口不会覆盖这里 (z-order 高但 bounds 不到这)
    float bar_h = 44.0f;
    auto* bar_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(11.0f),
                                       DWRITE_FONT_WEIGHT_BOLD);
    prim::drawText_(ctx, L"Counter-Strike 2", bar_fmt,
                    cx + 18, cy + 11, 300, 22,
                    br.solidA(pal.text, t));
    auto* bar_sub = app.texts().format(L"Microsoft YaHei UI", ptToDip(8.5f));
    prim::drawText_(ctx, L"Valve · Source 2", bar_sub,
                    cx + 18 + 130, cy + 14, 200, 18,
                    br.solidA(pal.text_muted, t));
    prim::fillRect(ctx, cx, cy + bar_h - 1, cw, 1, br.solidA(pal.divider, t));

    // ✕ 在标题栏右上 (bar_h 内，WebView 不覆盖)
    {
        LayoutRect xb{ cx + cw - 36, cy + 8, 28, 28 };
        bool xh = xb.contains(g_mouse);
        if (xh) {
            prim::fillRR(ctx, xb.x, xb.y, 28, 28, 6,
                         br.solidA(pal.text, t * 0.10f));
        }
        icons::drawIcon(app, icons::Name::X, xb.x + 6, xb.y + 6, 16,
                        fadeArgb(pal.text, t));
        hit(xb, [](){ closeCS2(); }, true);
    }

    // 顶部 cover 区域 — 在标题栏下方 (cy+bar_h)
    float cover_y = cy + bar_h;
    float cover_h = 280;            // 加大视频区
    bool wv_ready = webview::isReady();
    if (wv_ready && t > 0.95f) {
        float scale = app.dpi() / 96.0f;
        int wl = (int)((cx + 8) * scale);
        int wt = (int)(cover_y * scale);
        int wr = (int)((cx + cw - 8) * scale);
        int wb = (int)((cover_y + cover_h) * scale);
        webview::setBounds(wl, wt, wr, wb);
        webview::show(true);
        prim::fillRR(ctx, cx + 8, cover_y, cw - 16, cover_h, 8.0f,
                     br.solidA(pal.surface, t));
    } else {
        webview::show(false);
        auto cs2_path = cs2HeaderPath();
        auto* cover_bmp = cs2_path.empty() ? nullptr : app.images().fromFile(cs2_path);
        if (cover_bmp) {
            prim::pushLayerRR(ctx, app.factory(), cx + 8, cover_y, cw - 16, cover_h, 8.0f);
            D2D1_SIZE_F sz = cover_bmp->GetSize();
            float scale_ = (std::max)((cw - 16) / sz.width, cover_h / sz.height);
            float dw = sz.width * scale_, dh = sz.height * scale_;
            float dx = cx + 8 + ((cw - 16) - dw) * 0.5f;
            float dy = cover_y + (cover_h - dh) * 0.5f;
            ctx->DrawBitmap(cover_bmp, D2D1::RectF(dx, dy, dx + dw, dy + dh),
                            t, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            prim::fillRect(ctx, cx + 8, cover_y + cover_h - 60, cw - 16, 60,
                           br.solidA(0x000000, 0.55f * t));
            prim::popLayer(ctx);
        } else {
            prim::fillRR(ctx, cx + 8, cover_y, cw - 16, cover_h, 8.0f,
                         br.solidA(0xC96442, t));
        }
        auto* sub_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(9.5f));
        prim::drawText_(ctx, webview::runtimeAvailable() ? L"视频加载中…" :
                              L"WebView2 Runtime 未装 — 装 Edge 即可看视频",
                        sub_fmt,
                        cx + 24, cover_y + cover_h - 28, cw - 48, 18,
                        br.solidA(0xFFFFFF, t * 0.85f));
    }

    // 底部 stat
    auto* stat_lbl = app.texts().format(L"Microsoft YaHei UI", ptToDip(8.0f));
    auto* stat_val = app.texts().format(L"Microsoft YaHei UI", ptToDip(11.0f),
                                        DWRITE_FONT_WEIGHT_BOLD);
    struct S { const wchar_t* l; std::wstring v; };
    S stats[] = {
        { L"账号",   g_steam.persona.empty() ? std::wstring(L"未登录 Steam") : g_steam.persona },
        { L"最近玩", g_steam.last_played.empty() ? std::wstring(L"--") : g_steam.last_played },
        { L"总时长", g_steam.playtime_label.empty() ? std::wstring(L"--") : g_steam.playtime_label },
    };
    float sx = cx + 24, sy = cover_y + cover_h + 16;
    for (auto& s : stats) {
        prim::drawText_(ctx, s.l, stat_lbl,
                        sx, sy, 160, 14, br.solidA(pal.text_muted, t));
        prim::drawText_(ctx, s.v.c_str(), stat_val,
                        sx, sy + 18, 160, 22, br.solidA(pal.text, t));
        sx += 220;
    }

    drawPrimaryBtn(app, cx + 24, cy + ch - 56, 200, 40,
                   L"启动 CS2", t, [](){ launchCS2(); });
    drawGhostBtn(app, cx + 240, cy + ch - 56, 160, 40,
                 L"商店页面", t, [](){ openCS2Store(); });
}

// ============== History ==============
void openHistory() {
    g_history.open = true;
    g_history.t.start(0, 1, 0.25f, 0, curve::easeOutCubic);
    if (!g_session_token.empty()) {
        fetch::loginHistory(GetActiveWindow());
    }
}
static void closeHistory() {
    g_history.t.start(g_history.t.value(), 0, 0.18f, 0, curve::easeOutCubic);
    g_history.open = false;
}

void paintHistoryModal(D2DApp& app, float W, float H) {
    if (!g_history.open && g_history.t.value() < 0.001f) return;
    float t = g_history.t.value();
    if (t < 0.001f) return;
    paintDim(app, W, H, t);

    const Palette& pal = palette();
    auto* ctx = app.ctx();
    auto& br = app.brushes();

    float cw = 460, ch = 420;
    float cx = (W - cw) * 0.5f, cy = (H - ch) * 0.5f + 8 * (1.0f - t);
    prim::drawShadow(ctx, br, cx, cy, cw, ch, 16.0f, pal.shadow_card_hover, t, 6.0f, 4);
    prim::fillRR(ctx, cx, cy, cw, ch, 16.0f, br.solidA(pal.card, t));

    auto* h1 = app.texts().format(L"Microsoft YaHei UI", ptToDip(13.0f),
                                  DWRITE_FONT_WEIGHT_BOLD);
    auto* sub = app.texts().format(L"Microsoft YaHei UI", ptToDip(9.0f));
    prim::drawText_(ctx, L"登录历史", h1,
                    cx + 24, cy + 22, cw - 48, 22,
                    br.solidA(pal.text, t));
    if (!g_history.loaded) {
        prim::drawText_(ctx, L"加载中…", sub,
                        cx + 24, cy + 50, cw - 48, 18,
                        br.solidA(pal.text_muted, t));
    } else {
        wchar_t info[64];
        swprintf_s(info, L"共 %d 条记录 · 5 条/页", (int)g_history.rows.size());
        prim::drawText_(ctx, info, sub,
                        cx + 24, cy + 50, cw - 48, 18,
                        br.solidA(pal.text_muted, t));
    }

    auto* row_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(9.5f));
    int per = 5;
    int total = (int)g_history.rows.size();
    int total_pages = total > 0 ? (total + per - 1) / per : 1;
    if (g_history.page >= total_pages) g_history.page = total_pages - 1;
    if (g_history.page < 0) g_history.page = 0;
    int begin = g_history.page * per;
    int end = (std::min)(begin + per, total);

    if (total == 0) {
        // 占位 5 行
        for (int i = 0; i < 5; ++i) {
            float ry = cy + 90 + i * 50;
            prim::fillRR(ctx, cx + 20, ry, cw - 40, 40, 8.0f,
                         br.solidA(pal.surface, t * 0.6f));
        }
    } else {
        for (int i = begin; i < end; ++i) {
            float ry = cy + 90 + (i - begin) * 50;
            prim::fillRR(ctx, cx + 20, ry, cw - 40, 40, 8.0f,
                         br.solidA(pal.surface, t));
            prim::fillCircle(ctx, cx + 36, ry + 20, 6,
                             br.solidA(0x4ADE80, t));
            prim::drawText_(ctx, g_history.rows[i], row_fmt,
                            cx + 56, ry + 11, cw - 96, 18,
                            br.solidA(pal.text, t));
        }
    }
    // 翻页 ‹ ›
    if (total_pages > 1) {
        wchar_t pg[16]; swprintf_s(pg, L"%d / %d", g_history.page + 1, total_pages);
        prim::drawText_(ctx, pg, sub,
                        cx + cw * 0.5f - 30, cy + ch - 90, 60, 18,
                        br.solidA(pal.text_muted, t),
                        DWRITE_TEXT_ALIGNMENT_CENTER);
        drawGhostBtn(app, cx + cw * 0.5f - 90, cy + ch - 95, 30, 30, L"‹", t,
                     [](){ if (g_history.page > 0) g_history.page--; });
        drawGhostBtn(app, cx + cw * 0.5f + 60, cy + ch - 95, 30, 30, L"›", t,
                     [](){ g_history.page++; });
    }

    drawGhostBtn(app, cx + cw - 24 - 100, cy + ch - 52, 100, 36,
                 L"关闭", t, [](){ closeHistory(); });

    // 真接通 — 占位 5 行用 g_history.rows 替代（如果已加载）
    if (g_history.loaded && !g_history.rows.empty()) {
        // 简单分页 5/页
        // 已经画了占位 5 行，这里覆盖：用 rows 显示
    }
}

void onHistoryResult(const std::string& body) {
    g_history.rows.clear();
    g_history.page = 0;
    g_history.loaded = true;
    // 简单解析 [{"ts":"...","ok":true,"ip":"...","geo":"..."}, ...]
    size_t pos = 0;
    auto utf8w = [](const std::string& s) -> std::wstring {
        if (s.empty()) return {};
        int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
        if (n <= 0) return {};
        std::wstring w(n - 1, 0);
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
        return w;
    };
    while (true) {
        auto ob = body.find('{', pos);
        if (ob == std::string::npos) break;
        auto cb = body.find('}', ob);
        if (cb == std::string::npos) break;
        std::string obj = body.substr(ob, cb - ob + 1);
        std::string ts = net::jsonStr(obj, "ts");
        std::string ip = net::jsonStr(obj, "ip");
        std::string geo = net::jsonStr(obj, "geo");
        // ts 可能是 "2026-05-04T..." — 取前 16 字节够
        if (ts.size() > 16) ts.resize(16);
        for (auto& c : ts) if (c == 'T') c = ' ';
        std::wstring row = utf8w(ts) + L"  ·  " + utf8w(ip);
        if (!geo.empty()) row += L"  ·  " + utf8w(geo);
        g_history.rows.push_back(row);
        pos = cb + 1;
    }
}

// ============== AddTag ==============
void openAddTag() {
    g_addtag.open = true;
    g_addtag.input.text.clear();
    g_addtag.input.cursor = 0;
    g_addtag.input.clearSel();
    g_addtag.error_msg.clear();
    g_addtag.busy = false;
    g_addtag.t.start(0, 1, 0.22f, 0, curve::easeOutCubic);
}
static void closeAddTag() {
    g_addtag.t.start(g_addtag.t.value(), 0, 0.18f, 0, curve::easeOutCubic);
    g_addtag.open = false;
}

static void submitAddTag(HWND hwnd) {
    if (g_addtag.busy) return;
    std::wstring tag = g_addtag.input.text;
    while (!tag.empty() && (tag.front() == L' ' || tag.front() == L'\t')) tag.erase(tag.begin());
    while (!tag.empty() && (tag.back()  == L' ' || tag.back()  == L'\t')) tag.pop_back();
    if (tag.empty()) { g_addtag.error_msg = L"请输入标签内容"; return; }
    if (tag.size() > 24) { g_addtag.error_msg = L"最多 24 字"; return; }
    g_addtag.error_msg.clear();
    g_addtag.busy = true;
    fetch::addTag(hwnd, tag);
}

void onAddTagResult(bool success, int status) {
    g_addtag.busy = false;
    if (success) {
        closeAddTag();
        // tag list 重拉
        fetch::userTags(GetActiveWindow());
    } else {
        if (status == 409) g_addtag.error_msg = L"标签已存在";
        else if (status == 429) g_addtag.error_msg = L"标签数量上限 (20)";
        else g_addtag.error_msg = L"添加失败，稍后再试";
    }
}

void paintAddTagModal(D2DApp& app, float W, float H) {
    if (!g_addtag.open && g_addtag.t.value() < 0.001f) return;
    float t = g_addtag.t.value();
    if (t < 0.001f) return;
    paintDim(app, W, H, t);

    const Palette& pal = palette();
    auto* ctx = app.ctx();
    auto& br = app.brushes();

    float cw = 360, ch = 220;
    float cx = (W - cw) * 0.5f, cy = (H - ch) * 0.5f;
    prim::drawShadow(ctx, br, cx, cy, cw, ch, 16.0f, pal.shadow_card_hover, t, 6.0f, 4);
    prim::fillRR(ctx, cx, cy, cw, ch, 16.0f, br.solidA(pal.card, t));

    auto* h1 = app.texts().format(L"Microsoft YaHei UI", ptToDip(13.0f),
                                  DWRITE_FONT_WEIGHT_BOLD);
    auto* sub = app.texts().format(L"Microsoft YaHei UI", ptToDip(9.0f));
    prim::drawText_(ctx, L"添加标签", h1,
                    cx + 30, cy + 22, cw - 60, 22,
                    br.solidA(pal.text, t));
    prim::drawText_(ctx, L"24 字以内 · 上限 20 个", sub,
                    cx + 30, cy + 50, cw - 60, 18,
                    br.solidA(pal.text_muted, t));

    drawField(app, g_addtag.input, cx + 30, cy + 80, cw - 60, 40,
              L"标签内容", true, t);
    hit(g_addtag.input.bounds, [](){}, true);

    if (!g_addtag.error_msg.empty()) {
        prim::drawText_(ctx, g_addtag.error_msg, sub,
                        cx + 30, cy + 130, cw - 60, 18,
                        br.solidA(0xE34B4B, t));
    }

    float by = cy + ch - 52;
    drawGhostBtn(app, cx + 30, by, 120, 36, L"取消", t,
                 [](){ closeAddTag(); });
    drawPrimaryBtn(app, cx + cw - 30 - 160, by, 160, 36,
                   g_addtag.busy ? L"添加中…" : L"添加", t,
                   [hwnd = GetActiveWindow()](){ submitAddTag(hwnd); });
}

// ============== CreatePack ==============
void openCreatePack() {
    g_createpack.open = true;
    g_createpack.input.text.clear();
    g_createpack.input.cursor = 0;
    g_createpack.input.clearSel();
    g_createpack.error_msg.clear();
    g_createpack.busy = false;
    g_createpack.t.start(0, 1, 0.22f, 0, curve::easeOutCubic);
}
static void closeCreatePack() {
    g_createpack.t.start(g_createpack.t.value(), 0, 0.18f, 0, curve::easeOutCubic);
    g_createpack.open = false;
}
static void submitCreatePack(HWND hwnd) {
    if (g_createpack.busy) return;
    std::wstring name = g_createpack.input.text;
    if (name.empty()) { g_createpack.error_msg = L"请输入分组名"; return; }
    if (name.size() > 24) { g_createpack.error_msg = L"最多 24 字"; return; }
    if (g_session_token.empty()) { g_createpack.error_msg = L"未登录"; return; }
    g_createpack.busy = true;
    struct A { std::wstring n; HWND h; };
    auto* a = new A{ name, hwnd };
    CreateThread(nullptr, 0, [](LPVOID lp) -> DWORD {
        std::unique_ptr<A> a((A*)lp);
        std::string body = "{\"session_token\":\"" + g_session_token
                         + "\",\"name\":\"" + net::jsonEscape(a->n) + "\"}";
        auto r = net::postJson(L"/api/sticker/pack", body);
        PostMessageW(a->h, WM_APP + 19, r.ok() ? 1 : 0, 0);
        return 0;
    }, a, 0, nullptr);
}
void onCreatePackResult(bool success) {
    g_createpack.busy = false;
    if (success) closeCreatePack();
    else g_createpack.error_msg = L"创建失败";
}
void paintCreatePackModal(D2DApp& app, float W, float H) {
    if (!g_createpack.open && g_createpack.t.value() < 0.001f) return;
    float t = g_createpack.t.value();
    if (t < 0.001f) return;
    paintDim(app, W, H, t);

    const Palette& pal = palette();
    auto* ctx = app.ctx();
    auto& br = app.brushes();
    float cw = 360, ch = 220;
    float cx = (W - cw) * 0.5f, cy = (H - ch) * 0.5f;
    prim::drawShadow(ctx, br, cx, cy, cw, ch, 16.0f, pal.shadow_card_hover, t, 6.0f, 4);
    prim::fillRR(ctx, cx, cy, cw, ch, 16.0f, br.solidA(pal.card, t));

    auto* h1 = app.texts().format(L"Microsoft YaHei UI", ptToDip(13.0f), DWRITE_FONT_WEIGHT_BOLD);
    auto* sub = app.texts().format(L"Microsoft YaHei UI", ptToDip(9.0f));
    prim::drawText_(ctx, L"新建表情包", h1, cx + 30, cy + 22, cw - 60, 22, br.solidA(pal.text, t));
    prim::drawText_(ctx, L"分组名 · 24 字以内", sub, cx + 30, cy + 50, cw - 60, 18,
                    br.solidA(pal.text_muted, t));
    drawField(app, g_createpack.input, cx + 30, cy + 80, cw - 60, 40, L"分组名", true, t);
    hit(g_createpack.input.bounds, [](){}, true);
    if (!g_createpack.error_msg.empty()) {
        prim::drawText_(ctx, g_createpack.error_msg, sub,
                        cx + 30, cy + 130, cw - 60, 18,
                        br.solidA(0xE34B4B, t));
    }
    float by = cy + ch - 52;
    drawGhostBtn(app, cx + 30, by, 120, 36, L"取消", t, [](){ closeCreatePack(); });
    drawPrimaryBtn(app, cx + cw - 30 - 160, by, 160, 36,
                   g_createpack.busy ? L"创建中…" : L"创建", t,
                   [hwnd = GetActiveWindow()](){ submitCreatePack(hwnd); });
}

// ============== RenamePack ==============
void openRenamePack(const std::string& pack_id, const std::wstring& orig_name) {
    g_renamepack.open = true;
    g_renamepack.pack_id = pack_id;
    g_renamepack.orig_name = orig_name;
    g_renamepack.input.text = orig_name;
    g_renamepack.input.cursor = (int)orig_name.size();
    g_renamepack.input.clearSel();
    g_renamepack.error_msg.clear();
    g_renamepack.busy = false;
    g_renamepack.t.start(0, 1, 0.22f, 0, curve::easeOutCubic);
}
static void closeRenamePack() {
    g_renamepack.t.start(g_renamepack.t.value(), 0, 0.18f, 0, curve::easeOutCubic);
    g_renamepack.open = false;
}
static void submitRenamePack(HWND hwnd) {
    if (g_renamepack.busy) return;
    std::wstring name = g_renamepack.input.text;
    if (name.empty()) { g_renamepack.error_msg = L"分组名不能为空"; return; }
    if (name.size() > 24) { g_renamepack.error_msg = L"最多 24 字"; return; }
    g_renamepack.busy = true;
    struct A { std::string id; std::wstring n; HWND h; };
    auto* a = new A{ g_renamepack.pack_id, name, hwnd };
    CreateThread(nullptr, 0, [](LPVOID lp) -> DWORD {
        std::unique_ptr<A> a((A*)lp);
        std::string body = "{\"session_token\":\"" + g_session_token
                         + "\",\"pack_id\":\"" + a->id
                         + "\",\"new_name\":\"" + net::jsonEscape(a->n) + "\"}";
        auto r = net::postJson(L"/api/sticker/pack/rename", body);
        PostMessageW(a->h, WM_APP + 20, r.ok() ? 1 : 0, 0);
        return 0;
    }, a, 0, nullptr);
}
void onRenamePackResult(bool success) {
    g_renamepack.busy = false;
    if (success) closeRenamePack();
    else g_renamepack.error_msg = L"重命名失败";
}
// ============== UserProfile modal ==============
void openUserProfile(const std::wstring& uid_or_nickname) {
    g_user_profile.open = true;
    g_user_profile.t.start(0, 1, 0.25f, 0, curve::easeOutCubic);
    fetch::peerProfile(GetActiveWindow(), uid_or_nickname);
}
static void closeUserProfile() {
    g_user_profile.t.start(g_user_profile.t.value(), 0, 0.18f, 0, curve::easeOutCubic);
    g_user_profile.open = false;
}
void paintUserProfileModal(D2DApp& app, float W, float H) {
    if (!g_user_profile.open && g_user_profile.t.value() < 0.001f) return;
    float t = g_user_profile.t.value();
    if (t < 0.001f) return;
    paintDim(app, W, H, t);

    const Palette& pal = palette();
    auto* ctx = app.ctx();
    auto& br = app.brushes();
    float cw = 380, ch = 460;
    float cx = (W - cw) * 0.5f, cy = (H - ch) * 0.5f;
    prim::drawShadow(ctx, br, cx, cy, cw, ch, 16.0f, pal.shadow_card_hover, t, 6.0f, 4);
    prim::fillRR(ctx, cx, cy, cw, ch, 16.0f, br.solidA(pal.card, t));

    // 头部主色 banner
    prim::fillRR(ctx, cx, cy, cw, 100, 16.0f, br.solidA(pal.primary, t));
    prim::fillRect(ctx, cx, cy + 60, cw, 40, br.solidA(pal.primary, t));

    // 头像（用 peer.avatar_path 或占位）
    fetch::PeerProfile peer;
    {
        std::lock_guard<std::mutex> lk(fetch::g_peer_mtx);
        peer = fetch::g_peer;
    }
    float ar = 36.0f;
    float ax = cx + (cw - ar * 2) * 0.5f, ay = cy + 60;
    prim::fillCircle(ctx, ax + ar, ay + ar, ar + 4, br.solidA(pal.card, t));
    prim::fillCircle(ctx, ax + ar, ay + ar, ar, br.solidA(pal.primary_hover, t));
    auto* init_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(20.0f),
                                         DWRITE_FONT_WEIGHT_BOLD);
    wchar_t init[2] = { (wchar_t)towupper(peer.nickname.empty()
                        ? (peer.uid.empty() ? L'?' : peer.uid[0])
                        : peer.nickname[0]), 0 };
    prim::drawText_(ctx, init, init_fmt,
                    ax, ay, ar * 2, ar * 2,
                    br.solidA(0xFFFFFF, t),
                    DWRITE_TEXT_ALIGNMENT_CENTER,
                    DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

    auto* h1 = app.texts().format(L"Microsoft YaHei UI", ptToDip(15.0f),
                                  DWRITE_FONT_WEIGHT_BOLD);
    auto* sub = app.texts().format(L"Microsoft YaHei UI", ptToDip(9.5f));
    auto* mt = app.texts().format(L"Microsoft YaHei UI", ptToDip(8.0f));

    if (!peer.loaded) {
        prim::drawText_(ctx, L"加载中…", sub,
                        cx, cy + 200, cw, 20,
                        br.solidA(pal.text_muted, t),
                        DWRITE_TEXT_ALIGNMENT_CENTER);
    } else if (!peer.err.empty()) {
        prim::drawText_(ctx, L"获取失败", h1,
                        cx, cy + 180, cw, 24,
                        br.solidA(pal.text, t),
                        DWRITE_TEXT_ALIGNMENT_CENTER);
        prim::drawText_(ctx, peer.err, sub,
                        cx + 20, cy + 210, cw - 40, 40,
                        br.solidA(pal.text_muted, t),
                        DWRITE_TEXT_ALIGNMENT_CENTER);
    } else {
        // nickname
        prim::drawText_(ctx, peer.nickname.empty() ? peer.uid : peer.nickname, h1,
                        cx, cy + 150, cw, 26,
                        br.solidA(pal.text, t),
                        DWRITE_TEXT_ALIGNMENT_CENTER);
        // 在线状态 dot + label (status: online/busy/away/sleep/offline)
        {
            const wchar_t* st_label = L"离线";
            uint32_t st_color = 0xFF6B6A67;
            if (peer.status == L"online")  { st_label = L"在线"; st_color = 0xFF4ADE80; }
            else if (peer.status == L"busy"){ st_label = L"繁忙"; st_color = 0xFFE34B4B; }
            else if (peer.status == L"away"){ st_label = L"离开"; st_color = 0xFFF5A524; }
            else if (peer.status == L"sleep"){st_label = L"睡眠"; st_color = 0xFF8B7BD9; }
            float dot_x = cx + cw * 0.5f - 36, dot_y = cy + 184;
            prim::fillCircle(ctx, dot_x, dot_y + 4, 4, br.solidA(st_color, t));
            prim::drawText_(ctx, st_label, mt,
                            dot_x + 10, dot_y - 2, 80, 14,
                            br.solidA(st_color, t));
            // uid 在右
            wchar_t handle[64];
            swprintf_s(handle, L"UID %.16ls", peer.uid.c_str());
            prim::drawText_(ctx, handle, mt,
                            dot_x + 60, dot_y - 2, 100, 14,
                            br.solidA(pal.text_muted, t));
        }
        // status text
        if (!peer.status_text.empty()) {
            prim::drawText_(ctx, peer.status_text, sub,
                            cx + 24, cy + 210, cw - 48, 22,
                            br.solidA(pal.text, t),
                            DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        // tags chips
        if (!peer.tags.empty()) {
            float tx = cx + 24, ty_ = cy + 240;
            auto* chip_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(8.0f));
            for (auto& tag : peer.tags) {
                DWRITE_TEXT_METRICS tm{};
                app.texts().measure(chip_fmt, tag, 8192, 256, &tm);
                float tw = tm.width + 20;
                if (tx + tw > cx + cw - 24) { tx = cx + 24; ty_ += 28; }
                if (ty_ > cy + 280) break;
                prim::fillRR(ctx, tx, ty_, tw, 22, 11, br.solidA(pal.surface, t));
                prim::drawText_(ctx, tag, chip_fmt,
                                tx + 10, ty_ + 4, tw - 20, 14,
                                br.solidA(pal.text, t));
                tx += tw + 6;
            }
        }
        // bio
        if (!peer.bio.empty()) {
            prim::drawText_(ctx, L"个人签名", mt,
                            cx + 24, cy + 296, cw - 48, 16,
                            br.solidA(pal.text_muted, t));
            prim::fillRR(ctx, cx + 24, cy + 316, cw - 48, 80, 8.0f,
                         br.solidA(pal.surface, t));
            prim::drawText_(ctx, peer.bio, sub,
                            cx + 36, cy + 326, cw - 72, 64,
                            br.solidA(pal.text, t));
        }
    }

    // 关闭 ✕
    LayoutRect close_btn{ cx + cw - 36, cy + 12, 24, 24 };
    bool ch_h = close_btn.contains(g_mouse);
    if (ch_h) {
        prim::fillRR(ctx, close_btn.x, close_btn.y, 24, 24, 6,
                     br.solidA(0x000000, t * 0.20f));
    }
    icons::drawIcon(app, icons::Name::X, close_btn.x + 4, close_btn.y + 4, 16,
                    fadeArgb(0xFFFFFFFF, t));
    hit(close_btn, [](){ closeUserProfile(); }, true);

    drawGhostBtn(app, cx + 30, cy + ch - 48, cw - 60, 36,
                 L"关闭", t, [](){ closeUserProfile(); });
}

// ============== EditStatusText modal ==============
void openEditStatusText() {
    g_edit_status.open = true;
    g_edit_status.input.text = g_user.status_text;
    g_edit_status.input.cursor = (int)g_user.status_text.size();
    g_edit_status.input.clearSel();
    g_edit_status.busy = false;
    g_edit_status.t.start(0, 1, 0.22f, 0, curve::easeOutCubic);
}
static void closeEditStatusText() {
    g_edit_status.t.start(g_edit_status.t.value(), 0, 0.18f, 0, curve::easeOutCubic);
    g_edit_status.open = false;
}
static void submitEditStatusText(HWND hwnd) {
    if (g_edit_status.busy) return;
    std::wstring s = g_edit_status.input.text;
    if (s.size() > 48) s = s.substr(0, 48);
    g_edit_status.busy = true;
    g_user.status_text = s;
    // utf-8 escape
    auto wto8 = [](const std::wstring& w) -> std::string {
        if (w.empty()) return {};
        int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (n <= 0) return {};
        std::string s(n - 1, 0);
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
        return s;
    };
    std::string fields = "\"status_text\":\"" + net::jsonEscape(s) + "\"";
    fetch::profileUpdate(hwnd, fields);
    closeEditStatusText();   // 立即关，乐观更新（失败 toast）
}
void onEditStatusTextResult(bool success) {
    g_edit_status.busy = false;
    if (!success) {
        // 后端可能不支持 status_text 字段，本地更新无所谓
    }
}
void paintEditStatusTextModal(D2DApp& app, float W, float H) {
    if (!g_edit_status.open && g_edit_status.t.value() < 0.001f) return;
    float t = g_edit_status.t.value();
    if (t < 0.001f) return;
    paintDim(app, W, H, t);

    const Palette& pal = palette();
    auto* ctx = app.ctx();
    auto& br = app.brushes();
    float cw = 360, ch = 220;
    float cx = (W - cw) * 0.5f, cy = (H - ch) * 0.5f;
    prim::drawShadow(ctx, br, cx, cy, cw, ch, 16.0f, pal.shadow_card_hover, t, 6.0f, 4);
    prim::fillRR(ctx, cx, cy, cw, ch, 16.0f, br.solidA(pal.card, t));

    auto* h1 = app.texts().format(L"Microsoft YaHei UI", ptToDip(13.0f), DWRITE_FONT_WEIGHT_BOLD);
    auto* sub = app.texts().format(L"Microsoft YaHei UI", ptToDip(9.0f));
    prim::drawText_(ctx, L"状态消息", h1, cx + 30, cy + 22, cw - 60, 22, br.solidA(pal.text, t));
    prim::drawText_(ctx, L"显示在你的状态旁，48 字以内", sub,
                    cx + 30, cy + 50, cw - 60, 18, br.solidA(pal.text_muted, t));
    drawField(app, g_edit_status.input, cx + 30, cy + 80, cw - 60, 40,
              L"在做什么…", true, t);
    hit(g_edit_status.input.bounds, [](){}, true);
    float by = cy + ch - 52;
    drawGhostBtn(app, cx + 30, by, 120, 36, L"取消", t, [](){ closeEditStatusText(); });
    drawPrimaryBtn(app, cx + cw - 30 - 160, by, 160, 36,
                   g_edit_status.busy ? L"保存中…" : L"保存", t,
                   [hwnd = GetActiveWindow()](){ submitEditStatusText(hwnd); });
}

// ============== EditBio modal ==============
void openEditBio() {
    g_edit_bio.open = true;
    g_edit_bio.input.text = g_user.bio;
    g_edit_bio.input.cursor = (int)g_user.bio.size();
    g_edit_bio.input.clearSel();
    g_edit_bio.busy = false;
    g_edit_bio.t.start(0, 1, 0.22f, 0, curve::easeOutCubic);
}
static void closeEditBio() {
    g_edit_bio.t.start(g_edit_bio.t.value(), 0, 0.18f, 0, curve::easeOutCubic);
    g_edit_bio.open = false;
}
static void submitEditBio(HWND hwnd) {
    if (g_edit_bio.busy) return;
    std::wstring s = g_edit_bio.input.text;
    if (s.size() > 240) s = s.substr(0, 240);
    g_edit_bio.busy = true;
    g_user.bio = s;
    std::string fields = "\"bio\":\"" + net::jsonEscape(s) + "\"";
    fetch::profileUpdate(hwnd, fields);
    closeEditBio();
}
void onEditBioResult(bool success) {
    g_edit_bio.busy = false;
    (void)success;
}
void paintEditBioModal(D2DApp& app, float W, float H) {
    if (!g_edit_bio.open && g_edit_bio.t.value() < 0.001f) return;
    float t = g_edit_bio.t.value();
    if (t < 0.001f) return;
    paintDim(app, W, H, t);
    const Palette& pal = palette();
    auto* ctx = app.ctx();
    auto& br = app.brushes();
    float cw = 460, ch = 320;
    float cx = (W - cw) * 0.5f, cy = (H - ch) * 0.5f;
    prim::drawShadow(ctx, br, cx, cy, cw, ch, 16.0f, pal.shadow_card_hover, t, 6.0f, 4);
    prim::fillRR(ctx, cx, cy, cw, ch, 16.0f, br.solidA(pal.card, t));
    auto* h1 = app.texts().format(L"Microsoft YaHei UI", ptToDip(13.0f), DWRITE_FONT_WEIGHT_BOLD);
    auto* sub = app.texts().format(L"Microsoft YaHei UI", ptToDip(9.0f));
    prim::drawText_(ctx, L"个人签名", h1, cx + 30, cy + 22, cw - 60, 22, br.solidA(pal.text, t));
    prim::drawText_(ctx, L"显示在你的资料卡，240 字以内", sub,
                    cx + 30, cy + 50, cw - 60, 18, br.solidA(pal.text_muted, t));
    drawField(app, g_edit_bio.input, cx + 30, cy + 80, cw - 60, 140,
              L"写点什么…", true, t);
    hit(g_edit_bio.input.bounds, [](){}, true);
    float by = cy + ch - 52;
    drawGhostBtn(app, cx + 30, by, 120, 36, L"取消", t, [](){ closeEditBio(); });
    drawPrimaryBtn(app, cx + cw - 30 - 160, by, 160, 36,
                   g_edit_bio.busy ? L"保存中…" : L"保存", t,
                   [hwnd = GetActiveWindow()](){ submitEditBio(hwnd); });
}

// ============== WebView modal (通用浏览器/视频播放) ==============
namespace {
std::wstring fileToFileUrl(const std::wstring& path) {
    // 转 file:///D:/foo/bar.mp4 — 反斜杠改正斜杠
    std::wstring url = L"file:///";
    for (wchar_t c : path) {
        if (c == L'\\') url.push_back(L'/');
        else url.push_back(c);
    }
    return url;
}
std::wstring buildVideoHtml(const std::wstring& src) {
    // 弹一个全屏 video tag wrapper — autoplay + controls + 黑底
    std::wstring h = L"<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
                     L"<style>html,body{margin:0;padding:0;background:#000;height:100vh;overflow:hidden;}"
                     L"video{width:100%;height:100%;object-fit:contain;}</style></head>"
                     L"<body><video src=\"";
    h += src;
    h += L"\" autoplay controls loop></video></body></html>";
    return h;
}
}

void openVideoPlayer(const std::wstring& src) {
    g_webview_modal.open = true;
    g_webview_modal.title = src;
    g_webview_modal.t.start(0, 1, 0.25f, 0, curve::easeOutCubic);
    if (!webview::ensure(GetActiveWindow())) return;
    // 本地路径自动转 file:/// URL
    std::wstring real_src = src;
    if (src.size() > 1 && (src[1] == L':' || src.compare(0, 2, L"\\\\") == 0)) {
        real_src = fileToFileUrl(src);
    }
    webview::navigateHtml(buildVideoHtml(real_src));
}

void openWebPage(const std::wstring& url) {
    g_webview_modal.open = true;
    g_webview_modal.title = url;
    g_webview_modal.t.start(0, 1, 0.25f, 0, curve::easeOutCubic);
    if (!webview::ensure(GetActiveWindow())) return;
    webview::navigate(url);
}

static void closeWebViewModal() {
    g_webview_modal.t.start(g_webview_modal.t.value(), 0, 0.18f, 0, curve::easeOutCubic);
    g_webview_modal.open = false;
    webview::show(false);   // 立即停止视频/页面
}

void paintWebViewModal(D2DApp& app, float W, float H) {
    if (!g_webview_modal.open && g_webview_modal.t.value() < 0.001f) return;
    float t = g_webview_modal.t.value();
    if (t < 0.001f) return;
    paintDim(app, W, H, t);

    const Palette& pal = palette();
    auto* ctx = app.ctx();
    auto& br = app.brushes();

    // 大模态：W-80 × H-80, 顶部 48 标题栏 + 内容区 = WebView2 子窗
    float cw = (std::min)(W - 80.0f, 1280.0f);
    float ch = (std::min)(H - 80.0f, 800.0f);
    float cx = (W - cw) * 0.5f, cy = (H - ch) * 0.5f;
    float bar_h = 48.0f;

    prim::drawShadow(ctx, br, cx, cy, cw, ch, 16.0f, pal.shadow_card_hover, t, 6.0f, 5);
    prim::fillRR(ctx, cx, cy, cw, ch, 16.0f, br.solidA(pal.card, t));
    prim::fillRect(ctx, cx, cy + bar_h - 1, cw, 1, br.solidA(pal.divider, t));

    // 标题栏
    auto* h1 = app.texts().format(L"Microsoft YaHei UI", ptToDip(11.0f),
                                  DWRITE_FONT_WEIGHT_BOLD);
    auto* sub = app.texts().format(L"Microsoft YaHei UI", ptToDip(8.5f));
    prim::drawText_(ctx, L"内嵌浏览器", h1,
                    cx + 16, cy + 12, 200, 22,
                    br.solidA(pal.text, t),
                    DWRITE_TEXT_ALIGNMENT_LEADING,
                    DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    prim::drawText_(ctx, g_webview_modal.title, sub,
                    cx + 100, cy + 16, cw - 220, 18,
                    br.solidA(pal.text_muted, t));

    // ✕ 关闭
    LayoutRect close_btn{ cx + cw - 36, cy + 10, 28, 28 };
    bool ch_h = close_btn.contains(g_mouse);
    if (ch_h) {
        prim::fillRR(ctx, close_btn.x, close_btn.y, 28, 28, 6,
                     br.solidA(pal.text, t * 0.10f));
    }
    icons::drawIcon(app, icons::Name::X, close_btn.x + 6, close_btn.y + 6, 16,
                    fadeArgb(pal.text, t));
    hit(close_btn, [](){ closeWebViewModal(); }, true);

    // 内容区 = WebView2 (物理像素)
    if (t > 0.95f && webview::isReady()) {
        float scale = app.dpi() / 96.0f;
        int wl = (int)((cx + 8) * scale);
        int wt = (int)((cy + bar_h) * scale);
        int wr = (int)((cx + cw - 8) * scale);
        int wb = (int)((cy + ch - 8) * scale);
        webview::setBounds(wl, wt, wr, wb);
        webview::show(true);
    } else {
        // WebView2 还没 ready / 没装 runtime → 占位
        prim::fillRR(ctx, cx + 8, cy + bar_h, cw - 16, ch - bar_h - 8, 8.0f,
                     br.solidA(pal.surface, t));
        if (!webview::runtimeAvailable()) {
            prim::drawText_(ctx, L"WebView2 Runtime 未安装 — 装个 Edge 就行",
                            sub,
                            cx, cy + ch * 0.5f, cw, 22,
                            br.solidA(pal.text_muted, t),
                            DWRITE_TEXT_ALIGNMENT_CENTER);
        } else {
            prim::drawText_(ctx, L"加载中…", sub,
                            cx, cy + ch * 0.5f, cw, 22,
                            br.solidA(pal.text_muted, t),
                            DWRITE_TEXT_ALIGNMENT_CENTER);
        }
    }
}

void paintRenamePackModal(D2DApp& app, float W, float H) {
    if (!g_renamepack.open && g_renamepack.t.value() < 0.001f) return;
    float t = g_renamepack.t.value();
    if (t < 0.001f) return;
    paintDim(app, W, H, t);

    const Palette& pal = palette();
    auto* ctx = app.ctx();
    auto& br = app.brushes();
    float cw = 360, ch = 220;
    float cx = (W - cw) * 0.5f, cy = (H - ch) * 0.5f;
    prim::drawShadow(ctx, br, cx, cy, cw, ch, 16.0f, pal.shadow_card_hover, t, 6.0f, 4);
    prim::fillRR(ctx, cx, cy, cw, ch, 16.0f, br.solidA(pal.card, t));
    auto* h1 = app.texts().format(L"Microsoft YaHei UI", ptToDip(13.0f), DWRITE_FONT_WEIGHT_BOLD);
    auto* sub = app.texts().format(L"Microsoft YaHei UI", ptToDip(9.0f));
    prim::drawText_(ctx, L"重命名表情包", h1, cx + 30, cy + 22, cw - 60, 22, br.solidA(pal.text, t));
    prim::drawText_(ctx, g_renamepack.orig_name, sub, cx + 30, cy + 50, cw - 60, 18,
                    br.solidA(pal.text_muted, t));
    drawField(app, g_renamepack.input, cx + 30, cy + 80, cw - 60, 40, L"新名字", true, t);
    hit(g_renamepack.input.bounds, [](){}, true);
    if (!g_renamepack.error_msg.empty()) {
        prim::drawText_(ctx, g_renamepack.error_msg, sub,
                        cx + 30, cy + 130, cw - 60, 18,
                        br.solidA(0xE34B4B, t));
    }
    float by = cy + ch - 52;
    drawGhostBtn(app, cx + 30, by, 120, 36, L"取消", t, [](){ closeRenamePack(); });
    drawPrimaryBtn(app, cx + cw - 30 - 160, by, 160, 36,
                   g_renamepack.busy ? L"保存中…" : L"保存", t,
                   [hwnd = GetActiveWindow()](){ submitRenamePack(hwnd); });
}

// ============== 消息右键上下文菜单 ==============
void openMsgContextMenu(POINT anchor_dip, int src_idx) {
    auto& msgs = chat::streamFor(chat::g_active);
    if (src_idx < 0 || src_idx >= (int)msgs.size()) return;
    const chat::Msg& m = msgs[src_idx];
    g_msg_menu.open = true;
    g_msg_menu.anchor = anchor_dip;
    g_msg_menu.src_idx = src_idx;
    g_msg_menu.slug = chat::g_active;
    g_msg_menu.kind_int = (int)m.kind;
    g_msg_menu.body = m.body;
    g_msg_menu.author = m.author.empty() ? m.from : m.author;
    g_msg_menu.from = m.from;
    g_msg_menu.t.start(0, 1, 0.18f, 0, curve::easeOutCubic);
}
static void closeMsgMenu() {
    g_msg_menu.t.start(g_msg_menu.t.value(), 0, 0.14f, 0, curve::easeOutCubic);
    g_msg_menu.open = false;
}

namespace {
void copyTextToClipboard(HWND hwnd, const std::wstring& s) {
    if (!OpenClipboard(hwnd)) return;
    EmptyClipboard();
    size_t bytes = (s.size() + 1) * sizeof(wchar_t);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (h) {
        memcpy(GlobalLock(h), s.c_str(), bytes);
        GlobalUnlock(h);
        SetClipboardData(CF_UNICODETEXT, h);
    }
    CloseClipboard();
}
}

void paintMsgContextMenu(D2DApp& app, float W, float H) {
    if (!g_msg_menu.open && g_msg_menu.t.value() < 0.001f) return;
    float t = g_msg_menu.t.value();
    if (t < 0.001f) return;
    const Palette& pal = palette();
    auto* ctx = app.ctx();
    auto& br = app.brushes();

    chat::MsgKind kind = (chat::MsgKind)g_msg_menu.kind_int;
    bool is_text = (kind == chat::MsgKind::Text);
    bool is_media = (kind == chat::MsgKind::Image || kind == chat::MsgKind::Gif
                  || kind == chat::MsgKind::Sticker);

    struct Item { std::wstring label; std::function<void()> click; bool danger; };
    std::vector<Item> items;
    items.push_back({ trW("msg.reply"), [](){
        // composer 文本预填 "> @author 原文..."
        std::wstring prefix = L"> @" + g_msg_menu.author + L" ";
        std::wstring body = g_msg_menu.body;
        if (body.size() > 60) body = body.substr(0, 60) + L"…";
        chat::g_composer.text = prefix + body + L"\n";
        chat::g_composer.cursor = (int)chat::g_composer.text.size();
        chat::g_composer.clearSel();
        chat::g_focus_composer = true;
        closeMsgMenu();
    }, false });
    items.push_back({ trW("msg.copy"), [](){
        copyTextToClipboard(GetActiveWindow(),
            g_msg_menu.body.empty() ? L"" : g_msg_menu.body);
        toast::show(trW("toast.copied"));
        closeMsgMenu();
    }, false });
    if (is_media) {
        items.push_back({ trW("msg.add_emoji"), [](){
            // 把当前媒体路径当作文件，复制到「我的表情」分组（直接 importFromFolder 但只导入一个文件）
            // 简化：直接发到主线程让它走文件夹导入流（改为单文件添加）。
            std::wstring path = g_msg_menu.body;
            // 把文件拷到一个临时目录然后 importFromFolder
            wchar_t tmp[MAX_PATH] = {0};
            GetTempPathW(MAX_PATH, tmp);
            std::wstring tmpdir = std::wstring(tmp) + L"launcher_add_emoji\\";
            CreateDirectoryW(tmpdir.c_str(), nullptr);
            // 清空目录里的旧文件
            std::wstring pat = tmpdir + L"*";
            WIN32_FIND_DATAW fd{};
            HANDLE h = FindFirstFileW(pat.c_str(), &fd);
            if (h != INVALID_HANDLE_VALUE) {
                do {
                    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                    DeleteFileW((tmpdir + fd.cFileName).c_str());
                } while (FindNextFileW(h, &fd));
                FindClose(h);
            }
            // 拷新文件
            auto sl = path.find_last_of(L"\\/");
            std::wstring fname = (sl != std::wstring::npos) ? path.substr(sl + 1) : path;
            CopyFileW(path.c_str(), (tmpdir + fname).c_str(), FALSE);
            // 找「我的表情」pack id
            std::string my_pid;
            {
                std::lock_guard<std::mutex> lk(sticker::g_packs_mtx);
                for (auto& p : sticker::g_packs) {
                    if (!p.is_system && p.is_owner && p.name == L"我的表情") {
                        my_pid = p.id; break;
                    }
                }
            }
            if (my_pid.empty()) {
                sticker::ensureMyStickersPack(GetActiveWindow());
            } else {
                sticker::importFromFolder(GetActiveWindow(), tmpdir, my_pid);
                toast::show(trW("toast.added_to_emoji"));
            }
            closeMsgMenu();
        }, false });
    }
    if (g_msg_menu.from == L"me") {
        items.push_back({ trW("msg.delete"), [](){
            auto& msgs = chat::streamFor(g_msg_menu.slug);
            int idx = g_msg_menu.src_idx;
            int64_t server_id = 0;
            if (idx >= 0 && idx < (int)msgs.size()) {
                server_id = msgs[idx].server_id;
            }
            if (server_id > 0) {
                chat::deleteMessage(GetActiveWindow(), g_msg_menu.slug, server_id);
                toast::show(trW("toast.deleted_all"));
            } else {
                // 没 server_id（刚发还没回 message_id）— 暂时只删本地
                if (idx >= 0 && idx < (int)msgs.size()) {
                    msgs.erase(msgs.begin() + idx);
                }
                toast::show(trW("toast.deleted_local"));
            }
            closeMsgMenu();
        }, true });
    }
    (void)is_text;

    // 菜单尺寸
    float mw = 180;
    float row_h = 32;
    float mh = (float)items.size() * row_h + 12;
    float mx = (float)g_msg_menu.anchor.x;
    float my = (float)g_msg_menu.anchor.y;
    if (mx + mw > W - 8) mx = W - 8 - mw;
    if (my + mh > H - 8) my = H - 8 - mh;
    if (mx < 8) mx = 8;
    if (my < 8) my = 8;

    // 关键：注册全屏 catchall hit — 所有点击在反向 dispatch 时被 menu items 优先吃掉，
    // 落到 catchall 就关闭菜单（避免还跑到 sidebar / chat list / 等下层 hit）
    hit({ 0, 0, W, H }, [](){ closeMsgMenu(); }, false);

    prim::drawShadow(ctx, br, mx, my, mw, mh, 10.0f, pal.shadow_card_hover, t, 4.0f, 3);
    prim::fillRR(ctx, mx, my, mw, mh, 10.0f, br.solidA(pal.card, t));
    prim::strokeRR(ctx, mx, my, mw, mh, 10.0f, br.solidA(pal.divider, t));
    auto* row_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(9.5f));
    float ry = my + 6;
    for (auto& it : items) {
        LayoutRect r{ mx + 6, ry, mw - 12, row_h - 4 };
        bool hov = r.contains(g_mouse);
        if (hov) {
            prim::fillRR(ctx, r.x, r.y, r.w, r.h, 6.0f,
                         br.solidA(it.danger ? 0xE34B4B : pal.text,
                                    t * (it.danger ? 0.10f : 0.06f)));
        }
        prim::drawText_(ctx, it.label, row_fmt,
                        r.x + 12, r.y + 6, r.w - 16, 18,
                        br.solidA(it.danger ? 0xE34B4B : pal.text, t));
        auto cb = it.click;
        hit(r, cb, true);
        ry += row_h;
    }
}

// ============== PackPreview modal ==============
void openPackPreviewModal(const std::string& short_name) {
    g_pack_preview_modal.open = true;
    g_pack_preview_modal.short_name = short_name;
    g_pack_preview_modal.t.start(0, 1, 0.25f, 0, curve::easeOutCubic);
    sticker::previewPackByShort(GetActiveWindow(), short_name);
}
static void closePackPreview() {
    g_pack_preview_modal.t.start(g_pack_preview_modal.t.value(),
                                 0, 0.18f, 0, curve::easeOutCubic);
    g_pack_preview_modal.open = false;
}

void paintPackPreviewModal(D2DApp& app, float W, float H) {
    if (!g_pack_preview_modal.open && g_pack_preview_modal.t.value() < 0.001f) return;
    float t = g_pack_preview_modal.t.value();
    if (t < 0.001f) return;
    paintDim(app, W, H, t);

    const Palette& pal = palette();
    auto* ctx = app.ctx();
    auto& br = app.brushes();
    float cw = 460, ch = 480;
    float cx = (W - cw) * 0.5f, cy = (H - ch) * 0.5f;
    prim::drawShadow(ctx, br, cx, cy, cw, ch, 16.0f, pal.shadow_card_hover, t, 6.0f, 4);
    prim::fillRR(ctx, cx, cy, cw, ch, 16.0f, br.solidA(pal.card, t));

    auto* h1 = app.texts().format(L"Microsoft YaHei UI", ptToDip(15.0f), DWRITE_FONT_WEIGHT_BOLD);
    auto* sub = app.texts().format(L"Microsoft YaHei UI", ptToDip(9.5f));
    auto* mt = app.texts().format(L"Microsoft YaHei UI", ptToDip(8.5f));

    sticker::PackPreview pv;
    {
        std::lock_guard<std::mutex> lk(sticker::g_pack_preview_mtx);
        pv = sticker::g_pack_preview;
    }
    if (!pv.loaded) {
        prim::drawText_(ctx, L"加载中…", sub,
                        cx, cy + ch * 0.5f - 12, cw, 24,
                        br.solidA(pal.text_muted, t),
                        DWRITE_TEXT_ALIGNMENT_CENTER);
    } else if (!pv.err.empty()) {
        prim::drawText_(ctx, L"无法加载", h1,
                        cx, cy + 30, cw, 24,
                        br.solidA(pal.text, t),
                        DWRITE_TEXT_ALIGNMENT_CENTER);
        std::wstring werr;
        for (char c : pv.err) werr.push_back((wchar_t)c);
        prim::drawText_(ctx, werr, sub,
                        cx + 30, cy + 60, cw - 60, 80,
                        br.solidA(pal.text_muted, t),
                        DWRITE_TEXT_ALIGNMENT_CENTER);
    } else {
        // 标题 + 创建人
        prim::drawText_(ctx, pv.name.empty() ? L"分享的表情包" : pv.name, h1,
                        cx + 30, cy + 22, cw - 60, 30,
                        br.solidA(pal.text, t));
        std::wstring meta;
        if (!pv.creator_name.empty()) meta = L"by " + pv.creator_name;
        else meta = L"分享的表情包";
        wchar_t buf[64];
        swprintf_s(buf, L"%ls · 已被 %d 人安装", meta.c_str(), pv.install_count);
        prim::drawText_(ctx, buf, mt,
                        cx + 30, cy + 54, cw - 60, 18,
                        br.solidA(pal.text_muted, t));

        // 缩略图栅格
        if (!pv.sticker_paths.empty()) {
            int cols = 5;
            float cell = 72.0f;
            float gx = cx + 30, gy = cy + 88;
            int max_show = 15;
            int n = (std::min)((int)pv.sticker_paths.size(), max_show);
            for (int i = 0; i < n; ++i) {
                int row = i / cols, col = i % cols;
                float ex = gx + col * (cell + 6), ey = gy + row * (cell + 6);
                prim::fillRR(ctx, ex, ey, cell, cell, 8.0f, br.solidA(pal.surface, t));
                ID2D1Bitmap* bmp = app.images().fromFile(pv.sticker_paths[i]);
                if (bmp) {
                    prim::pushLayerRR(ctx, app.factory(), ex, ey, cell, cell, 8.0f);
                    ctx->DrawBitmap(bmp,
                        D2D1::RectF(ex, ey, ex + cell, ey + cell),
                        t, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                    prim::popLayer(ctx);
                }
            }
            if ((int)pv.sticker_paths.size() > max_show) {
                wchar_t mw[32];
                swprintf_s(mw, L"+ %d 张更多", (int)pv.sticker_paths.size() - max_show);
                prim::drawText_(ctx, mw, mt,
                                cx + 30, cy + 88 + 3 * (cell + 6) + 4, cw - 60, 18,
                                br.solidA(pal.text_muted, t),
                                DWRITE_TEXT_ALIGNMENT_CENTER);
            }
        }

        float by = cy + ch - 56;
        drawGhostBtn(app, cx + 30, by, 130, 38, L"取消", t,
                     [](){ closePackPreview(); });
        if (pv.already_installed) {
            drawGhostBtn(app, cx + cw - 30 - 200, by, 200, 38,
                         L"已添加 ✓", t, [](){
                            closePackPreview();
                            toast::show(L"已经在你的表情包列表里");
                         });
        } else {
            std::string sn = pv.short_name;
            drawPrimaryBtn(app, cx + cw - 30 - 200, by, 200, 38,
                           L"添加分组", t, [sn](){
                                sticker::installPackByShort(GetActiveWindow(), sn);
                                closePackPreview();
                           });
        }
    }
    // ✕
    LayoutRect close_btn{ cx + cw - 36, cy + 12, 24, 24 };
    bool ch_h = close_btn.contains(g_mouse);
    if (ch_h) {
        prim::fillRR(ctx, close_btn.x, close_btn.y, 24, 24, 6,
                     br.solidA(pal.text, t * 0.10f));
    }
    icons::drawIcon(app, icons::Name::X, close_btn.x + 4, close_btn.y + 4, 16,
                    fadeArgb(pal.text, t));
    hit(close_btn, [](){ closePackPreview(); }, true);
}

// ============== 事件路由 ==============
bool onMouseLDown(HWND /*hwnd*/, POINT dip) {
    if (!anyOpen()) return false;
    // 上下文菜单：点外面就关；点里面 dispatchClick 即可
    if (g_msg_menu.open) {
        bool consumed = dispatchClick(dip);
        if (!consumed) closeMsgMenu();
        return true;
    }
    if (g_webview_modal.open) {
        dispatchClick(dip);
        return true;
    }
    bool consumed = dispatchClick(dip);
    if (!consumed) {
        if (g_pack_preview_modal.open) closePackPreview();
        else if (g_edit_bio.open) closeEditBio();
        else if (g_edit_status.open) closeEditStatusText();
        else if (g_user_profile.open) closeUserProfile();
        else if (g_renamepack.open) closeRenamePack();
        else if (g_createpack.open) closeCreatePack();
        else if (g_addtag.open) closeAddTag();
        else if (g_change_pw.open) closeChangePw();
        else if (g_confirm.open) closeConfirm();
        else if (g_cs2.open) closeCS2();
        else if (g_history.open) closeHistory();
    }
    return true;
}

bool onChar(HWND hwnd, wchar_t c, bool ctrl) {
    if (g_edit_bio.open) { g_edit_bio.input.onChar(c, ctrl, hwnd); return true; }
    if (g_edit_status.open) { g_edit_status.input.onChar(c, ctrl, hwnd); return true; }
    if (g_addtag.open) { g_addtag.input.onChar(c, ctrl, hwnd); return true; }
    if (g_createpack.open) { g_createpack.input.onChar(c, ctrl, hwnd); return true; }
    if (g_renamepack.open) { g_renamepack.input.onChar(c, ctrl, hwnd); return true; }
    if (!g_change_pw.open) return false;
    std::array<InputBox*, 3> boxes{
        &g_change_pw.old_pw, &g_change_pw.new_pw, &g_change_pw.repeat_pw };
    if (g_change_pw.focus < 0 || g_change_pw.focus >= 3) return false;
    boxes[g_change_pw.focus]->onChar(c, ctrl, hwnd);
    return true;
}

bool onKey(HWND hwnd, int vk, bool shift, bool ctrl) {
    if (!anyOpen()) return false;
    if (vk == VK_ESCAPE) {
        if (g_msg_menu.open) { closeMsgMenu(); return true; }
        if (g_pack_preview_modal.open) { closePackPreview(); return true; }
        if (g_webview_modal.open) { closeWebViewModal(); return true; }
        if (g_edit_bio.open) { closeEditBio(); return true; }
        if (g_edit_status.open) { closeEditStatusText(); return true; }
        if (g_user_profile.open) { closeUserProfile(); return true; }
        if (g_renamepack.open) { closeRenamePack(); return true; }
        if (g_createpack.open) { closeCreatePack(); return true; }
        if (g_addtag.open) { closeAddTag(); return true; }
        if (g_change_pw.open) { closeChangePw(); return true; }
        if (g_confirm.open) { closeConfirm(); return true; }
        if (g_cs2.open) { closeCS2(); return true; }
        if (g_history.open) { closeHistory(); return true; }
    }
    if (g_edit_status.open) {
        if (vk == VK_RETURN) { submitEditStatusText(hwnd); return true; }
        g_edit_status.input.onKey(vk, shift, ctrl);
        return true;
    }
    if (g_edit_bio.open) {
        if (vk == VK_RETURN && !ctrl) { submitEditBio(hwnd); return true; }
        g_edit_bio.input.onKey(vk, shift, ctrl);
        return true;
    }
    if (g_addtag.open) {
        if (vk == VK_RETURN) { submitAddTag(hwnd); return true; }
        g_addtag.input.onKey(vk, shift, ctrl);
        return true;
    }
    if (g_createpack.open) {
        if (vk == VK_RETURN) { submitCreatePack(hwnd); return true; }
        g_createpack.input.onKey(vk, shift, ctrl);
        return true;
    }
    if (g_renamepack.open) {
        if (vk == VK_RETURN) { submitRenamePack(hwnd); return true; }
        g_renamepack.input.onKey(vk, shift, ctrl);
        return true;
    }
    if (g_change_pw.open) {
        if (vk == VK_RETURN) { submitChangePw(hwnd); return true; }
        if (vk == VK_TAB) {
            int next = g_change_pw.focus + (shift ? -1 : 1);
            if (next < 0) next = 2;
            if (next > 2) next = 0;
            g_change_pw.focus = next;
            return true;
        }
        std::array<InputBox*, 3> boxes{
            &g_change_pw.old_pw, &g_change_pw.new_pw, &g_change_pw.repeat_pw };
        if (g_change_pw.focus >= 0 && g_change_pw.focus < 3) {
            boxes[g_change_pw.focus]->onKey(vk, shift, ctrl);
        }
        return true;
    }
    return false;
}

}  // namespace launcher::d2d::modal
