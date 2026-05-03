// Modals 实现 — 见 modals.h；简化版，完整版见 GDI+ tools/preview/modals.inl。

#include "modals.h"
#include "icons.h"
#include "palette.h"
#include "user_state.h"
#include "net.h"
#include "hit.h"
#include "stages.h"
#include "render/primitives.h"

#include <algorithm>
#include <array>
#include <memory>
#include <mutex>
#include <cstdio>

namespace launcher::d2d::modal {

ChangePwState g_change_pw;
ConfirmState  g_confirm;
CS2State      g_cs2;
HistoryState  g_history;

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
}

bool anyOpen() {
    return g_change_pw.open || g_confirm.open || g_cs2.open || g_history.open;
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
}
static void closeCS2() {
    g_cs2.t.start(g_cs2.t.value(), 0, 0.20f, 0, curve::easeOutQuint);
    g_cs2.open = false;
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

    // 顶部 cover (主色渐变占位)
    float cover_h = 200;
    prim::fillRR(ctx, cx, cy, cw, cover_h, 16.0f, br.solidA(0xC96442, t));
    auto* hh = app.texts().format(L"Microsoft YaHei UI", ptToDip(22.0f),
                                  DWRITE_FONT_WEIGHT_BOLD);
    prim::drawText_(ctx, L"Counter-Strike 2", hh,
                    cx + 24, cy + 24, cw - 48, 36,
                    br.solidA(0xFFFFFF, t));
    auto* sub = app.texts().format(L"Microsoft YaHei UI", ptToDip(10.0f));
    prim::drawText_(ctx, L"Valve  ·  Source 2", sub,
                    cx + 24, cy + 60, cw - 48, 18,
                    br.solidA(0xFFFFFF, t * 0.85f));

    // 中央 ▶
    float pcx = cx + cw * 0.5f, pcy = cy + cover_h * 0.5f + 20;
    prim::fillCircle(ctx, pcx, pcy, 32, br.solidA(0xFFFFFF, t * 0.92f));
    icons::drawIcon(app, icons::Name::Play, pcx - 14, pcy - 14, 28, 0xFF1F1E1D);
    hit(LayoutRect{ pcx - 32, pcy - 32, 64, 64 }, [](){
        // 真启动 Steam 留 polish
    }, true);

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

    // 启动按钮
    drawPrimaryBtn(app, cx + 24, cy + ch - 56, 200, 40,
                   L"启动 CS2", t, [](){ /* 启 Steam 留 polish */ });
    drawGhostBtn(app, cx + 240, cy + ch - 56, 160, 40,
                 L"商店页面", t, [](){ /* ShellExecute steam:// */ });

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
    prim::drawText_(ctx, L"接 GET /api/profile/login-history (留下一轮接通)", sub,
                    cx + 24, cy + 50, cw - 48, 18,
                    br.solidA(pal.text_muted, t));

    // 占位行 5 个
    auto* row_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(9.5f));
    for (int i = 0; i < 5; ++i) {
        float ry = cy + 90 + i * 50;
        prim::fillRR(ctx, cx + 20, ry, cw - 40, 40, 8.0f,
                     br.solidA(pal.surface, t));
        prim::fillCircle(ctx, cx + 36, ry + 20, 6,
                         br.solidA(0x4ADE80, t));
        wchar_t buf[64];
        swprintf_s(buf, L"2026-05-0%d  ·  154.40.36.x  ·  Tokyo", i + 1);
        prim::drawText_(ctx, buf, row_fmt,
                        cx + 56, ry + 11, cw - 96, 18,
                        br.solidA(pal.text, t));
    }

    drawGhostBtn(app, cx + cw - 24 - 100, cy + ch - 52, 100, 36,
                 L"关闭", t, [](){ closeHistory(); });
}

// ============== 事件路由 ==============
bool onMouseLDown(HWND /*hwnd*/, POINT dip) {
    // 任意 modal 开着时由 modal 优先 hit；外部点击关闭
    if (!anyOpen()) return false;
    // hits 已经在 paint 时注册（含关闭按钮 / yes/no 按钮 / inputbox）
    bool consumed = dispatchClick(dip);
    if (!consumed) {
        // 点击外部 — 关闭最顶 modal
        if (g_change_pw.open) closeChangePw();
        else if (g_confirm.open) closeConfirm();
        else if (g_cs2.open) closeCS2();
        else if (g_history.open) closeHistory();
    }
    return true;
}

bool onChar(HWND hwnd, wchar_t c, bool ctrl) {
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
        if (g_change_pw.open) { closeChangePw(); return true; }
        if (g_confirm.open) { closeConfirm(); return true; }
        if (g_cs2.open) { closeCS2(); return true; }
        if (g_history.open) { closeHistory(); return true; }
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
