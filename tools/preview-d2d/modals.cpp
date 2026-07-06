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
#include <ctime>
#include <memory>
#include <mutex>
#include <set>
#include <cstdio>

namespace launcher::d2d::modal {

void dismissTopModal();  // 定义在文件后段;paintDim(匿名命名空间内)经限定名调用。

ChangePwState     g_change_pw;
ConfirmState      g_confirm;
CS2State          g_cs2;
MarketDetailState g_market_detail_modal;
HistoryState      g_history;
AddTagState       g_addtag;
CreatePackState   g_createpack;
RenamePackState   g_renamepack;
UserProfileState  g_user_profile;
EditStatusTextState g_edit_status;
EditBioState      g_edit_bio;
EditNicknameState g_edit_nickname;
WebViewModalState g_webview_modal;
MsgContextMenuState g_msg_menu;
UserContextMenuState g_user_menu;
ChatMoreMenuState g_chat_more;
MuteUserState     g_mute_user;
PackPreviewState  g_pack_preview_modal;
SearchState       g_search;

static void closeMsgMenu();
static void closeUserMenu();
// 互斥：打开任一 modal 前先关掉其它已开的 modal/卡片，避免双窗口叠加
// （例：先点头像开资料卡，再点分享表情包卡片，两个卡片会同时浮在屏上）。
// 定义在文件末尾（所有 close 函数之后），此处前置声明供各 openX() 调用。
void dismissAllModals();

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
    // 全屏背景遮罩 hit:吞掉落到模态之外的点击,防止穿透触发后面主页面控件;
    // 点它 = 关闭最上层模态(点外返回)。模态自身的按钮 hit 在此之后注册,
    // dispatchClick 反向遍历时先命中模态按钮,点模态外才落到这个遮罩上。
    // dismissTopModal 是 modal 命名空间(非本 anon)的函数,用限定名。
    hit({ 0, 0, W, H }, [](){ launcher::d2d::modal::dismissTopModal(); }, false);
    (void)pal;
}

// Primary button with pressed/hover states.
void drawPrimaryBtn(D2DApp& app, float x, float y, float w, float h,
                    std::wstring_view label, float op,
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
                  std::wstring_view label, float op,
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
               std::wstring_view placeholder, bool focused, float op) {
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
    g_market_detail_modal.t.tick(dt);
    g_market_detail_modal.review_input.float_t.tick(dt);
    g_history.t.tick(dt);
    g_history.page_anim.tick(dt);
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
    g_edit_nickname.t.tick(dt);
    g_edit_nickname.input.float_t.tick(dt);
    g_webview_modal.t.tick(dt);
    g_msg_menu.t.tick(dt);
    g_user_menu.t.tick(dt);
    g_chat_more.t.tick(dt);
    g_mute_user.t.tick(dt);
    g_mute_user.duration.float_t.tick(dt);
    g_mute_user.reason.float_t.tick(dt);
    g_pack_preview_modal.t.tick(dt);
    g_search.t.tick(dt);
    g_search.input.float_t.tick(dt);
}

bool anyOpen() {
    return g_change_pw.open || g_confirm.open || g_cs2.open || g_history.open
        || g_market_detail_modal.open
        || g_addtag.open || g_createpack.open || g_renamepack.open
        || g_user_profile.open || g_edit_status.open || g_edit_bio.open
        || g_edit_nickname.open
        || g_webview_modal.open || g_msg_menu.open || g_user_menu.open || g_chat_more.open
        || g_mute_user.open
        || g_pack_preview_modal.open
        || g_search.open;
}

bool hasBlockingModalOpen() {
    return g_change_pw.open || g_confirm.open || g_cs2.open || g_history.open
        || g_market_detail_modal.open
        || g_addtag.open || g_createpack.open || g_renamepack.open
        || g_user_profile.open || g_edit_status.open || g_edit_bio.open
        || g_edit_nickname.open
        || g_webview_modal.open || g_mute_user.open || g_pack_preview_modal.open
        || g_search.open;
}

// ============== ChangePw ==============
void openChangePw() {
    dismissAllModals();
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
        g_change_pw.error_msg = trW("pw.empty"); return;
    }
    if (g_change_pw.new_pw.text != g_change_pw.repeat_pw.text) {
        g_change_pw.error_msg = trW("pw.mismatch"); return;
    }
    if (g_change_pw.new_pw.text.size() < 6) {
        g_change_pw.error_msg = trW("pw.too_short"); return;
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
                if (m.empty()) {
                    g_pw_pending_error = trW("pw.change_failed");
                } else {
                    int n = MultiByteToWideChar(CP_UTF8, 0, m.c_str(), -1, nullptr, 0);
                    std::wstring w(n > 0 ? n - 1 : 0, 0);
                    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, m.c_str(), -1, w.data(), n);
                    g_pw_pending_error = w;
                }
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
            ? trW("pw.change_failed") : g_pw_pending_error;
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
    prim::drawText_(ctx, trW("profile.change_pw"), h1,
                    cx + 30, cy + 28, cw - 60, 24,
                    br.solidA(pal.text, t));
    prim::drawText_(ctx, trW("pw.signin_again_hint"), sub,
                    cx + 30, cy + 56, cw - 60, 18,
                    br.solidA(pal.text_muted, t));

    float fy = cy + 90;
    drawField(app, g_change_pw.old_pw, cx + 30, fy, cw - 60, 40,
              trW("pw.current"), g_change_pw.focus == 0, t);
    fy += 50;
    drawField(app, g_change_pw.new_pw, cx + 30, fy, cw - 60, 40,
              trW("pw.new_label"), g_change_pw.focus == 1, t);
    fy += 50;
    drawField(app, g_change_pw.repeat_pw, cx + 30, fy, cw - 60, 40,
              trW("pw.repeat_label"), g_change_pw.focus == 2, t);
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
    drawGhostBtn(app, cx + 30, by, 130, 38, trW("common.cancel"), t,
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
    dismissAllModals();
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
        auto* cover_bmp = cs2_path.empty() ? nullptr : app.images().fromFile(cs2_path, 720);
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
        prim::drawText_(ctx, webview::runtimeAvailable() ? trW("webview.runtime_ready") :
                              trW("webview.no_runtime"),
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
                 trW("launch.store_page"), t, [](){ openCS2Store(); });
}

// ============== MarketDetail ==============
void openMarketDetail(const std::string& id) {
    dismissAllModals();
    g_market_detail_modal.open = true;
    g_market_detail_modal.listing_id = id;
    g_market_detail_modal.buying = false;
    g_market_detail_modal.reviewing = false;
    g_market_detail_modal.rating = 5;
    g_market_detail_modal.review_input.text.clear();
    g_market_detail_modal.review_input.cursor = 0;
    g_market_detail_modal.review_input.clearSel();
    g_market_detail_modal.error_msg.clear();
    g_market_detail_modal.t.start(0, 1, 0.30f, 0, curve::easeOutQuint);
    fetch::getListing(GetActiveWindow(), id);
}
static void closeMarketDetail() {
    g_market_detail_modal.t.start(g_market_detail_modal.t.value(), 0, 0.20f, 0,
                                  curve::easeOutQuint);
    g_market_detail_modal.open = false;
}

