// Modals implementation. See modals.h; simplified D2D modal surface.

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
#include "persist.h"
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
UserContextMenuState g_user_menu;
MuteUserState     g_mute_user;
PackPreviewState  g_pack_preview_modal;
SearchState       g_search;

namespace {

float measureW(D2DApp& app, std::wstring_view s, IDWriteTextFormat* fmt) {
    if (s.empty() || !fmt) return 0.0f;
    DWRITE_TEXT_METRICS m{};
    if (!app.texts().measure(fmt, s, 8192.0f, 256.0f, &m)) return 0.0f;
    return m.width;
}

std::wstring utf8ToWModal(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(n - 1, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

bool isSelfProfileKey(const std::wstring& key) {
    if (key.empty()) return false;
    return key == L"me"
        || key == utf8ToWModal(g_user_id)
        || key == g_user.uid
        || key == g_user.username
        || key == g_user.nickname;
}

fetch::PeerProfile selfPeerProfileSnapshot() {
    fetch::PeerProfile p;
    p.cache_key = utf8ToWModal(g_user_id);
    p.uid = g_user.uid;
    p.username = g_user.username;
    p.nickname = g_user.nickname;
    p.status = statusKey(g_status);
    p.status_text = g_user.status_text;
    p.bio = g_user.bio;
    p.role = g_user.role;
    p.role_label = g_user.role_label;
    p.avatar_path = g_avatar_path;
    p.tags = g_user_tags;
    p.loaded = true;
    return p;
}

std::wstring fitText(D2DApp& app, const std::wstring& s,
                     IDWriteTextFormat* fmt, float max_w) {
    if (s.empty() || max_w <= 4.0f || measureW(app, s, fmt) <= max_w) return s;
    const std::wstring ell = L"...";
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
    ComPtr<ID2D1BitmapBrush> bb;
    if (FAILED(ctx->CreateBitmapBrush(bmp, bp, &bb))) return false;
    auto mt = D2D1::Matrix3x2F::Scale({scale, scale}, {0, 0})
            * D2D1::Matrix3x2F::Translation(dx, dy);
    bb->SetTransform(mt);
    bb->SetOpacity(opacity);
    ctx->FillEllipse(D2D1::Ellipse({x + r, y + r}, r, r), bb.Get());
    return true;
}

inline uint32_t fadeArgb(uint32_t argb, float op) {
    uint32_t a = (argb >> 24) & 0xFFu;
    a = (uint32_t)(a * op + 0.5f);
    if (a > 255) a = 255;
    return (a << 24) | (argb & 0xFFFFFFu);
}

// Shared dim background.
void paintDim(D2DApp& app, float W, float H, float t) {
    const Palette& pal = palette();
    app.ctx()->FillRectangle(D2D1::RectF(0, 0, W, H),
                             app.brushes().solidA(0x000000, 0.40f * t));
    (void)pal;
}

// Primary button with pressed/hover states.
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

// Ghost button with outline and transparent background.
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

// Shared InputBox rendering for modal fields.
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
            // Keep caret height fixed for compact and textarea-style fields.
            // This prevents a tall textarea from drawing an oversized caret.
            float cx = x + 12 + pre_w;
            float cy_top = y + 10;
            float cy_bot = cy_top + 18.0f;
            if (cy_bot > y + h - 4) cy_bot = y + h - 4;
            prim::drawLine(ctx, cx, cy_top, cx, cy_bot,
                           br.solidA(pal.primary, op), 1.5f);
        }
    }
}

// === ChangePw backend ===
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
    g_user_menu.t.tick(dt);
    g_mute_user.t.tick(dt);
    g_mute_user.duration.float_t.tick(dt);
    g_mute_user.reason.float_t.tick(dt);
    g_pack_preview_modal.t.tick(dt);
    g_search.t.tick(dt);
    g_search.input.float_t.tick(dt);
}

bool anyOpen() {
    return g_change_pw.open || g_confirm.open || g_cs2.open || g_history.open
        || g_addtag.open || g_createpack.open || g_renamepack.open
        || g_user_profile.open || g_edit_status.open || g_edit_bio.open
        || g_webview_modal.open || g_msg_menu.open || g_user_menu.open || g_mute_user.open
        || g_pack_preview_modal.open
        || g_search.open;
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
        g_change_pw.error_msg = L"New passwords do not match"; return;
    }
    if (g_change_pw.new_pw.text.size() < 6) {
        g_change_pw.error_msg = L"New password must be at least 6 chars"; return;
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
                if (m.empty()) m = "Password change failed";
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
        // Password changed successfully; force a fresh sign-in.
        persist::clearSession();
        g_session_token.clear();
        g_user_id.clear();
        closeChangePw();
        stages::g_stage = stages::Stage::Auth;
        stages::g_auth_card_op.start(0, 1, 0.40f, 0.05f, curve::easeOutCubic);
        stages::g_auth_card_y.start(12, 0, 0.45f, 0.05f, curve::easeOutQuint);
    } else {
        g_change_pw.error_msg = g_pw_pending_error.empty()
            ? L"Password change failed" : g_pw_pending_error;
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
    prim::drawText_(ctx, L"Change password", h1,
                    cx + 30, cy + 28, cw - 60, 24,
                    br.solidA(pal.text, t));
    prim::drawText_(ctx, L"Sign in again after password change", sub,
                    cx + 30, cy + 56, cw - 60, 18,
                    br.solidA(pal.text_muted, t));

    float fy = cy + 90;
    drawField(app, g_change_pw.old_pw, cx + 30, fy, cw - 60, 40,
              L"当前密码", g_change_pw.focus == 0, t);
    fy += 50;
    drawField(app, g_change_pw.new_pw, cx + 30, fy, cw - 60, 40,
              L"New password (at least 6 chars)", g_change_pw.focus == 1, t);
    fy += 50;
    drawField(app, g_change_pw.repeat_pw, cx + 30, fy, cw - 60, 40,
              L"Repeat new password", g_change_pw.focus == 2, t);
    fy += 56;

    hit(g_change_pw.old_pw.bounds, [](){ g_change_pw.focus = 0;
        g_change_pw.new_pw.clearSel(); g_change_pw.repeat_pw.clearSel(); }, true);
    hit(g_change_pw.new_pw.bounds, [](){ g_change_pw.focus = 1;
        g_change_pw.old_pw.clearSel(); g_change_pw.repeat_pw.clearSel(); }, true);
    hit(g_change_pw.repeat_pw.bounds, [](){ g_change_pw.focus = 2;
        g_change_pw.old_pw.clearSel(); g_change_pw.new_pw.clearSel(); }, true);

    // Error message.
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
                   g_change_pw.busy ? L"Submitting..." : L"Submit", t,
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
    // Start embedded WebView2 Steam store widget.
    if (webview::ensure(GetActiveWindow())) {
        webview::navigate(L"https://store.steampowered.com/widget/730/embed/");
    }
}
static void closeCS2() {
    g_cs2.t.start(g_cs2.t.value(), 0, 0.20f, 0, curve::easeOutQuint);
    g_cs2.open = false;
    webview::show(false);   // Hide immediately so video does not cover the app.
}

