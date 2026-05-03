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
            prim::drawLine(ctx, x + 12 + pre_w, y + 8,
                           x + 12 + pre_w, y + h - 8,
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
}

bool anyOpen() {
    return g_change_pw.open || g_confirm.open || g_cs2.open || g_history.open
        || g_addtag.open || g_createpack.open || g_renamepack.open
        || g_user_profile.open || g_edit_status.open || g_edit_bio.open;
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

    float cw = 600, ch = 380;
    float cx = (W - cw) * 0.5f, cy = (H - ch) * 0.5f;
    prim::drawShadow(ctx, br, cx, cy, cw, ch, 16.0f, pal.shadow_card_hover, t, 6.0f, 5);
    prim::fillRR(ctx, cx, cy, cw, ch, 16.0f, br.solidA(pal.card, t));

    // 顶部 cover 区域 — 默认显示真 CS2 头图；WebView2 ready 后用 Steam 嵌入视频替代
    float cover_h = 220;
    bool wv_ready = webview::isReady();
    if (wv_ready && t > 0.95f) {
        // WebView2 子窗口接管 cover 区域（物理像素 = DIP × dpi/96）
        float scale = app.dpi() / 96.0f;
        int wl = (int)((cx + 8) * scale);
        int wt = (int)((cy + 8) * scale);
        int wr = (int)((cx + cw - 8) * scale);
        int wb = (int)((cy + cover_h) * scale);
        webview::setBounds(wl, wt, wr, wb);
        webview::show(true);
        // D2D 这里只画一个圆角占位，WebView2 会盖在上面
        prim::fillRR(ctx, cx, cy, cw, cover_h, 16.0f, br.solidA(pal.surface, t));
    } else {
        // WebView2 没就绪时画静态 CS2 头图
        webview::show(false);
        auto cs2_path = cs2HeaderPath();
        auto* cover_bmp = cs2_path.empty() ? nullptr : app.images().fromFile(cs2_path);
        if (cover_bmp) {
            ctx->PushAxisAlignedClip(D2D1::RectF(cx, cy, cx + cw, cy + cover_h),
                                     D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            D2D1_SIZE_F sz = cover_bmp->GetSize();
            float scale_ = (std::max)(cw / sz.width, cover_h / sz.height);
            float dw = sz.width * scale_, dh = sz.height * scale_;
            float dx = cx + (cw - dw) * 0.5f, dy = cy + (cover_h - dh) * 0.5f;
            ctx->DrawBitmap(cover_bmp, D2D1::RectF(dx, dy, dx + dw, dy + dh),
                            t, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            prim::fillRect(ctx, cx, cy + cover_h - 80, cw, 80,
                           br.solidA(0x000000, 0.55f * t));
            ctx->PopAxisAlignedClip();
        } else {
            prim::fillRR(ctx, cx, cy, cw, cover_h, 16.0f, br.solidA(0xC96442, t));
        }
        auto* hh = app.texts().format(L"Microsoft YaHei UI", ptToDip(22.0f),
                                      DWRITE_FONT_WEIGHT_BOLD);
        prim::drawText_(ctx, L"Counter-Strike 2", hh,
                        cx + 24, cy + cover_h - 60, cw - 48, 30,
                        br.solidA(0xFFFFFF, t));
        auto* sub_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(10.0f));
        prim::drawText_(ctx, L"Valve  ·  Source 2  ·  视频加载中…", sub_fmt,
                        cx + 24, cy + cover_h - 28, cw - 48, 18,
                        br.solidA(0xFFFFFF, t * 0.85f));
    }

    // 底部 stat
    auto* stat_lbl = app.texts().format(L"Microsoft YaHei UI", ptToDip(8.0f));
    auto* stat_val = app.texts().format(L"Microsoft YaHei UI", ptToDip(11.0f),
                                        DWRITE_FONT_WEIGHT_BOLD);
    struct S { const wchar_t* l; const wchar_t* v; };
    S stats[] = {
        { L"账号", L"未登录 Steam" },
        { L"最近玩", L"--" },
        { L"总时长", L"--" },
    };
    float sx = cx + 24, sy = cy + cover_h + 24;
    for (auto& s : stats) {
        prim::drawText_(ctx, s.l, stat_lbl,
                        sx, sy, 160, 14, br.solidA(pal.text_muted, t));
        prim::drawText_(ctx, s.v, stat_val,
                        sx, sy + 18, 160, 22, br.solidA(pal.text, t));
        sx += 180;
    }

    drawPrimaryBtn(app, cx + 24, cy + ch - 56, 200, 40,
                   L"启动 CS2", t, [](){ launchCS2(); });
    drawGhostBtn(app, cx + 240, cy + ch - 56, 160, 40,
                 L"商店页面", t, [](){ openCS2Store(); });

    // 关闭 ✕
    LayoutRect close_btn{ cx + cw - 40, cy + 12, 28, 28 };
    bool close_hov = close_btn.contains(g_mouse);
    if (close_hov) {
        prim::fillRR(ctx, close_btn.x, close_btn.y, 28, 28, 6.0f,
                     br.solidA(0x000000, t * 0.20f));
    }
    icons::drawIcon(app, icons::Name::X, close_btn.x + 6, close_btn.y + 6, 16,
                    fadeArgb(0xFFFFFFFF, t));
    hit(close_btn, [](){ closeCS2(); }, true);
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
        // nickname + status dot
        prim::drawText_(ctx, peer.nickname.empty() ? peer.uid : peer.nickname, h1,
                        cx, cy + 150, cw, 26,
                        br.solidA(pal.text, t),
                        DWRITE_TEXT_ALIGNMENT_CENTER);
        // uid + username
        wchar_t handle[128];
        swprintf_s(handle, L"@%.32ls  ·  UID %.16ls",
                   peer.username.c_str(), peer.uid.c_str());
        prim::drawText_(ctx, handle, mt,
                        cx, cy + 178, cw, 18,
                        br.solidA(pal.text_muted, t),
                        DWRITE_TEXT_ALIGNMENT_CENTER);

        // status text
        if (!peer.status_text.empty()) {
            prim::drawText_(ctx, peer.status_text, sub,
                            cx + 24, cy + 210, cw - 48, 40,
                            br.solidA(pal.text_muted, t),
                            DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        // bio
        if (!peer.bio.empty()) {
            prim::drawText_(ctx, L"个人签名", mt,
                            cx + 24, cy + 270, cw - 48, 16,
                            br.solidA(pal.text_muted, t));
            prim::fillRR(ctx, cx + 24, cy + 290, cw - 48, 80, 8.0f,
                         br.solidA(pal.surface, t));
            prim::drawText_(ctx, peer.bio, sub,
                            cx + 36, cy + 300, cw - 72, 64,
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

// ============== 事件路由 ==============
bool onMouseLDown(HWND /*hwnd*/, POINT dip) {
    if (!anyOpen()) return false;
    bool consumed = dispatchClick(dip);
    if (!consumed) {
        if (g_edit_bio.open) closeEditBio();
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