void onMarketPurchaseResult(bool success) {
    g_market_detail_modal.buying = false;
    fetch::MarketActionResult res;
    {
        std::lock_guard<std::mutex> lk(fetch::g_market_action_mtx);
        res = fetch::g_market_action;
    }
    if (success) {
        toast::show(trW("market.purchase_ok"));
    } else if (res.status == 402) {
        toast::show(trW("market.insufficient_credit"));
    } else {
        toast::show(trW("market.purchase_failed"));
    }
}

void onMarketReviewResult(bool success) {
    g_market_detail_modal.reviewing = false;
    if (success) {
        g_market_detail_modal.review_input.text.clear();
        g_market_detail_modal.review_input.cursor = 0;
        g_market_detail_modal.review_input.clearSel();
        toast::show(trW("market.review_ok"));
    } else {
        toast::show(trW("market.review_failed"));
    }
}

void paintMarketDetailModal(D2DApp& app, float W, float H) {
    if (!g_market_detail_modal.open && g_market_detail_modal.t.value() < 0.001f) return;
    float t = g_market_detail_modal.t.value();
    if (t < 0.001f) return;
    paintDim(app, W, H, t);

    const Palette& pal = palette();
    auto* ctx = app.ctx();
    auto& br = app.brushes();

    float cw = 460, ch = 480;
    float cx = (W - cw) * 0.5f, cy = (H - ch) * 0.5f + 8 * (1.0f - t);
    prim::drawShadow(ctx, br, cx, cy, cw, ch, 16.0f, pal.shadow_card_hover, t, 6.0f, 4);
    prim::fillRR(ctx, cx, cy, cw, ch, 16.0f, br.solidA(pal.card, t));

    // 关闭按钮
    {
        LayoutRect xb{ cx + cw - 40, cy + 14, 28, 28 };
        bool xh = xb.contains(g_mouse);
        if (xh) prim::fillRR(ctx, xb.x, xb.y, 28, 28, 6, br.solidA(pal.text, t * 0.10f));
        icons::drawIcon(app, icons::Name::X, xb.x + 6, xb.y + 6, 16, fadeArgb(pal.text, t));
        hit(xb, [](){ closeMarketDetail(); }, true);
    }

    auto* h1 = app.texts().format(L"Microsoft YaHei UI", ptToDip(15.0f),
                                  DWRITE_FONT_WEIGHT_BOLD);
    auto* sub = app.texts().format(L"Microsoft YaHei UI", ptToDip(9.5f));
    auto* body_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(10.0f));

    fetch::ListingDetail d;
    {
        std::lock_guard<std::mutex> lk(fetch::g_market_detail_mtx);
        d = fetch::g_market_detail;
    }
    // 详情还在拉 / 拉的是别的 id → 显示 loading
    bool matches = (d.id == g_market_detail_modal.listing_id);
    if (!matches || !d.loaded) {
        prim::drawText_(ctx, trW("common.loading"), sub,
                        cx + 28, cy + 24, cw - 96, 22, br.solidA(pal.text_muted, t));
        return;
    }
    if (!d.error.empty()) {
        prim::drawText_(ctx, trW("market.load_failed"), h1,
                        cx + 28, cy + 24, cw - 96, 24, br.solidA(pal.text, t));
        prim::drawText_(ctx, utf8ToWModal(d.error), sub,
                        cx + 28, cy + 56, cw - 56, 40, br.solidA(0xE34B4B, t));
        return;
    }

    // 标题 + 分类 + 描述
    prim::drawText_(ctx, d.title, h1,
                    cx + 28, cy + 24, cw - 96, 26, br.solidA(pal.text, t));
    prim::drawText_(ctx, d.category, sub,
                    cx + 28, cy + 54, cw - 56, 18, br.solidA(pal.text_muted, t));
    prim::drawText_(ctx, d.description, body_fmt,
                    cx + 28, cy + 80, cw - 56, 90, br.solidA(pal.text, t));

    // 价格 + 评分
    std::wstring price_line = trW("market.price_cents");
    if (auto p = price_line.find(L"{n}"); p != std::wstring::npos)
        price_line.replace(p, 3, std::to_wstring((long long)d.price_cents));
    prim::drawText_(ctx, price_line, h1,
                    cx + 28, cy + 176, cw - 56, 24, br.solidA(pal.primary, t));
    wchar_t rbuf[64];
    swprintf_s(rbuf, L"%.1f  (%d)", d.rating_avg, d.rating_count);
    prim::drawText_(ctx, rbuf, sub,
                    cx + 28, cy + 204, cw - 56, 18, br.solidA(pal.text_muted, t));

    bool signed_in = !g_session_token.empty();

    // 1-5 星选择行
    auto* star_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(18.0f),
                                        DWRITE_FONT_WEIGHT_BOLD);
    prim::drawText_(ctx, trW("market.your_rating"), sub,
                    cx + 28, cy + 232, cw - 56, 18, br.solidA(pal.text_muted, t));
    for (int i = 1; i <= 5; ++i) {
        LayoutRect sr{ cx + 28 + (i - 1) * 32.0f, cy + 252, 28, 28 };
        bool filled = i <= g_market_detail_modal.rating;
        prim::drawText_(ctx, filled ? L"★" : L"☆", star_fmt,
                        sr.x, sr.y, 28, 28,
                        br.solidA(filled ? pal.primary : pal.text_muted, t),
                        DWRITE_TEXT_ALIGNMENT_CENTER,
                        DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        if (signed_in) hit(sr, [i](){ g_market_detail_modal.rating = i; }, true);
    }

    // 评价文字（可选）
    drawField(app, g_market_detail_modal.review_input, cx + 28, cy + 290, cw - 56, 40,
              trW("market.review_placeholder"), signed_in, t);
    if (signed_in) hit(g_market_detail_modal.review_input.bounds, [](){}, true);

    if (!g_market_detail_modal.error_msg.empty()) {
        prim::drawText_(ctx, g_market_detail_modal.error_msg, sub,
                        cx + 28, cy + 336, cw - 56, 18, br.solidA(0xE34B4B, t));
    }

    // 底部按钮：Buy + Submit review
    float by = cy + ch - 56;
    bool self_listing = (d.seller_id == utf8ToWModal(g_user_id));
    bool can_buy = signed_in && !self_listing && d.status == L"active";
    if (can_buy) {
        std::wstring buy_label = g_market_detail_modal.buying
            ? trW("market.buying") : trW("market.buy");
        drawPrimaryBtn(app, cx + 28, by, 180, 40, buy_label, t,
            [hwnd = GetActiveWindow()](){
                if (g_market_detail_modal.buying) return;   // 非幂等：禁止重复点击
                g_market_detail_modal.buying = true;
                fetch::purchaseListing(hwnd, g_market_detail_modal.listing_id);
            });
    } else {
        // 登出 / 自售 / 已下架：给出禁用态说明
        std::wstring why = !signed_in ? trW("market.signin_to_buy")
            : (self_listing ? trW("market.own_listing") : trW("market.not_active"));
        prim::drawText_(ctx, why, sub, cx + 28, by + 12, 180, 18,
                        br.solidA(pal.text_muted, t));
    }
    if (signed_in) {
        std::wstring rev_label = g_market_detail_modal.reviewing
            ? trW("market.submitting") : trW("market.submit_review");
        drawGhostBtn(app, cx + cw - 28 - 180, by, 180, 40, rev_label, t,
            [hwnd = GetActiveWindow()](){
                if (g_market_detail_modal.reviewing) return;
                g_market_detail_modal.reviewing = true;
                fetch::reviewListing(hwnd, g_market_detail_modal.listing_id,
                                     g_market_detail_modal.rating,
                                     g_market_detail_modal.review_input.text);
            });
    }
}