void paintCS2Modal(D2DApp& app, float W, float H) {
    if (!g_cs2.open && g_cs2.t.value() < 0.001f) return;
    float t = g_cs2.t.value();
    if (t < 0.001f) return;
    paintDim(app, W, H, t);

    const Palette& pal = palette();
    auto* ctx = app.ctx();
    auto& br = app.brushes();

    float cw = 720, ch = 560;       // Large enough for the store widget.
    float cx = (W - cw) * 0.5f, cy = (H - ch) * 0.5f;
    prim::drawShadow(ctx, br, cx, cy, cw, ch, 16.0f, pal.shadow_card_hover, t, 6.0f, 5);
    prim::fillRR(ctx, cx, cy, cw, ch, 16.0f, br.solidA(pal.card, t));

    // Title bar stays outside the WebView bounds.
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

    // Close button in the title bar.
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

    // Cover area below the title bar.
    float cover_y = cy + bar_h;
    float cover_h = 280;            // Larger video area.
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
        prim::drawText_(ctx, webview::runtimeAvailable() ? L"Runtime ready" :
                              L"WebView2 Runtime missing. Install Edge WebView2.",
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
        { L"Account", g_steam.persona.empty() ? std::wstring(L"Steam not linked") : g_steam.persona },
        { L"Last played", g_steam.last_played.empty() ? std::wstring(L"--") : g_steam.last_played },
        { L"Playtime", g_steam.playtime_label.empty() ? std::wstring(L"--") : g_steam.playtime_label },
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
                   L"Launch CS2", t, [](){ launchCS2(); });
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
        prim::drawText_(ctx, L"Loading...", sub,
                        cx + 24, cy + 50, cw - 48, 18,
                        br.solidA(pal.text_muted, t));
    } else {
        wchar_t info[64];
        swprintf_s(info, L"%d records, 5 per page", (int)g_history.rows.size());
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
        // Draw five placeholder rows.
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
    // Pagination controls.
    if (total_pages > 1) {
        wchar_t pg[16]; swprintf_s(pg, L"%d / %d", g_history.page + 1, total_pages);
        prim::drawText_(ctx, pg, sub,
                        cx + cw * 0.5f - 30, cy + ch - 90, 60, 18,
                        br.solidA(pal.text_muted, t),
                        DWRITE_TEXT_ALIGNMENT_CENTER);
        drawGhostBtn(app, cx + cw * 0.5f - 90, cy + ch - 95, 30, 30, L"<", t,
                     [](){ if (g_history.page > 0) g_history.page--; });
        drawGhostBtn(app, cx + cw * 0.5f + 60, cy + ch - 95, 30, 30, L">", t,
                     [](){ g_history.page++; });
    }

    drawGhostBtn(app, cx + cw - 24 - 100, cy + ch - 52, 100, 36,
                 L"关闭", t, [](){ closeHistory(); });

    // History rows replace the placeholders after loading.
    if (g_history.loaded && !g_history.rows.empty()) {
        // Simple five-row pagination.
        // 已经画了占位 5 行，这里覆盖：用 rows 显示
    }
}