// ============== History ==============
void openHistory() {
    dismissAllModals();
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

    // ch 需容纳:标题区(~90) + 5 行 × 50 = 250 + 分页(~44) + 关闭按钮(~52) + 边距。
    // 旧值 420 装不下(行区 cy+90..cy+340 压到 cy+330 的分页控件上 → 渲染重叠)。
    float cw = 460, ch = 540;
    float cx = (W - cw) * 0.5f, cy = (H - ch) * 0.5f + 8 * (1.0f - t);
    prim::drawShadow(ctx, br, cx, cy, cw, ch, 16.0f, pal.shadow_card_hover, t, 6.0f, 4);
    prim::fillRR(ctx, cx, cy, cw, ch, 16.0f, br.solidA(pal.card, t));

    auto* h1 = app.texts().format(L"Microsoft YaHei UI", ptToDip(13.0f),
                                  DWRITE_FONT_WEIGHT_BOLD);
    auto* sub = app.texts().format(L"Microsoft YaHei UI", ptToDip(9.0f));
    prim::drawText_(ctx, trW("acc.history"), h1,
                    cx + 24, cy + 22, cw - 48, 22,
                    br.solidA(pal.text, t));
    if (!g_history.loaded) {
        prim::drawText_(ctx, trW("common.loading"), sub,
                        cx + 24, cy + 50, cw - 48, 18,
                        br.solidA(pal.text_muted, t));
    } else {
        std::wstring info = trW("history.records_fmt");
        {
            auto p = info.find(L"{n}");
            if (p != std::wstring::npos)
                info.replace(p, 3, std::to_wstring((int)g_history.rows.size()));
        }
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
        // 画某一页的 5 行，整体加水平位移 dx（用于翻页滑动）。x 基准同旧代码
        // （行框 cx+20、圆点 cx+36、文字 cx+56），只在 x 上叠加 dx。
        auto drawPageRows = [&](int pageIdx, float dx) {
            int pbegin = pageIdx * per;
            int pend = (std::min)(pbegin + per, total);
            for (int i = pbegin; i < pend; ++i) {
                float ry = cy + 90 + (i - pbegin) * 50;
                prim::fillRR(ctx, cx + 20 + dx, ry, cw - 40, 40, 8.0f,
                             br.solidA(pal.surface, t));
                // 成功 = 绿点，失败 = 红点（之前恒为绿，看不出失败登录）。
                uint32_t dot = g_history.rows[i].success ? 0x4ADE80 : 0xE34B4B;
                prim::fillCircle(ctx, cx + 36 + dx, ry + 20, 6,
                                 br.solidA(dot, t));
                prim::drawText_(ctx, g_history.rows[i].text, row_fmt,
                                cx + 56 + dx, ry + 11, cw - 96, 18,
                                br.solidA(pal.text, t));
            }
        };
        // 裁剪到行区，滑动的两页不溢出卡片。区域覆盖 5 行 (90..90+5*50=340) + 少量 pad。
        ctx->PushAxisAlignedClip(D2D1::RectF(cx + 20, cy + 84, cx + cw - 20, cy + 90 + per * 50),
                                 D2D1_ANTIALIAS_MODE_ALIASED);
        bool animating = g_history.page_anim.started && !g_history.page_anim.done();
        if (animating) {
            float e = g_history.page_anim.value();         // 0→1
            int dir = g_history.slide_dir;                 // +1 next, -1 prev
            // 旧页从中心滑向 -dir 方向出场；新页从 dir 方向入场。
            drawPageRows(g_history.prev_page, (float)(-dir) * e * cw);
            drawPageRows(g_history.page,      (float)dir * (1.0f - e) * cw);
        } else {
            drawPageRows(g_history.page, 0.0f);
        }
        ctx->PopAxisAlignedClip();
    }
    // Pagination controls.
    if (total_pages > 1) {
        wchar_t pg[16]; swprintf_s(pg, L"%d / %d", g_history.page + 1, total_pages);
        prim::drawText_(ctx, pg, sub,
                        cx + cw * 0.5f - 30, cy + ch - 90, 60, 18,
                        br.solidA(pal.text_muted, t),
                        DWRITE_TEXT_ALIGNMENT_CENTER);
        int tp_capt = total_pages;
        drawGhostBtn(app, cx + cw * 0.5f - 90, cy + ch - 95, 30, 30, L"<", t,
                     [](){
                         if (g_history.page > 0) {
                             g_history.prev_page = g_history.page;
                             g_history.page--;
                             g_history.slide_dir = -1;
                             g_history.page_anim.start(0.0f, 1.0f, 0.22f, 0, curve::easeOutCubic);
                         }
                     });
        drawGhostBtn(app, cx + cw * 0.5f + 60, cy + ch - 95, 30, 30, L">", t,
                     [tp_capt](){
                         if (g_history.page < tp_capt - 1) {
                             g_history.prev_page = g_history.page;
                             g_history.page++;
                             g_history.slide_dir = 1;
                             g_history.page_anim.start(0.0f, 1.0f, 0.22f, 0, curve::easeOutCubic);
                         }
                     });
    }

    drawGhostBtn(app, cx + cw - 24 - 100, cy + ch - 52, 100, 36,
                 trW("common.close"), t, [](){ closeHistory(); });

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
        // 服务端字段名（profile.rs LoginEntry）：occurred_at(epoch i64)/success(bool)/
        // failure_reason/remote_ip/geo_country/geo_city —— 不是旧的 ts/ip/geo。
        long long occurred = net::jsonInt(obj, "occurred_at");
        std::string ip = net::jsonStr(obj, "remote_ip");
        std::string country = net::jsonStr(obj, "geo_country");
        std::string city = net::jsonStr(obj, "geo_city");
        std::string fail = net::jsonStr(obj, "failure_reason");
        // success 是 JSON bool：直接判字面量（jsonStr/jsonInt 读不准 bool）。
        bool ok = true;
        {
            size_t b = 0, e = 0;
            if (net::jsonValueRange(obj, "success", b, e))
                ok = obj.compare(b, 4, "true") == 0;
        }
        // epoch 秒 → "YYYY-MM-DD HH:MM" 本地时间。
        std::wstring tsw;
        if (occurred > 0) {
            time_t tt = (time_t)occurred;
            struct tm lt{};
            localtime_s(&lt, &tt);
            wchar_t buf[32];
            wcsftime(buf, 32, L"%Y-%m-%d %H:%M", &lt);
            tsw = buf;
        }
        std::wstring row = tsw;
        if (!ip.empty())      row += (row.empty() ? L"" : L"  ·  ") + utf8w(ip);
        std::string geo = country;
        if (!city.empty())    geo = city + (country.empty() ? "" : ", " + country);
        if (!geo.empty())     row += L"  ·  " + utf8w(geo);
        if (!ok && !fail.empty()) row += L"  ·  " + utf8w(fail);
        g_history.rows.push_back({ row, ok });
        pos = cb + 1;
    }
}