void onHistoryResult(const std::string& body) {
    g_history.rows.clear();
    g_history.page = 0;
    g_history.loaded = true;
    // Parse login history rows.
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
        auto cb = net::findJsonObjectEnd(body, ob);
        if (cb == std::string::npos) break;
        std::string obj = body.substr(ob, cb - ob + 1);
        std::string ts = net::jsonStr(obj, "ts");
        std::string ip = net::jsonStr(obj, "ip");
        std::string geo = net::jsonStr(obj, "geo");
        // Keep the timestamp compact.
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
    if (tag.empty()) { g_addtag.error_msg = L"Tag cannot be empty"; return; }
    if (tag.size() > 24) { g_addtag.error_msg = L"Tag must be 24 chars or fewer"; return; }
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
        if (status == 409) g_addtag.error_msg = L"Tag already exists";
        else if (status == 429) g_addtag.error_msg = L"Tag limit reached (20)";
        else g_addtag.error_msg = L"Failed to add tag";
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
    prim::drawText_(ctx, L"Add tag", h1,
                    cx + 30, cy + 22, cw - 60, 22,
                    br.solidA(pal.text, t));
    prim::drawText_(ctx, L"Up to 24 chars; max 20 tags", sub,
                    cx + 30, cy + 50, cw - 60, 18,
                    br.solidA(pal.text_muted, t));

    drawField(app, g_addtag.input, cx + 30, cy + 80, cw - 60, 40,
              L"Tag", true, t);
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
                   g_addtag.busy ? L"Adding..." : L"Add", t,
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
    if (name.size() > 24) { g_createpack.error_msg = L"Name must be 24 chars or fewer"; return; }
    if (g_session_token.empty()) { g_createpack.error_msg = L"Please sign in first"; return; }
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
    prim::drawText_(ctx, L"Create pack", h1, cx + 30, cy + 22, cw - 60, 22, br.solidA(pal.text, t));
    prim::drawText_(ctx, L"Pack name, up to 24 chars", sub, cx + 30, cy + 50, cw - 60, 18,
                    br.solidA(pal.text_muted, t));
    drawField(app, g_createpack.input, cx + 30, cy + 80, cw - 60, 40, L"Pack name", true, t);
    hit(g_createpack.input.bounds, [](){}, true);
    if (!g_createpack.error_msg.empty()) {
        prim::drawText_(ctx, g_createpack.error_msg, sub,
                        cx + 30, cy + 130, cw - 60, 18,
                        br.solidA(0xE34B4B, t));
    }
    float by = cy + ch - 52;
    drawGhostBtn(app, cx + 30, by, 120, 36, L"取消", t, [](){ closeCreatePack(); });
    drawPrimaryBtn(app, cx + cw - 30 - 160, by, 160, 36,
                   g_createpack.busy ? L"Creating..." : L"Create", t,
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
    if (name.empty()) { g_renamepack.error_msg = L"Pack name cannot be empty"; return; }
    if (name.size() > 24) { g_renamepack.error_msg = L"Name must be 24 chars or fewer"; return; }
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
    else g_renamepack.error_msg = L"Rename failed";
}
// ============== UserProfile modal ==============
void openUserProfile(const std::wstring& uid_or_nickname) {
    g_user_profile.open = true;
    g_user_profile.t.start(0, 1, 0.25f, 0, curve::easeOutCubic);
    if (isSelfProfileKey(uid_or_nickname)) {
        std::lock_guard<std::mutex> lk(fetch::g_peer_mtx);
        fetch::g_peer = selfPeerProfileSnapshot();
    } else {
        fetch::peerProfile(GetActiveWindow(), uid_or_nickname);
    }
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
        peer = isSelfProfileKey(fetch::g_peer.cache_key)
            ? selfPeerProfileSnapshot()
            : fetch::g_peer;
    }
    float ar = 36.0f;
    float ax = cx + (cw - ar * 2) * 0.5f, ay = cy + 60;
    prim::fillCircle(ctx, ax + ar, ay + ar, ar + 4, br.solidA(pal.card, t));
    bool drew_peer_avatar = false;
    if (!peer.avatar_path.empty()) {
        if (auto* bmp = app.images().fromFile(peer.avatar_path)) {
            drew_peer_avatar = drawCoverCircle(app, bmp, ax, ay, ar, t);
        }
    }
    if (!drew_peer_avatar) {
        prim::fillCircle(ctx, ax + ar, ay + ar, ar, br.solidA(pal.primary_hover, t));
    }
    if (!drew_peer_avatar) {
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
    }

    auto* h1 = app.texts().format(L"Microsoft YaHei UI", ptToDip(15.0f),
                                  DWRITE_FONT_WEIGHT_BOLD);
    auto* sub = app.texts().format(L"Microsoft YaHei UI", ptToDip(9.5f));
    auto* mt = app.texts().format(L"Microsoft YaHei UI", ptToDip(8.0f));

    if (!peer.loaded) {
        prim::drawText_(ctx, L"Loading...", sub,
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
        std::wstring title = peer.nickname.empty() ? peer.uid : peer.nickname;
        title = fitText(app, title, h1, cw - 48.0f);
        prim::drawText_(ctx, title, h1,
                        cx, cy + 150, cw, 26,
                        br.solidA(pal.text, t),
                        DWRITE_TEXT_ALIGNMENT_CENTER);
        // Online status dot and label.
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
            std::wstring uid_disp = fitText(app, peer.uid, mt, 100.0f);
            swprintf_s(handle, L"UID %ls", uid_disp.c_str());
            prim::drawText_(ctx, handle, mt,
                            dot_x + 60, dot_y - 2, 100, 14,
                            br.solidA(pal.text_muted, t));
        }
        if (!peer.role_label.empty() || !peer.role.empty()) {
            std::wstring role = peer.role_label.empty() ? peer.role : peer.role_label;
            role = fitText(app, role, mt, 160.0f);
            prim::fillRR(ctx, cx + (cw - 170.0f) * 0.5f, cy + 198, 170.0f, 20.0f, 10.0f,
                         br.solidA(pal.primary, 0.14f * t));
            prim::drawText_(ctx, role, mt,
                            cx + (cw - 170.0f) * 0.5f, cy + 202, 170.0f, 12.0f,
                            br.solidA(pal.primary, t),
                            DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        // status text
        if (!peer.status_text.empty()) {
            std::wstring status_text = fitText(app, peer.status_text, sub, cw - 48.0f);
            prim::drawText_(ctx, status_text, sub,
                            cx + 24, cy + 220, cw - 48, 22,
                            br.solidA(pal.text, t),
                            DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        // tags chips
        if (!peer.tags.empty()) {
            float tx = cx + 24, ty_ = cy + 240;
            auto* chip_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(8.0f));
            for (auto& tag : peer.tags) {
                float max_chip_w = cw - 48.0f;
                std::wstring tag_disp = fitText(app, tag, chip_fmt, max_chip_w - 20.0f);
                float tw = (std::min)(measureW(app, tag_disp, chip_fmt) + 20.0f, max_chip_w);
                if (tx + tw > cx + cw - 24) { tx = cx + 24; ty_ += 28; }
                if (ty_ > cy + 280) break;
                prim::fillRR(ctx, tx, ty_, tw, 22, 11, br.solidA(pal.surface, t));
                prim::drawText_(ctx, tag_disp, chip_fmt,
                                tx + 10, ty_ + 4, tw - 20, 14,
                                br.solidA(pal.text, t));
                tx += tw + 6;
            }
        }
        // bio
        if (!peer.bio.empty()) {
            prim::drawText_(ctx, L"Bio", mt,
                            cx + 24, cy + 296, cw - 48, 16,
                            br.solidA(pal.text_muted, t));
            prim::fillRR(ctx, cx + 24, cy + 316, cw - 48, 80, 8.0f,
                         br.solidA(pal.surface, t));
            prim::drawText_(ctx, peer.bio, sub,
                            cx + 36, cy + 326, cw - 72, 64,
                            br.solidA(pal.text, t));
        }
    }

    // Close button.
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
    fetch::profileUpdate(hwnd, fetch::ProfileUpdateKind::StatusText, fields);
    closeEditStatusText();
}
void onEditStatusTextResult(bool success) {
    if (!g_edit_status.busy) return;
    g_edit_status.busy = false;
    if (!success) {
        fetch::myProfile(GetActiveWindow());
        toast::show(L"保存失败，已重新拉取资料");
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
    prim::drawText_(ctx, L"Edit status", h1, cx + 30, cy + 22, cw - 60, 22, br.solidA(pal.text, t));
    prim::drawText_(ctx, L"Short status, up to 80 chars", sub,
                    cx + 30, cy + 50, cw - 60, 18, br.solidA(pal.text_muted, t));
    drawField(app, g_edit_status.input, cx + 30, cy + 80, cw - 60, 40,
              L"What are you up to?", true, t);
    hit(g_edit_status.input.bounds, [](){}, true);
    float by = cy + ch - 52;
    std::wstring cancel = trW("common.cancel");
    std::wstring save = g_edit_status.busy ? std::wstring(L"Saving...") : trW("common.save");
    drawGhostBtn(app, cx + 30, by, 120, 36, cancel.c_str(), t, [](){ closeEditStatusText(); });
    drawPrimaryBtn(app, cx + cw - 30 - 160, by, 160, 36,
                   save.c_str(), t,
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
    fetch::profileUpdate(hwnd, fetch::ProfileUpdateKind::Bio, fields);
    closeEditBio();
}
void onEditBioResult(bool success) {
    if (!g_edit_bio.busy) return;
    g_edit_bio.busy = false;
    if (!success) {
        fetch::myProfile(GetActiveWindow());
        toast::show(L"保存失败，已重新拉取资料");
    }
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
    prim::drawText_(ctx, trW("profile.bio"), h1, cx + 30, cy + 22, cw - 60, 22, br.solidA(pal.text, t));
    prim::drawText_(ctx, L"Short bio, up to 240 chars", sub,
                    cx + 30, cy + 50, cw - 60, 18, br.solidA(pal.text_muted, t));
    drawField(app, g_edit_bio.input, cx + 30, cy + 80, cw - 60, 140,
              L"Tell people about yourself", true, t);
    hit(g_edit_bio.input.bounds, [](){}, true);
    float by = cy + ch - 52;
    std::wstring cancel = trW("common.cancel");
    std::wstring save = g_edit_bio.busy ? std::wstring(L"Saving...") : trW("common.save");
    drawGhostBtn(app, cx + 30, by, 120, 36, cancel.c_str(), t, [](){ closeEditBio(); });
    drawPrimaryBtn(app, cx + cw - 30 - 160, by, 160, 36,
                   save.c_str(), t,
                   [hwnd = GetActiveWindow()](){ submitEditBio(hwnd); });
}

// ============== WebView modal ==============
namespace {
std::wstring fileToFileUrl(const std::wstring& path) {
    // Convert local paths to file:/// URLs.
    std::wstring url = L"file:///";
    for (wchar_t c : path) {
        if (c == L'\\') url.push_back(L'/');
        else url.push_back(c);
    }
    return url;
}
std::wstring buildVideoHtml(const std::wstring& src) {
    // Full-window video wrapper with autoplay and controls.
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
    // Local paths are converted to file:/// URLs.
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
    webview::show(false);   // Stop the embedded page/video immediately.
}

void paintWebViewModal(D2DApp& app, float W, float H) {
    if (!g_webview_modal.open && g_webview_modal.t.value() < 0.001f) return;
    float t = g_webview_modal.t.value();
    if (t < 0.001f) return;
    paintDim(app, W, H, t);

    const Palette& pal = palette();
    auto* ctx = app.ctx();
    auto& br = app.brushes();

    // Large modal with a title bar and WebView content area.
    float cw = (std::min)(W - 80.0f, 1280.0f);
    float ch = (std::min)(H - 80.0f, 800.0f);
    float cx = (W - cw) * 0.5f, cy = (H - ch) * 0.5f;
    float bar_h = 48.0f;

    prim::drawShadow(ctx, br, cx, cy, cw, ch, 16.0f, pal.shadow_card_hover, t, 6.0f, 5);
    prim::fillRR(ctx, cx, cy, cw, ch, 16.0f, br.solidA(pal.card, t));
    prim::fillRect(ctx, cx, cy + bar_h - 1, cw, 1, br.solidA(pal.divider, t));

    // Title bar.
    auto* h1 = app.texts().format(L"Microsoft YaHei UI", ptToDip(11.0f),
                                  DWRITE_FONT_WEIGHT_BOLD);
    auto* sub = app.texts().format(L"Microsoft YaHei UI", ptToDip(8.5f));
    prim::drawText_(ctx, L"Web", h1,
                    cx + 16, cy + 12, 200, 22,
                    br.solidA(pal.text, t),
                    DWRITE_TEXT_ALIGNMENT_LEADING,
                    DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    prim::drawText_(ctx, g_webview_modal.title, sub,
                    cx + 100, cy + 16, cw - 220, 18,
                    br.solidA(pal.text_muted, t));

    // Close button.
    LayoutRect close_btn{ cx + cw - 36, cy + 10, 28, 28 };
    bool ch_h = close_btn.contains(g_mouse);
    if (ch_h) {
        prim::fillRR(ctx, close_btn.x, close_btn.y, 28, 28, 6,
                     br.solidA(pal.text, t * 0.10f));
    }
    icons::drawIcon(app, icons::Name::X, close_btn.x + 6, close_btn.y + 6, 16,
                    fadeArgb(pal.text, t));
    hit(close_btn, [](){ closeWebViewModal(); }, true);

    // Content area maps to WebView2 physical pixels.
    if (t > 0.95f && webview::isReady()) {
        float scale = app.dpi() / 96.0f;
        int wl = (int)((cx + 8) * scale);
        int wt = (int)((cy + bar_h) * scale);
        int wr = (int)((cx + cw - 8) * scale);
        int wb = (int)((cy + ch - 8) * scale);
        webview::setBounds(wl, wt, wr, wb);
        webview::show(true);
    } else {
        // Placeholder while WebView2 is not ready or runtime is missing.
        prim::fillRR(ctx, cx + 8, cy + bar_h, cw - 16, ch - bar_h - 8, 8.0f,
                     br.solidA(pal.surface, t));
        if (!webview::runtimeAvailable()) {
            prim::drawText_(ctx, L"WebView2 Runtime missing. Install Edge WebView2.",
                            sub,
                            cx, cy + ch * 0.5f, cw, 22,
                            br.solidA(pal.text_muted, t),
                            DWRITE_TEXT_ALIGNMENT_CENTER);
        } else {
            prim::drawText_(ctx, L"Loading...", sub,
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
    drawField(app, g_renamepack.input, cx + 30, cy + 80, cw - 60, 40, L"New name", true, t);
    hit(g_renamepack.input.bounds, [](){}, true);
    if (!g_renamepack.error_msg.empty()) {
        prim::drawText_(ctx, g_renamepack.error_msg, sub,
                        cx + 30, cy + 130, cw - 60, 18,
                        br.solidA(0xE34B4B, t));
    }
    float by = cy + ch - 52;
    drawGhostBtn(app, cx + 30, by, 120, 36, L"取消", t, [](){ closeRenamePack(); });
    drawPrimaryBtn(app, cx + cw - 30 - 160, by, 160, 36,
                   g_renamepack.busy ? L"Saving..." : L"Save", t,
                   [hwnd = GetActiveWindow()](){ submitRenamePack(hwnd); });
}

// ============== Message context menu ==============
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
    g_msg_menu.profile_key = (m.from == L"me")
        ? utf8ToWModal(g_user_id)
        : (!m.peer_key.empty() ? m.peer_key : (!m.author_key.empty() ? m.author_key : m.from));
    g_msg_menu.server_id = m.server_id;
    g_msg_menu.reply_author = m.reply_author;
    g_msg_menu.reply_preview = m.reply_preview;
    g_msg_menu.t.start(0, 1, 0.18f, 0, curve::easeOutCubic);
}
static void closeMsgMenu() {
    g_msg_menu.t.start(g_msg_menu.t.value(), 0, 0.14f, 0, curve::easeOutCubic);
    g_msg_menu.open = false;
}

void openUserContextMenu(POINT anchor_dip, const std::wstring& profile_key, const std::wstring& label) {
    if (profile_key.empty()) return;
    g_user_menu.open = true;
    g_user_menu.anchor = anchor_dip;
    g_user_menu.profile_key = profile_key;
    g_user_menu.label = label.empty() ? profile_key : label;
    g_user_menu.t.start(0, 1, 0.18f, 0, curve::easeOutCubic);
    std::string chat_id = chat::activeChatId();
    if (!chat_id.empty()) fetch::moderationMember(GetActiveWindow(), chat_id, profile_key);
}
static void closeUserMenu() {
    g_user_menu.t.start(g_user_menu.t.value(), 0, 0.14f, 0, curve::easeOutCubic);
    g_user_menu.open = false;
}

static int64_t muteDurationSeconds() {
    int64_t n = _wtoi64(g_mute_user.duration.text.c_str());
    if (n <= 0) n = 5;
    switch (g_mute_user.unit) {
        case 0: return n;
        case 2: return n * 3600;
        case 3: return n * 86400;
        default: return n * 60;
    }
}

void openMuteUser(const std::wstring& target_user_id, const std::wstring& target_label) {
    if (target_user_id.empty()) return;
    g_mute_user.open = true;
    g_mute_user.busy = false;
    g_mute_user.target_user_id = target_user_id;
    g_mute_user.target_label = target_label.empty() ? target_user_id : target_label;
    g_mute_user.duration.text = L"30";
    g_mute_user.duration.cursor = (int)g_mute_user.duration.text.size();
    g_mute_user.reason.text.clear();
    g_mute_user.reason.cursor = 0;
    g_mute_user.unit = 1;
    g_mute_user.focus = 1;
    g_mute_user.error_msg.clear();
    g_mute_user.chat_id = chat::activeChatId();
    g_mute_user.t.start(0, 1, 0.22f, 0, curve::easeOutCubic);
}
static void closeMuteUser() {
    g_mute_user.t.start(g_mute_user.t.value(), 0, 0.16f, 0, curve::easeOutCubic);
    g_mute_user.open = false;
}

static void submitMuteUser(HWND hwnd) {
    if (g_mute_user.busy) return;
    if (g_mute_user.chat_id.empty()) {
        g_mute_user.error_msg = L"Channel not ready";
        return;
    }
    if (g_mute_user.reason.text.empty()) {
        g_mute_user.error_msg = L"请输入原因";
        return;
    }
    g_mute_user.busy = true;
    fetch::muteUser(hwnd, g_mute_user.chat_id, g_mute_user.target_user_id,
                    muteDurationSeconds(), g_mute_user.reason.text);
}

void onMuteUserResult(bool success) {
    g_mute_user.busy = false;
    if (success) {
        toast::show(L"禁言已提交");
        closeMuteUser();
    } else {
        std::lock_guard<std::mutex> lk(fetch::g_moderation_mtx);
        g_mute_user.error_msg = fetch::g_moderation_member.error.empty()
            ? L"禁言失败" : fetch::g_moderation_member.error;
    }
}

void onUnmuteUserResult(bool success) {
    if (success) toast::show(L"已解除禁言");
    else {
        std::lock_guard<std::mutex> lk(fetch::g_moderation_mtx);
        toast::show(fetch::g_moderation_member.error.empty()
            ? L"解除禁言失败" : fetch::g_moderation_member.error);
    }
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
    if (g_msg_menu.server_id > 0) items.push_back({ trW("msg.reply"), [](){
        std::wstring body = g_msg_menu.body;
        if (body.size() > 80) body = body.substr(0, 80) + L"...";
        if (body.empty()) {
            chat::MsgKind k = (chat::MsgKind)g_msg_menu.kind_int;
            if (k == chat::MsgKind::Image) body = L"[image]";
            else if (k == chat::MsgKind::Gif) body = L"[gif]";
            else if (k == chat::MsgKind::Sticker) body = L"[sticker]";
            else if (k == chat::MsgKind::Video) body = L"[video]";
            else body = L"[message]";
        }
        chat::g_pending_reply.active = true;
        chat::g_pending_reply.id = g_msg_menu.server_id;
        chat::g_pending_reply.slug = g_msg_menu.slug;
        chat::g_pending_reply.author = g_msg_menu.author;
        chat::g_pending_reply.author_key = g_msg_menu.profile_key;
        chat::g_pending_reply.preview = body;
        if (g_msg_menu.from != L"me" && !g_msg_menu.profile_key.empty()) {
            chat::addMentionToComposer(g_msg_menu.profile_key, g_msg_menu.author);
        }
        chat::g_focus_composer = true;
        closeMsgMenu();
    }, false });
    if (!g_msg_menu.profile_key.empty()) {
        items.push_back({ L"View profile", [](){
            std::wstring key = g_msg_menu.profile_key;
            closeMsgMenu();
            openUserProfile(key);
        }, false });
    }
    items.push_back({ trW("msg.copy"), [](){
        copyTextToClipboard(GetActiveWindow(),
            g_msg_menu.body.empty() ? L"" : g_msg_menu.body);
        toast::show(trW("toast.copied"));
        closeMsgMenu();
    }, false });
    if (is_media) {
        items.push_back({ trW("msg.add_emoji"), [](){
            // Copy current media to a temporary folder before importing it as emoji.
            // importFromFolder handles the actual single-file import path.
            std::wstring path = g_msg_menu.body;
            // Copy the file into a temporary import directory.
            wchar_t tmp[MAX_PATH] = {0};
            GetTempPathW(MAX_PATH, tmp);
            std::wstring tmpdir = std::wstring(tmp) + L"launcher_add_emoji\\";
            CreateDirectoryW(tmpdir.c_str(), nullptr);
            // Remove old files from the temporary import directory.
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
            // Find the backend pack id for the local My Stickers pack.
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
            int64_t server_id = g_msg_menu.server_id;
            if (server_id > 0) {
                chat::deleteMessage(GetActiveWindow(), g_msg_menu.slug, server_id);
                toast::show(trW("toast.deleted_all"));
            } else {
                // No server id yet; remove only the local pending message.
                if (idx >= 0 && idx < (int)msgs.size()
                    && msgs[idx].server_id == 0
                    && msgs[idx].from == g_msg_menu.from
                    && msgs[idx].body == g_msg_menu.body) {
                    msgs.erase(msgs.begin() + idx);
                } else {
                    for (auto it = msgs.begin(); it != msgs.end(); ++it) {
                        if (it->server_id == 0
                            && it->from == g_msg_menu.from
                            && it->body == g_msg_menu.body) {
                            msgs.erase(it);
                            break;
                        }
                    }
                }
                toast::show(trW("toast.deleted_local"));
            }
            closeMsgMenu();
        }, true });
    }
    (void)is_text;

    // Menu layout.
    float mw = 180;
    float row_h = 32;
    float mh = (float)items.size() * row_h + 12;
    float mx = (float)g_msg_menu.anchor.x;
    float my = (float)g_msg_menu.anchor.y;
    if (mx + mw > W - 8) mx = W - 8 - mw;
    if (my + mh > H - 8) my = H - 8 - mh;
    if (mx < 8) mx = 8;
    if (my < 8) my = 8;

    // Register a full-screen catchall hit; item hits consume clicks first.
    // Clicking outside the menu closes it.
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

void paintUserContextMenu(D2DApp& app, float W, float H) {
    if (!g_user_menu.open && g_user_menu.t.value() < 0.001f) return;
    float t = g_user_menu.t.value();
    if (t < 0.001f) return;
    const Palette& pal = palette();
    auto* ctx = app.ctx();
    auto& br = app.brushes();

    fetch::ModerationMemberState mod;
    {
        std::lock_guard<std::mutex> lk(fetch::g_moderation_mtx);
        mod = fetch::g_moderation_member;
    }
    if (mod.target_user_id != g_user_menu.profile_key) {
        mod = fetch::ModerationMemberState{};
    }

    struct Item { std::wstring label; std::function<void()> click; bool danger; bool disabled; };
    std::vector<Item> items;
    items.push_back({ L"查看个人信息", [](){
        std::wstring key = g_user_menu.profile_key;
        closeUserMenu();
        openUserProfile(key);
    }, false, false });
    items.push_back({ L"@提及", [](){
        chat::addMentionToComposer(g_user_menu.profile_key, g_user_menu.label);
        closeUserMenu();
    }, false, false });
    bool self = isSelfProfileKey(g_user_menu.profile_key);
    if (g_user.is_admin && !self) {
        if (!mod.loaded) {
            items.push_back({ L"加载管理状态...", [](){}, false, true });
        } else if (!mod.ok) {
            items.push_back({ mod.error.empty() ? L"管理状态加载失败" : mod.error, [](){}, false, true });
        } else if (mod.active) {
            items.push_back({ L"解除禁言", [](){
                std::string chat_id = chat::activeChatId();
                if (!chat_id.empty()) {
                    fetch::unmuteUser(GetActiveWindow(), chat_id,
                                      g_user_menu.profile_key, L"Manual unmute");
                }
                closeUserMenu();
            }, false, !mod.can_unmute });
        } else {
            items.push_back({ L"禁言", [](){
                std::wstring key = g_user_menu.profile_key;
                std::wstring label = g_user_menu.label;
                closeUserMenu();
                openMuteUser(key, label);
            }, true, !mod.can_mute });
        }
    }

    float mw = 190.0f;
    float row = 32.0f;
    float mh = 12.0f + row * (float)items.size() + 12.0f;
    float mx = (float)g_user_menu.anchor.x;
    float my = (float)g_user_menu.anchor.y;
    if (mx + mw > W - 8) mx = W - mw - 8;
    if (my + mh > H - 8) my = H - mh - 8;
    if (mx < 8) mx = 8;
    if (my < 8) my = 8;
    prim::fillRR(ctx, mx, my, mw, mh, 8, br.solidA(pal.card, t));
    prim::strokeRR(ctx, mx, my, mw, mh, 8, br.solidA(pal.divider, t), 1.0f);
    auto* fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(10.0f));
    for (size_t i = 0; i < items.size(); ++i) {
        float y = my + 8 + row * (float)i;
        LayoutRect r{ mx + 6, y, mw - 12, row };
        bool hov = !items[i].disabled && r.contains(g_mouse);
        if (hov) prim::fillRR(ctx, r.x, r.y, r.w, r.h, 6, br.solidA(pal.primary, 0.12f * t));
        uint32_t color = items[i].disabled ? pal.text_muted : (items[i].danger ? 0xFFE8795C : pal.text);
        prim::drawText_(ctx, items[i].label, fmt, r.x + 10, r.y + 8, r.w - 20, 16,
                        br.solidA(color, items[i].disabled ? 0.55f * t : t));
        if (!items[i].disabled) hit(r, items[i].click, true);
    }
}

void paintMuteUserModal(D2DApp& app, float W, float H) {
    if (!g_mute_user.open && g_mute_user.t.value() < 0.001f) return;
    float t = g_mute_user.t.value();
    if (t < 0.001f) return;
    paintDim(app, W, H, t);
    const Palette& pal = palette();
    auto* ctx = app.ctx();
    auto& br = app.brushes();
    float cw = 460, ch = 350;
    float cx = (W - cw) * 0.5f;
    float cy = (H - ch) * 0.5f;
    prim::fillRR(ctx, cx, cy, cw, ch, 14, br.solidA(pal.card, t));
    prim::strokeRR(ctx, cx, cy, cw, ch, 14, br.solidA(pal.divider, t), 1.0f);
    auto* h1 = app.texts().format(L"Microsoft YaHei UI", ptToDip(15.0f), DWRITE_FONT_WEIGHT_BOLD);
    auto* sub = app.texts().format(L"Microsoft YaHei UI", ptToDip(9.5f));
    prim::drawText_(ctx, L"禁言用户", h1, cx + 28, cy + 22, cw - 56, 24, br.solidA(pal.text, t));
    std::wstring line = L"目标：" + g_mute_user.target_label;
    prim::drawText_(ctx, line, sub, cx + 28, cy + 52, cw - 56, 18, br.solidA(pal.text_muted, t));

    drawField(app, g_mute_user.duration, cx + 28, cy + 86, 120, 38, L"30", true, t);
    hit(g_mute_user.duration.bounds, [](){ g_mute_user.focus = 0; }, true);
    const wchar_t* units[] = { L"秒", L"分钟", L"小时", L"天" };
    float ux = cx + 160;
    for (int i = 0; i < 4; ++i) {
        LayoutRect r{ ux + i * 66.0f, cy + 88, 58, 32 };
        bool on = g_mute_user.unit == i;
        bool hov = r.contains(g_mouse);
        prim::fillRR(ctx, r.x, r.y, r.w, r.h, 8,
                     br.solidA(on ? pal.primary : pal.surface, (on ? 0.95f : (hov ? 0.55f : 0.35f)) * t));
        prim::drawText_(ctx, units[i], sub, r.x, r.y + 8, r.w, 14,
                        br.solidA(on ? 0xFFFFFFFF : pal.text, t),
                        DWRITE_TEXT_ALIGNMENT_CENTER);
        hit(r, [i](){ g_mute_user.unit = i; }, true);
    }
    drawField(app, g_mute_user.reason, cx + 28, cy + 146, cw - 56, 108, L"原因", true, t);
    hit(g_mute_user.reason.bounds, [](){ g_mute_user.focus = 1; }, true);

    if (!g_mute_user.error_msg.empty()) {
        prim::drawText_(ctx, g_mute_user.error_msg, sub, cx + 28, cy + 262, cw - 56, 18,
                        br.solidA(0xFFE8795C, t));
    }
    float by = cy + ch - 54;
    drawGhostBtn(app, cx + 28, by, 120, 36, L"取消", t, [](){ closeMuteUser(); });
    drawPrimaryBtn(app, cx + cw - 188, by, 160, 36,
                   g_mute_user.busy ? L"提交中..." : L"确认禁言", t,
                   [hwnd = GetActiveWindow()](){ submitMuteUser(hwnd); });
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
        prim::drawText_(ctx, L"Loading...", sub,
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
        // Title and creator.
        prim::drawText_(ctx, pv.name.empty() ? L"分享的表情包" : pv.name, h1,
                        cx + 30, cy + 22, cw - 60, 30,
                        br.solidA(pal.text, t));
        std::wstring meta;
        if (!pv.creator_name.empty()) meta = L"by " + pv.creator_name;
        else meta = L"分享的表情包";
        wchar_t buf[64];
        swprintf_s(buf, L"%ls - installed %d times", meta.c_str(), pv.install_count);
        prim::drawText_(ctx, buf, mt,
                        cx + 30, cy + 54, cw - 60, 18,
                        br.solidA(pal.text_muted, t));

        // Sticker preview grid.
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
                swprintf_s(mw, L"+ %d more", (int)pv.sticker_paths.size() - max_show);
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
                         L"Installed", t, [](){
                            closePackPreview();
                            toast::show(L"Pack already installed");
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
    // Close button.
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

// ============== Search modal (Ctrl+F) ==============
void openSearch() {
    g_search.open = true;
    g_search.input.text.clear();
    g_search.input.cursor = 0;
    g_search.input.clearSel();
    g_search.results.clear();
    g_search.scroll = 0;
    g_search.last_query.clear();
    g_search.busy = false;
    g_search.t.start(0, 1, 0.22f, 0, curve::easeOutCubic);
}
static void closeSearch() {
    g_search.t.start(g_search.t.value(), 0, 0.18f, 0, curve::easeOutCubic);
    g_search.open = false;
}
namespace {
struct SearchArg { std::wstring q; HWND h; };
std::mutex g_search_mtx;
std::string g_search_pending_body;

std::wstring utf8wSearch(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(n - 1, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

void kickSearch(HWND hwnd, const std::wstring& q) {
    g_search.busy = true;
    auto* a = new SearchArg{ q, hwnd };
    CreateThread(nullptr, 0, [](LPVOID lp) -> DWORD {
        std::unique_ptr<SearchArg> a((SearchArg*)lp);
        if (g_session_token.empty()) return 0;
        std::string qu = net::jsonEscape(a->q);
        // URL-encode the search query.
        // jsonEscape is not URL encoding, so encode query-string bytes here.
        std::string enc;
        for (unsigned char c : qu) {
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
                || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~')
                enc.push_back(c);
            else { char buf[4]; sprintf_s(buf, "%%%02X", c); enc += buf; }
        }
        std::string url = "/api/chat/search?session_token=" + g_session_token
                        + "&q=" + enc + "&limit=50";
        std::wstring wurl(url.begin(), url.end());
        auto r = net::request(L"GET", wurl.c_str(), {}, L"");
        if (r.ok()) {
            std::lock_guard<std::mutex> lk(g_search_mtx);
            g_search_pending_body = r.body;
        }
        PostMessageW(a->h, WM_APP + 55, r.ok() ? 1 : 0, 0);
        return 0;
    }, a, 0, nullptr);
}

void runSearchIfChanged(HWND hwnd) {
    std::wstring q = g_search.input.text;
    while (!q.empty() && (q.front() == L' ')) q.erase(q.begin());
    while (!q.empty() && (q.back()  == L' ')) q.pop_back();
    if (q == g_search.last_query) return;
    g_search.last_query = q;
    if (q.size() < 2) {
        g_search.results.clear();
        return;
    }
    kickSearch(hwnd, q);
}
}  // anon

void drainSearchResult() {
    std::string body;
    {
        std::lock_guard<std::mutex> lk(g_search_mtx);
        body = std::move(g_search_pending_body);
        g_search_pending_body.clear();
    }
    onSearchResult(body);
}

void onSearchResult(const std::string& body) {
    g_search.busy = false;
    g_search.results.clear();
    g_search.scroll = 0;
    // body = [{id,chat_id,chat_slug,sender_id,msg_type,payload,created_at}, ...]
    size_t pos = 0;
    while (true) {
        auto ob = body.find('{', pos);
        if (ob == std::string::npos) break;
        auto cb = net::findJsonObjectEnd(body, ob);
        if (cb == std::string::npos) break;
        std::string obj = body.substr(ob, cb - ob + 1);
        SearchHit h;
        h.msg_id    = net::jsonInt(obj, "id");
        h.chat_id   = utf8wSearch(net::jsonStr(obj, "chat_id"));
        h.slug      = utf8wSearch(net::jsonStr(obj, "chat_slug"));
        h.sender_id = utf8wSearch(net::jsonStr(obj, "sender_id"));
        h.kind      = utf8wSearch(net::jsonStr(obj, "msg_type"));
        std::string pl = net::jsonStr(obj, "payload");
        if (pl.size() >= 2 && pl.front() == '"' && pl.back() == '"') {
            pl = pl.substr(1, pl.size() - 2);
        }
        h.payload = utf8wSearch(pl);
        int64_t ts = net::jsonInt(obj, "created_at");
        if (ts > 0) {
            time_t tt = (time_t)ts;
            struct tm lt{};
            localtime_s(&lt, &tt);
            wchar_t tbuf[24];
            swprintf_s(tbuf, L"%02d-%02d %02d:%02d",
                       lt.tm_mon + 1, lt.tm_mday, lt.tm_hour, lt.tm_min);
            h.time = tbuf;
        }
        if (h.msg_id > 0) g_search.results.push_back(std::move(h));
        pos = cb + 1;
    }
}

void paintSearchModal(D2DApp& app, float W, float H) {
    if (!g_search.open && g_search.t.value() < 0.001f) return;
    float t = g_search.t.value();
    if (t < 0.001f) return;
    paintDim(app, W, H, t);

    const Palette& pal = palette();
    auto* ctx = app.ctx();
    auto& br = app.brushes();

    float cw = 540, ch = 480;
    float cx = (W - cw) * 0.5f, cy = (H - ch) * 0.5f + 8 * (1.0f - t);
    prim::drawShadow(ctx, br, cx, cy, cw, ch, 16.0f, pal.shadow_card_hover, t, 6.0f, 4);
    prim::fillRR(ctx, cx, cy, cw, ch, 16.0f, br.solidA(pal.card, t));

    auto* h1 = app.texts().format(L"Microsoft YaHei UI", ptToDip(13.0f),
                                  DWRITE_FONT_WEIGHT_BOLD);
    auto* sub = app.texts().format(L"Microsoft YaHei UI", ptToDip(9.0f));
    auto* row_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(9.5f));
    auto* meta = app.texts().format(L"Microsoft YaHei UI", ptToDip(8.0f));

    prim::drawText_(ctx, L"搜索消息", h1,
                    cx + 24, cy + 22, cw - 48, 22,
                    br.solidA(pal.text, t));
    wchar_t hint_buf[64];
    swprintf_s(hint_buf, L"Search results - %d matches",
               (int)g_search.results.size());
    prim::drawText_(ctx, hint_buf, sub,
                    cx + 24, cy + 50, cw - 48, 18,
                    br.solidA(pal.text_muted, t));

    // Search box.
    drawField(app, g_search.input, cx + 24, cy + 78, cw - 48, 40,
              L"Search messages", true, t);
    hit(g_search.input.bounds, [](){}, true);

    // 结果列表
    float lx = cx + 24, ly = cy + 130;
    float lw = cw - 48, lh = ch - 130 - 60;
    prim::fillRR(ctx, lx, ly, lw, lh, 8.0f, br.solidA(pal.surface, t * 0.4f));
    ctx->PushAxisAlignedClip(D2D1::RectF(lx, ly, lx + lw, ly + lh),
                             D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    if (g_search.results.empty()) {
        const wchar_t* m_ = g_search.busy ? L"Searching..."
                          : (g_search.input.text.size() < 2
                                ? L"Enter at least 2 characters"
                                : L"No results");
        prim::drawText_(ctx, m_, sub,
                        lx, ly + lh * 0.4f, lw, 22,
                        br.solidA(pal.text_muted, t),
                        DWRITE_TEXT_ALIGNMENT_CENTER);
    } else {
        float row_h = 56;
        float ry = ly + 4 - g_search.scroll;
        for (size_t i = 0; i < g_search.results.size(); ++i) {
            auto& r = g_search.results[i];
            if (ry + row_h < ly) { ry += row_h + 4; continue; }
            if (ry > ly + lh) break;
            LayoutRect rr{ lx + 4, ry, lw - 8, row_h };
            bool hov = rr.contains(g_mouse);
            if (hov) {
                prim::fillRR(ctx, rr.x, rr.y, rr.w, rr.h, 8.0f,
                             br.solidA(pal.primary, t * 0.10f));
            }
            // First row: channel and time.
            wchar_t head[128];
            swprintf_s(head, L"#%.16ls  ·  %.16ls",
                       r.slug.empty() ? L"?" : r.slug.c_str(),
                       r.time.c_str());
            prim::drawText_(ctx, head, meta,
                            rr.x + 12, rr.y + 8, rr.w - 24, 14,
                            br.solidA(pal.text_muted, t));
            // Second row: message preview.
            std::wstring body = r.payload;
            if (body.size() > 80) body = body.substr(0, 80) + L"...";
            prim::drawText_(ctx, body, row_fmt,
                            rr.x + 12, rr.y + 26, rr.w - 24, 24,
                            br.solidA(pal.text, t));
            int64_t mid = r.msg_id;
            std::wstring sl = r.slug;
            hit(rr, [mid, sl](){
                chat::focusMessage(sl, mid);
                closeSearch();
            }, true);
            ry += row_h + 4;
        }
    }
    ctx->PopAxisAlignedClip();

    // Footer button.
    drawGhostBtn(app, cx + cw - 24 - 100, cy + ch - 52, 100, 36,
                 trW("common.close").c_str(), t, [](){ closeSearch(); });
}

// ============== Event routing ==============
bool onMouseLDown(HWND /*hwnd*/, POINT dip) {
    if (!anyOpen()) return false;
    // Context menu: outside click closes; menu rows are handled by dispatchClick.
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
        if (g_search.open) closeSearch();
        else if (g_user_menu.open) closeUserMenu();
        else if (g_mute_user.open) closeMuteUser();
        else if (g_pack_preview_modal.open) closePackPreview();
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
    if (g_search.open) {
        g_search.input.onChar(c, ctrl, hwnd);
        runSearchIfChanged(hwnd);
        return true;
    }
    if (g_mute_user.open) {
        if (g_mute_user.focus == 0) {
            if ((c >= L'0' && c <= L'9') || c == 0x08 || ctrl) {
                g_mute_user.duration.onChar(c, ctrl, hwnd);
            }
        } else {
            g_mute_user.reason.onChar(c, ctrl, hwnd);
        }
        return true;
    }
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
        if (g_search.open) { closeSearch(); return true; }
        if (g_user_menu.open) { closeUserMenu(); return true; }
        if (g_mute_user.open) { closeMuteUser(); return true; }
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
    if (g_search.open) {
        g_search.input.onKey(vk, shift, ctrl);
        runSearchIfChanged(hwnd);
        return true;
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
    if (g_mute_user.open) {
        if (vk == VK_RETURN && !ctrl) { submitMuteUser(hwnd); return true; }
        if (g_mute_user.focus == 0) g_mute_user.duration.onKey(vk, shift, ctrl);
        else g_mute_user.reason.onKey(vk, shift, ctrl);
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