// ============== AddTag ==============
void openAddTag() {
    dismissAllModals();
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
    if (tag.empty()) { g_addtag.error_msg = trW("addtag.empty"); return; }
    if (tag.size() > 24) { g_addtag.error_msg = trW("addtag.too_long"); return; }
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
        if (status == 409) g_addtag.error_msg = trW("addtag.exists");
        else if (status == 429) g_addtag.error_msg = trW("addtag.limit");
        else g_addtag.error_msg = trW("addtag.failed");
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
    prim::drawText_(ctx, trW("addtag.title"), h1,
                    cx + 30, cy + 22, cw - 60, 22,
                    br.solidA(pal.text, t));
    prim::drawText_(ctx, trW("addtag.hint"), sub,
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
    drawGhostBtn(app, cx + 30, by, 120, 36, trW("common.cancel"), t,
                 [](){ closeAddTag(); });
    drawPrimaryBtn(app, cx + cw - 30 - 160, by, 160, 36,
                   g_addtag.busy ? L"Adding..." : L"Add", t,
                   [hwnd = GetActiveWindow()](){ submitAddTag(hwnd); });
}

// ============== CreatePack ==============
void openCreatePack() {
    dismissAllModals();
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
    if (name.empty()) { g_createpack.error_msg = trW("createpack.name_required"); return; }
    if (name.size() > 24) { g_createpack.error_msg = trW("createpack.too_long"); return; }
    if (g_session_token.empty()) { g_createpack.error_msg = trW("createpack.signin_required"); return; }
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
    else g_createpack.error_msg = trW("createpack.failed");
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
    prim::drawText_(ctx, trW("createpack.title"), h1, cx + 30, cy + 22, cw - 60, 22, br.solidA(pal.text, t));
    prim::drawText_(ctx, trW("createpack.hint"), sub, cx + 30, cy + 50, cw - 60, 18,
                    br.solidA(pal.text_muted, t));
    drawField(app, g_createpack.input, cx + 30, cy + 80, cw - 60, 40, trW("createpack.placeholder"), true, t);
    hit(g_createpack.input.bounds, [](){}, true);
    if (!g_createpack.error_msg.empty()) {
        prim::drawText_(ctx, g_createpack.error_msg, sub,
                        cx + 30, cy + 130, cw - 60, 18,
                        br.solidA(0xE34B4B, t));
    }
    float by = cy + ch - 52;
    drawGhostBtn(app, cx + 30, by, 120, 36, trW("common.cancel"), t, [](){ closeCreatePack(); });
    drawPrimaryBtn(app, cx + cw - 30 - 160, by, 160, 36,
                   g_createpack.busy ? L"Creating..." : L"Create", t,
                   [hwnd = GetActiveWindow()](){ submitCreatePack(hwnd); });
}

// ============== RenamePack ==============
void openRenamePack(const std::string& pack_id, const std::wstring& orig_name) {
    dismissAllModals();
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
    if (name.empty()) { g_renamepack.error_msg = trW("renamepack.empty"); return; }
    if (name.size() > 24) { g_renamepack.error_msg = trW("renamepack.too_long"); return; }
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
    else g_renamepack.error_msg = trW("renamepack.failed");
}
// ============== UserProfile modal ==============
void openUserProfile(const std::wstring& uid_or_nickname) {
    // 连点同一头像去重:已在展示同一个 key 就忽略,不重启弹出动画(修连点闪烁重弹)。
    if (g_user_profile.open && g_user_profile.cur_key == uid_or_nickname) return;
    dismissAllModals();  // 互斥：先关其它浮层，避免与表情包卡片等叠加
    g_user_profile.open = true;
    g_user_profile.cur_key = uid_or_nickname;
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
        if (auto* bmp = app.images().fromFile(peer.avatar_path, (uint32_t)(ar * 2.0f + 0.5f))) {
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
        prim::drawText_(ctx, trW("common.loading"), sub,
                        cx, cy + 200, cw, 20,
                        br.solidA(pal.text_muted, t),
                        DWRITE_TEXT_ALIGNMENT_CENTER);
    } else if (!peer.err.empty()) {
        prim::drawText_(ctx, trW("profile.fetch_failed"), h1,
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
            std::wstring st_label = trW("status.offline");
            uint32_t st_color = 0xFF6B6A67;
            if (peer.status == L"online")  { st_label = trW("status.online"); st_color = 0xFF4ADE80; }
            else if (peer.status == L"busy"){ st_label = trW("status.busy"); st_color = 0xFFE34B4B; }
            else if (peer.status == L"away"){ st_label = trW("status.away"); st_color = 0xFFF5A524; }
            else if (peer.status == L"sleep"){st_label = trW("status.sleep"); st_color = 0xFF8B7BD9; }
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
                float target_chip_w = (std::min)((std::max)(measureW(app, tag, chip_fmt) + 20.0f, 64.0f),
                                                 max_chip_w);
                std::wstring tag_disp = fitText(app, tag, chip_fmt, target_chip_w - 20.0f);
                float tw = (std::min)((std::max)(measureW(app, tag_disp, chip_fmt) + 20.0f, 64.0f),
                                      max_chip_w);
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
            prim::drawText_(ctx, trW("profile.bio"), mt,
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
                 trW("common.close"), t, [](){ closeUserProfile(); });
}

// ============== EditStatusText modal ==============
void openEditStatusText() {
    dismissAllModals();
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
        toast::show(trW("toast.save_fail_refetch"));
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
    prim::drawText_(ctx, trW("status.edit_title"), h1, cx + 30, cy + 22, cw - 60, 22, br.solidA(pal.text, t));
    prim::drawText_(ctx, trW("status.edit_hint"), sub,
                    cx + 30, cy + 50, cw - 60, 18, br.solidA(pal.text_muted, t));
    drawField(app, g_edit_status.input, cx + 30, cy + 80, cw - 60, 40,
              trW("status.placeholder"), true, t);
    hit(g_edit_status.input.bounds, [](){}, true);
    float by = cy + ch - 52;
    std::wstring cancel = trW("common.cancel");
    std::wstring save = g_edit_status.busy ? trW("status.saving") : trW("common.save");
    drawGhostBtn(app, cx + 30, by, 120, 36, cancel.c_str(), t, [](){ closeEditStatusText(); });
    drawPrimaryBtn(app, cx + cw - 30 - 160, by, 160, 36,
                   save.c_str(), t,
                   [hwnd = GetActiveWindow()](){ submitEditStatusText(hwnd); });
}

// ============== EditBio modal ==============
void openEditBio() {
    dismissAllModals();
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
        toast::show(trW("toast.save_fail_refetch"));
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
    prim::drawText_(ctx, trW("bio.edit_hint"), sub,
                    cx + 30, cy + 50, cw - 60, 18, br.solidA(pal.text_muted, t));
    drawField(app, g_edit_bio.input, cx + 30, cy + 80, cw - 60, 140,
              trW("bio.placeholder"), true, t);
    hit(g_edit_bio.input.bounds, [](){}, true);
    float by = cy + ch - 52;
    std::wstring cancel = trW("common.cancel");
    std::wstring save = g_edit_bio.busy ? trW("status.saving") : trW("common.save");
    drawGhostBtn(app, cx + 30, by, 120, 36, cancel.c_str(), t, [](){ closeEditBio(); });
    drawPrimaryBtn(app, cx + cw - 30 - 160, by, 160, 36,
                   save.c_str(), t,
                   [hwnd = GetActiveWindow()](){ submitEditBio(hwnd); });
}

// ============== EditNickname modal ==============
void openEditNickname() {
    dismissAllModals();
    g_edit_nickname.open = true;
    g_edit_nickname.input.text = g_user.nickname;
    g_edit_nickname.input.cursor = (int)g_user.nickname.size();
    g_edit_nickname.input.clearSel();
    g_edit_nickname.busy = false;
    g_edit_nickname.t.start(0, 1, 0.22f, 0, curve::easeOutCubic);
}
static void closeEditNickname() {
    g_edit_nickname.t.start(g_edit_nickname.t.value(), 0, 0.18f, 0, curve::easeOutCubic);
    g_edit_nickname.open = false;
}
static void submitEditNickname(HWND hwnd) {
    if (g_edit_nickname.busy) return;
    std::wstring s = g_edit_nickname.input.text;
    // 后端按 Unicode scalar 计数上限 24；客户端 UTF-16 保守裁到 24 wchar，
    // astral 字符可能仍超标 → 后端权威，UI 处理 400。
    if (s.size() > 24) s = s.substr(0, 24);
    if (s.empty()) return;  // 后端拒绝空昵称，避免无意义请求
    g_edit_nickname.busy = true;
    g_user.nickname = s;    // 乐观更新，成功后由 myProfile 回收服务端 trim 结果
    fetch::changeNickname(hwnd, net::jsonEscape(s));
    closeEditNickname();
}
void onEditNicknameResult(unsigned int status) {
    if (!g_edit_nickname.busy) return;
    g_edit_nickname.busy = false;
    if (status >= 200 && status < 300) {
        // 204 成功：refetch 让 g_user.nickname 与服务端 trim 后的值一致。
        fetch::myProfile(GetActiveWindow());
        toast::show(trW("toast.saved"));
    } else if (status == 429) {
        // 冷却中：DB 未变，refetch 把乐观值回滚成真实昵称。
        fetch::myProfile(GetActiveWindow());
        toast::show(trW("toast.nickname_cooldown"));
    } else {
        // 400 长度非法 / 其他失败：回滚 + 提示。
        fetch::myProfile(GetActiveWindow());
        toast::show(trW("toast.save_fail_refetch"));
    }
}
void paintEditNicknameModal(D2DApp& app, float W, float H) {
    if (!g_edit_nickname.open && g_edit_nickname.t.value() < 0.001f) return;
    float t = g_edit_nickname.t.value();
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
    prim::drawText_(ctx, trW("profile.nickname"), h1, cx + 30, cy + 22, cw - 60, 22, br.solidA(pal.text, t));
    prim::drawText_(ctx, trW("nickname.edit_hint"), sub,
                    cx + 30, cy + 50, cw - 60, 18, br.solidA(pal.text_muted, t));
    drawField(app, g_edit_nickname.input, cx + 30, cy + 80, cw - 60, 40,
              trW("profile.nickname"), true, t);
    hit(g_edit_nickname.input.bounds, [](){}, true);
    float by = cy + ch - 52;
    std::wstring cancel = trW("common.cancel");
    std::wstring save = g_edit_nickname.busy ? trW("status.saving") : trW("common.save");
    drawGhostBtn(app, cx + 30, by, 120, 36, cancel.c_str(), t, [](){ closeEditNickname(); });
    drawPrimaryBtn(app, cx + cw - 30 - 160, by, 160, 36,
                   save.c_str(), t,
                   [hwnd = GetActiveWindow()](){ submitEditNickname(hwnd); });
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
    dismissAllModals();
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
    dismissAllModals();
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
    prim::drawText_(ctx, trW("web.title"), h1,
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
            prim::drawText_(ctx, trW("webview.no_runtime"),
                            sub,
                            cx, cy + ch * 0.5f, cw, 22,
                            br.solidA(pal.text_muted, t),
                            DWRITE_TEXT_ALIGNMENT_CENTER);
        } else {
            prim::drawText_(ctx, trW("common.loading"), sub,
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
    prim::drawText_(ctx, trW("renamepack.title"), h1, cx + 30, cy + 22, cw - 60, 22, br.solidA(pal.text, t));
    prim::drawText_(ctx, g_renamepack.orig_name, sub, cx + 30, cy + 50, cw - 60, 18,
                    br.solidA(pal.text_muted, t));
    drawField(app, g_renamepack.input, cx + 30, cy + 80, cw - 60, 40, trW("renamepack.placeholder"), true, t);
    hit(g_renamepack.input.bounds, [](){}, true);
    if (!g_renamepack.error_msg.empty()) {
        prim::drawText_(ctx, g_renamepack.error_msg, sub,
                        cx + 30, cy + 130, cw - 60, 18,
                        br.solidA(0xE34B4B, t));
    }
    float by = cy + ch - 52;
    drawGhostBtn(app, cx + 30, by, 120, 36, trW("common.cancel"), t, [](){ closeRenamePack(); });
    drawPrimaryBtn(app, cx + cw - 30 - 160, by, 160, 36,
                   g_renamepack.busy ? L"Saving..." : L"Save", t,
                   [hwnd = GetActiveWindow()](){ submitRenamePack(hwnd); });
}

// ============== Message context menu ==============
void openMsgContextMenu(POINT anchor_dip, int src_idx) {
    if (hasBlockingModalOpen()) return;
    auto& msgs = chat::streamFor(chat::g_active);
    if (src_idx < 0 || src_idx >= (int)msgs.size()) return;
    const chat::Msg& m = msgs[src_idx];
    closeUserMenu();
    g_msg_menu.open = true;
    g_msg_menu.anchor = anchor_dip;
    g_msg_menu.menu_rect = {};
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
    g_msg_menu.client_msg_id = m.client_msg_id;
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
    if (hasBlockingModalOpen()) return;
    closeMsgMenu();
    g_user_menu.open = true;
    g_user_menu.anchor = anchor_dip;
    g_user_menu.menu_rect = {};
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

void openChatMoreMenu(POINT anchor_dip) {
    if (hasBlockingModalOpen()) return;
    closeMsgMenu();
    closeUserMenu();
    g_chat_more.open = true;
    g_chat_more.anchor = anchor_dip;
    g_chat_more.menu_rect = {};
    g_chat_more.view = 0;
    g_chat_more.scroll = 0;
    g_chat_more.t.start(0, 1, 0.18f, 0, curve::easeOutCubic);
}
void closeChatMoreMenu() {
    g_chat_more.t.start(g_chat_more.t.value(), 0, 0.14f, 0, curve::easeOutCubic);
    g_chat_more.open = false;
}

void closeContextMenus() {
    if (g_msg_menu.open) closeMsgMenu();
    if (g_user_menu.open) closeUserMenu();
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
    dismissAllModals();
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
        g_mute_user.error_msg = trW("mute.channel_not_ready");
        return;
    }
    if (g_mute_user.reason.text.empty()) {
        g_mute_user.error_msg = trW("mute.reason_required");
        return;
    }
    g_mute_user.busy = true;
    fetch::muteUser(hwnd, g_mute_user.chat_id, g_mute_user.target_user_id,
                    muteDurationSeconds(), g_mute_user.reason.text);
}

void onMuteUserResult(bool success) {
    g_mute_user.busy = false;
    if (success) {
        toast::show(trW("mute.submitted"));
        closeMuteUser();
    } else {
        std::lock_guard<std::mutex> lk(fetch::g_moderation_mtx);
        g_mute_user.error_msg = fetch::g_moderation_member.error.empty()
            ? trW("mute.failed") : fetch::g_moderation_member.error;
    }
}

void onUnmuteUserResult(bool success) {
    if (success) toast::show(trW("mute.unmuted"));
    else {
        std::lock_guard<std::mutex> lk(fetch::g_moderation_mtx);
        toast::show(fetch::g_moderation_member.error.empty()
            ? trW("mute.unmute_failed") : fetch::g_moderation_member.error);
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
    if (hasBlockingModalOpen()) return;
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
        chat::beginReplyToMessage(g_msg_menu.slug, g_msg_menu.server_id,
                                  g_msg_menu.client_msg_id, g_msg_menu.author,
                                  g_msg_menu.profile_key, body);
        if (g_msg_menu.from != L"me" && !g_msg_menu.profile_key.empty()) {
            chat::addMentionToComposer(g_msg_menu.profile_key, g_msg_menu.author);
        }
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
    if (g_msg_menu.server_id > 0) {
        items.push_back({ trW("msg.react"), [](){
            // 复用 emoji picker：置 react target，选中的 glyph 走 reactToMessage 而非 composer。
            chat::beginReactPick(g_msg_menu.slug, g_msg_menu.server_id);
            closeMsgMenu();
        }, false });
    }
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
                    if (sticker::isMyStickersPack(p)) {
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
    g_msg_menu.menu_rect = { mx, my, mw, mh };

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
    if (hasBlockingModalOpen()) return;
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
    items.push_back({ trW("usermenu.view_profile"), [](){
        std::wstring key = g_user_menu.profile_key;
        closeUserMenu();
        openUserProfile(key);
    }, false, false });
    items.push_back({ trW("usermenu.mention"), [](){
        chat::addMentionToComposer(g_user_menu.profile_key, g_user_menu.label);
        closeUserMenu();
    }, false, false });
    bool self = isSelfProfileKey(g_user_menu.profile_key);
    if (g_user.is_admin && !self) {
        if (!mod.loaded) {
            items.push_back({ trW("usermenu.loading_mod"), [](){}, false, true });
        } else if (!mod.ok) {
            items.push_back({ mod.error.empty() ? trW("usermenu.mod_load_failed") : mod.error, [](){}, false, true });
        } else if (mod.active) {
            items.push_back({ trW("usermenu.unmute"), [](){
                std::string chat_id = chat::activeChatId();
                if (!chat_id.empty()) {
                    fetch::unmuteUser(GetActiveWindow(), chat_id,
                                      g_user_menu.profile_key, L"Manual unmute");
                }
                closeUserMenu();
            }, false, !mod.can_unmute });
        } else {
            items.push_back({ trW("usermenu.mute"), [](){
                std::wstring key = g_user_menu.profile_key;
                std::wstring label = g_user_menu.label;
                closeUserMenu();
                openMuteUser(key, label);
            }, true, !mod.can_mute });
        }
    }

    auto* menu_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(10.0f));
    // 菜单宽度按最宽项实测（"Loading moderation status..." / 日文长状态会超出 190 固定宽）。
    float mw = 190.0f;
    for (const auto& it : items) {
        float need = measureW(app, it.label, menu_fmt) + 32.0f;  // 左右各 ~16 padding
        if (need > mw) mw = need;
    }
    if (mw > 360.0f) mw = 360.0f;  // 上限，超长状态文末尾省略
    float row = 32.0f;
    float mh = 12.0f + row * (float)items.size() + 12.0f;
    float mx = (float)g_user_menu.anchor.x;
    float my = (float)g_user_menu.anchor.y;
    if (mx + mw > W - 8) mx = W - mw - 8;
    if (my + mh > H - 8) my = H - mh - 8;
    if (mx < 8) mx = 8;
    if (my < 8) my = 8;
    g_user_menu.menu_rect = { mx, my, mw, mh };

    hit({ 0, 0, W, H }, [](){ closeUserMenu(); }, false);

    prim::fillRR(ctx, mx, my, mw, mh, 8, br.solidA(pal.card, t));
    prim::strokeRR(ctx, mx, my, mw, mh, 8, br.solidA(pal.divider, t), 1.0f);
    for (size_t i = 0; i < items.size(); ++i) {
        float y = my + 8 + row * (float)i;
        LayoutRect r{ mx + 6, y, mw - 12, row };
        bool hov = !items[i].disabled && r.contains(g_mouse);
        if (hov) prim::fillRR(ctx, r.x, r.y, r.w, r.h, 6, br.solidA(pal.primary, 0.12f * t));
        uint32_t color = items[i].disabled ? pal.text_muted : (items[i].danger ? 0xFFE8795C : pal.text);
        std::wstring lbl = fitText(app, items[i].label, menu_fmt, r.w - 20);
        prim::drawTextNoWrap(ctx, lbl, menu_fmt, r.x + 10, r.y + 8, r.w - 20, 16,
                        br.solidA(color, items[i].disabled ? 0.55f * t : t));
        if (!items[i].disabled) hit(r, items[i].click, true);
    }
}

namespace {
// 成员子列表最多可见行数（含 Back 行之外的成员行）；超出用 g_chat_more.scroll 翻。
constexpr int kChatMoreMembersVisible = 12;

// 复刻 chat::authorKeyFor 的键逻辑（chat_internal.h 仅供 chat_*.cpp 用，这里内联）。
std::wstring chatMoreAuthorKey(const chat::Msg& m) {
    if (!m.author_key.empty()) return m.author_key;
    if (!m.peer_key.empty()) return m.peer_key;
    if (m.from == L"me") return L"me";
    return m.from;
}
// 复刻 chat::displayAuthorFor 的展示逻辑（昵称/用户名 via peerProfileCached）。
std::wstring chatMoreAuthorLabel(const chat::Msg& m) {
    if (m.from == L"me") {
        if (!m.author.empty()) return m.author;
        if (!g_user.nickname.empty()) return g_user.nickname;
        if (!g_user.username.empty()) return g_user.username;
        return L"me";
    }
    std::wstring key = !m.author_key.empty() ? m.author_key
                     : (!m.peer_key.empty() ? m.peer_key : m.from);
    if (!key.empty()) {
        fetch::PeerProfile peer = fetch::peerProfileCached(key);
        if (peer.loaded && peer.err.empty()) {
            if (!peer.nickname.empty()) return peer.nickname;
            if (!peer.username.empty()) return peer.username;
            if (!peer.uid.empty())      return peer.uid;
        }
    }
    if (!m.author.empty()) return m.author;
    if (!m.from.empty())   return m.from;
    return key;
}
}  // namespace

void paintChatMoreMenu(D2DApp& app, float W, float H) {
    if (hasBlockingModalOpen()) return;
    if (!g_chat_more.open && g_chat_more.t.value() < 0.001f) return;
    float t = g_chat_more.t.value();
    if (t < 0.001f) return;
    const Palette& pal = palette();
    auto* ctx = app.ctx();
    auto& br = app.brushes();

    struct Item { std::wstring label; std::function<void()> click; bool danger; bool disabled; };
    std::vector<Item> items;

    if (g_chat_more.view == 0) {
        // 主菜单：搜索 / 成员 / 公告
        items.push_back({ trW("chatmenu.search"), [](){
            closeChatMoreMenu();
            openSearch();
        }, false, false });
        items.push_back({ trW("chatmenu.members"), [](){
            // 就地切到成员子列表；弹层保持打开、下一帧重新测量。
            g_chat_more.view = 1;
            g_chat_more.scroll = 0;
        }, false, false });
        items.push_back({ trW("chatmenu.announcements"), [](){
            closeChatMoreMenu();
            chat::switchChannel(L"announcements");
        }, false, false });
    } else {
        // 成员子列表：Back 行 + 当前流里出现过的唯一发言者（客户端来源，真实数据）。
        items.push_back({ L"‹ " + trW("chatmenu.back"), [](){
            g_chat_more.view = 0;
            g_chat_more.scroll = 0;
        }, false, false });

        std::vector<std::pair<std::wstring, std::wstring>> roster;  // key -> label，首次出现顺序
        std::set<std::wstring> seen;
        for (const auto& m : chat::streamFor(chat::g_active)) {
            std::wstring key = chatMoreAuthorKey(m);
            if (key.empty()) continue;
            if (!seen.insert(key).second) continue;
            roster.emplace_back(key, chatMoreAuthorLabel(m));
        }

        if (roster.empty()) {
            items.push_back({ trW("chatmenu.members_empty"), [](){}, false, true });
        } else {
            int total = (int)roster.size();
            int off = g_chat_more.scroll;
            if (off > total - kChatMoreMembersVisible) off = total - kChatMoreMembersVisible;
            if (off < 0) off = 0;
            g_chat_more.scroll = off;
            int end = (std::min)(total, off + kChatMoreMembersVisible);
            for (int i = off; i < end; ++i) {
                std::wstring key = roster[i].first;
                std::wstring lbl = roster[i].second;
                items.push_back({ lbl, [key, lbl](){
                    closeChatMoreMenu();
                    openUserContextMenu(g_chat_more.anchor, key, lbl);
                }, false, false });
            }
        }
    }

    auto* menu_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(10.0f));
    float mw = 190.0f;
    for (const auto& it : items) {
        float need = measureW(app, it.label, menu_fmt) + 32.0f;
        if (need > mw) mw = need;
    }
    if (mw > 360.0f) mw = 360.0f;
    float row = 32.0f;
    float mh = 12.0f + row * (float)items.size() + 12.0f;
    float mx = (float)g_chat_more.anchor.x;
    float my = (float)g_chat_more.anchor.y;
    if (mx + mw > W - 8) mx = W - mw - 8;
    if (my + mh > H - 8) my = H - mh - 8;
    if (mx < 8) mx = 8;
    if (my < 8) my = 8;
    g_chat_more.menu_rect = { mx, my, mw, mh };

    hit({ 0, 0, W, H }, [](){ closeChatMoreMenu(); }, false);

    prim::fillRR(ctx, mx, my, mw, mh, 8, br.solidA(pal.card, t));
    prim::strokeRR(ctx, mx, my, mw, mh, 8, br.solidA(pal.divider, t), 1.0f);
    for (size_t i = 0; i < items.size(); ++i) {
        float y = my + 8 + row * (float)i;
        LayoutRect r{ mx + 6, y, mw - 12, row };
        bool hov = !items[i].disabled && r.contains(g_mouse);
        if (hov) prim::fillRR(ctx, r.x, r.y, r.w, r.h, 6, br.solidA(pal.primary, 0.12f * t));
        uint32_t color = items[i].disabled ? pal.text_muted : (items[i].danger ? 0xFFE8795C : pal.text);
        std::wstring lbl = fitText(app, items[i].label, menu_fmt, r.w - 20);
        prim::drawTextNoWrap(ctx, lbl, menu_fmt, r.x + 10, r.y + 8, r.w - 20, 16,
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
    prim::drawText_(ctx, trW("mute.title"), h1, cx + 28, cy + 22, cw - 56, 24, br.solidA(pal.text, t));
    std::wstring line = trW("mute.target");
    {
        auto p = line.find(L"{name}");
        if (p != std::wstring::npos) line.replace(p, 6, g_mute_user.target_label);
    }
    prim::drawText_(ctx, line, sub, cx + 28, cy + 52, cw - 56, 18, br.solidA(pal.text_muted, t));

    drawField(app, g_mute_user.duration, cx + 28, cy + 86, 120, 38, L"30", true, t);
    hit(g_mute_user.duration.bounds, [](){ g_mute_user.focus = 0; }, true);
    const std::wstring units[] = { trW("mute.unit_sec"), trW("mute.unit_min"), trW("mute.unit_hour"), trW("mute.unit_day") };
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
    drawField(app, g_mute_user.reason, cx + 28, cy + 146, cw - 56, 108, trW("mute.reason_placeholder"), true, t);
    hit(g_mute_user.reason.bounds, [](){ g_mute_user.focus = 1; }, true);

    if (!g_mute_user.error_msg.empty()) {
        prim::drawText_(ctx, g_mute_user.error_msg, sub, cx + 28, cy + 262, cw - 56, 18,
                        br.solidA(0xFFE8795C, t));
    }
    float by = cy + ch - 54;
    drawGhostBtn(app, cx + 28, by, 120, 36, trW("common.cancel"), t, [](){ closeMuteUser(); });
    drawPrimaryBtn(app, cx + cw - 188, by, 160, 36,
                   g_mute_user.busy ? trW("mute.submitting") : trW("mute.confirm"), t,
                   [hwnd = GetActiveWindow()](){ submitMuteUser(hwnd); });
}

// ============== PackPreview modal ==============
void openPackPreviewModal(const std::string& short_name) {
    dismissAllModals();  // 互斥：先关资料卡等浮层，避免与本卡片叠加
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
        prim::drawText_(ctx, trW("common.loading"), sub,
                        cx, cy + ch * 0.5f - 12, cw, 24,
                        br.solidA(pal.text_muted, t),
                        DWRITE_TEXT_ALIGNMENT_CENTER);
    } else if (!pv.err.empty()) {
        prim::drawText_(ctx, trW("pack.cant_load"), h1,
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
        prim::drawText_(ctx, pv.name.empty() ? trW("pack.share_card_title") : pv.name, h1,
                        cx + 30, cy + 22, cw - 60, 30,
                        br.solidA(pal.text, t));
        std::wstring meta;
        if (!pv.creator_name.empty()) meta = trW("pack.by_prefix") + pv.creator_name;
        else meta = trW("pack.share_card_title");
        std::wstring installed = trW("pack.installed_count");
        {
            auto p = installed.find(L"{n}");
            if (p != std::wstring::npos)
                installed.replace(p, 3, std::to_wstring(pv.install_count));
        }
        std::wstring info = meta + L" - " + installed;
        prim::drawText_(ctx, info, mt,
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
                ID2D1Bitmap* bmp = app.images().fromFile(pv.sticker_paths[i], 72);
                if (bmp) {
                    prim::pushLayerRR(ctx, app.factory(), ex, ey, cell, cell, 8.0f);
                    ctx->DrawBitmap(bmp,
                        D2D1::RectF(ex, ey, ex + cell, ey + cell),
                        t, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                    prim::popLayer(ctx);
                }
            }
            if ((int)pv.sticker_paths.size() > max_show) {
                std::wstring mw = trW("pack.more_count");
                {
                    auto p = mw.find(L"{n}");
                    if (p != std::wstring::npos)
                        mw.replace(p, 3, std::to_wstring((int)pv.sticker_paths.size() - max_show));
                }
                prim::drawText_(ctx, mw, mt,
                                cx + 30, cy + 88 + 3 * (cell + 6) + 4, cw - 60, 18,
                                br.solidA(pal.text_muted, t),
                                DWRITE_TEXT_ALIGNMENT_CENTER);
            }
        }

        float by = cy + ch - 56;
        drawGhostBtn(app, cx + 30, by, 130, 38, trW("common.cancel"), t,
                     [](){ closePackPreview(); });
        if (pv.already_installed) {
            drawGhostBtn(app, cx + cw - 30 - 200, by, 200, 38,
                         trW("pack.installed"), t, [](){
                            closePackPreview();
                            toast::show(trW("pack.already_installed"));
                         });
        } else {
            std::string sn = pv.short_name;
            drawPrimaryBtn(app, cx + cw - 30 - 200, by, 200, 38,
                           trW("pack.add_group"), t, [sn](){
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
    dismissAllModals();
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

    prim::drawText_(ctx, trW("search.title"), h1,
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

// 关闭当前最上层的「带背景遮罩」模态(paintDim 遮罩 hit 的回调 = 点模态外返回)。
// 菜单类(msg/user/chat_more/webview)不走 paintDim,由 onMouseLDown 各自处理,不在此列。
void dismissTopModal() {
    if (g_search.open) closeSearch();
    else if (g_mute_user.open) closeMuteUser();
    else if (g_pack_preview_modal.open) closePackPreview();
    else if (g_edit_bio.open) closeEditBio();
    else if (g_edit_nickname.open) closeEditNickname();
    else if (g_edit_status.open) closeEditStatusText();
    else if (g_user_profile.open) closeUserProfile();
    else if (g_renamepack.open) closeRenamePack();
    else if (g_createpack.open) closeCreatePack();
    else if (g_addtag.open) closeAddTag();
    else if (g_change_pw.open) closeChangePw();
    else if (g_confirm.open) closeConfirm();
    else if (g_cs2.open) closeCS2();
    else if (g_market_detail_modal.open) closeMarketDetail();
    else if (g_history.open) closeHistory();
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
        else if (g_chat_more.open) closeChatMoreMenu();
        else if (g_mute_user.open) closeMuteUser();
        else if (g_pack_preview_modal.open) closePackPreview();
        else if (g_edit_bio.open) closeEditBio();
        else if (g_edit_nickname.open) closeEditNickname();
        else if (g_edit_status.open) closeEditStatusText();
        else if (g_user_profile.open) closeUserProfile();
        else if (g_renamepack.open) closeRenamePack();
        else if (g_createpack.open) closeCreatePack();
        else if (g_addtag.open) closeAddTag();
        else if (g_change_pw.open) closeChangePw();
        else if (g_confirm.open) closeConfirm();
        else if (g_cs2.open) closeCS2();
        else if (g_market_detail_modal.open) closeMarketDetail();
        else if (g_history.open) closeHistory();
    }
    return true;
}

bool onMouseRDown(HWND /*hwnd*/, POINT dip) {
    if (g_msg_menu.open) {
        bool inside = g_msg_menu.menu_rect.contains(dip);
        if (!inside) closeMsgMenu();
        return inside;
    }
    if (g_user_menu.open) {
        bool inside = g_user_menu.menu_rect.contains(dip);
        if (!inside) closeUserMenu();
        return inside;
    }
    if (g_chat_more.open) {
        bool inside = g_chat_more.menu_rect.contains(dip);
        if (!inside) closeChatMoreMenu();
        return inside;
    }
    return hasBlockingModalOpen();
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
    if (g_edit_nickname.open) { g_edit_nickname.input.onChar(c, ctrl, hwnd); return true; }
    if (g_edit_status.open) { g_edit_status.input.onChar(c, ctrl, hwnd); return true; }
    if (g_market_detail_modal.open) {
        if (!g_session_token.empty())
            g_market_detail_modal.review_input.onChar(c, ctrl, hwnd);
        return true;
    }
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
        if (g_chat_more.open) { closeChatMoreMenu(); return true; }
        if (g_mute_user.open) { closeMuteUser(); return true; }
        if (g_msg_menu.open) { closeMsgMenu(); return true; }
        if (g_pack_preview_modal.open) { closePackPreview(); return true; }
        if (g_webview_modal.open) { closeWebViewModal(); return true; }
        if (g_edit_bio.open) { closeEditBio(); return true; }
        if (g_edit_nickname.open) { closeEditNickname(); return true; }
        if (g_edit_status.open) { closeEditStatusText(); return true; }
        if (g_user_profile.open) { closeUserProfile(); return true; }
        if (g_renamepack.open) { closeRenamePack(); return true; }
        if (g_createpack.open) { closeCreatePack(); return true; }
        if (g_addtag.open) { closeAddTag(); return true; }
        if (g_change_pw.open) { closeChangePw(); return true; }
        if (g_confirm.open) { closeConfirm(); return true; }
        if (g_cs2.open) { closeCS2(); return true; }
        if (g_market_detail_modal.open) { closeMarketDetail(); return true; }
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
    if (g_edit_nickname.open) {
        if (vk == VK_RETURN) { submitEditNickname(hwnd); return true; }
        g_edit_nickname.input.onKey(vk, shift, ctrl);
        return true;
    }
    if (g_market_detail_modal.open) {
        if (!g_session_token.empty()) g_market_detail_modal.review_input.onKey(vk, shift, ctrl);
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

// 关掉当前所有打开的 modal/卡片（含上下文菜单），供 openX() 在打开新窗口前调用，
// 实现“同一时刻只有一个浮层”。逐个复用各自 close 函数以保留收起动画/副作用
// （如 CS2 关 WebView、用户卡清状态）。
void dismissAllModals() {
    if (g_change_pw.open)          closeChangePw();
    if (g_confirm.open)            closeConfirm();
    if (g_cs2.open)                closeCS2();
    if (g_market_detail_modal.open) closeMarketDetail();
    if (g_history.open)            closeHistory();
    if (g_addtag.open)             closeAddTag();
    if (g_createpack.open)         closeCreatePack();
    if (g_renamepack.open)         closeRenamePack();
    if (g_user_profile.open)       closeUserProfile();
    if (g_edit_status.open)        closeEditStatusText();
    if (g_edit_bio.open)           closeEditBio();
    if (g_edit_nickname.open)      closeEditNickname();
    if (g_webview_modal.open)      closeWebViewModal();
    if (g_mute_user.open)          closeMuteUser();
    if (g_pack_preview_modal.open) closePackPreview();
    if (g_search.open)             closeSearch();
    closeContextMenus();  // msg menu + user menu
}

}  // namespace launcher::d2d::modal
