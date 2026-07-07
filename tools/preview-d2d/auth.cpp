// Auth view — 详见 auth.h；GDI+ 等价 tools/preview/loading_demo.cpp:2477。
//
// 1:1 翻译要点：
//   * MeasureString → IDWriteTextLayout::GetMetrics（TextCache::measure）
//   * StringAlignmentCenter → DWRITE_TEXT_ALIGNMENT_CENTER
//   * FontStyleBold → DWRITE_FONT_WEIGHT_BOLD
//   * GraphicsPath + SetClip → ID2D1DeviceContext::PushAxisAlignedClip
//   * FillEllipse / DrawLine / Pen 圆头圆 join → strokes_.round()

#include "auth.h"
#include "stages.h"
#include "palette.h"
#include "net.h"
#include "user_state.h"
#include "hwid.h"
#include "persist.h"
#include "i18n.h"
#include "chat.h"
#include "render/primitives.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <mutex>

namespace launcher::d2d {
std::vector<HitArea> g_hits;
POINT g_mouse{ -1, -1 };
bool  g_mouse_pressed{ false };
size_t g_modal_hit_floor{ 0 };
}

namespace launcher::d2d::auth {

Form g_form;

// UI 文案走 i18n（tr/trW），随系统/用户语言切换
namespace {

// 测文字 DIP 宽度
float measureW(D2DApp& app, std::wstring_view s, IDWriteTextFormat* fmt) {
    if (s.empty()) return 0.0f;
    DWRITE_TEXT_METRICS m{};
    if (!app.texts().measure(fmt, s, 8192.0f, 256.0f, &m)) return 0.0f;
    return m.width;
}
}  // anon

// drawField — floating label + halo + caret + 选区高亮
//   ix/iy/iw/ih: DIP 几何
//   labelStr: 浮动 label 文本
//   idx: 0=username 1=password 2=invite
//   op: 父卡片整体 opacity (淡入淡出共用)
static void drawField(D2DApp& app, InputBox& box,
                      float ix, float iy, float iw, float ih,
                      const wchar_t* labelStr, int idx, float op) {
    box.bounds = { ix, iy, iw, ih };
    bool focused = (g_form.focus == idx);
    bool filled  = !box.text.empty();
    bool floating = focused || filled;

    // floating label tween 状态切换时启动
    float target = floating ? 1.0f : 0.0f;
    if (!box.float_t.started) {
        box.float_t.start(target, target, 0.001f, 0, curve::easeOutCubic);
    } else if (std::abs(box.float_t.to - target) > 0.001f) {
        box.float_t.start(box.float_t.value(), target, 0.22f, 0, curve::easeOutCubic);
    }
    float anim_v = box.float_t.value();

    auto* ctx = app.ctx();
    auto& br = app.brushes();
    const Palette& pal = palette();

    // bg + border
    prim::fillRR(ctx, ix, iy, iw, ih, 10.0f, br.solidA(pal.bg, op));
    auto* border = focused
        ? br.solidA(pal.primary, op)
        : br.solidA(pal.divider, op);
    prim::strokeRR(ctx, ix, iy, iw, ih, 10.0f, border, focused ? 1.5f : 1.0f);

    // halo — focus 时主色发光
    if (focused) {
        prim::strokeRR(ctx, ix - 2, iy - 2, iw + 4, ih + 4, 12.0f,
                       br.solidA(pal.primary, op * 0.15f), 4.0f);
    }

    // floating label：filled 时 size 9 顶部，empty 时 size 12 中央
    // 之前 size 11 + body 10.5 在 iy+22 → 只 1px 间距，filled 时 label 渲染框顶到 body 顶
    // 新方案：label 顶部 iy+4，filled 时只占 iy+4..iy+19；body iy+22+ → 3px 留白
    float lab_size = 12.0f - 3.0f * anim_v;
    float lab_y = iy + 4.0f + (ih * 0.5f - 10.0f) * (1.0f - anim_v);
    auto interp = [&](uint32_t a, uint32_t b) -> uint32_t {
        auto byte = [](uint32_t c, int sh) { return (c >> sh) & 0xFFu; };
        uint32_t r = (uint32_t)(byte(a, 16) + (int)((byte(b, 16) - byte(a, 16)) * anim_v));
        uint32_t g = (uint32_t)(byte(a,  8) + (int)((byte(b,  8) - byte(a,  8)) * anim_v));
        uint32_t bb = (uint32_t)(byte(a,  0) + (int)((byte(b,  0) - byte(a,  0)) * anim_v));
        return (r << 16) | (g << 8) | bb;
    };
    uint32_t lab_rgb = interp(pal.text_muted, pal.primary);
    auto* lab_fmt = app.texts().format(L"Microsoft YaHei UI", lab_size,
                                       anim_v > 0.5f ? DWRITE_FONT_WEIGHT_BOLD
                                                     : DWRITE_FONT_WEIGHT_NORMAL);
    prim::drawText_(ctx, labelStr, lab_fmt,
                    ix + 14.0f, lab_y, iw - 28.0f, lab_size + 6.0f,
                    br.solidA(lab_rgb, op),
                    DWRITE_TEXT_ALIGNMENT_LEADING,
                    DWRITE_PARAGRAPH_ALIGNMENT_NEAR);

    // 文本 + 选区高亮 + caret
    auto* tx_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(10.5f));

    if (focused && box.hasSelection()) {
        float pre_w = measureW(app, box.displaySlice(0, box.selStart()), tx_fmt);
        float in_w  = measureW(app, box.displaySlice(box.selStart(), box.selEnd()), tx_fmt);
        prim::fillRect(ctx,
                       ix + 14.0f + pre_w, iy + 21.0f, in_w, 18.0f,
                       br.solidA(pal.primary, op * 0.38f));
    }

    std::wstring txt = box.display();
    if (!txt.empty()) {
        prim::drawText_(ctx, txt, tx_fmt,
                        ix + 14.0f, iy + 22.0f, iw - 28.0f, 22.0f,
                        br.solidA(pal.text, op));
    }

    // 0.5Hz 闪烁 caret — 用 g_time_in_stage 周期相位
    // caret 紧贴 baseline（不要从 iy+22 开始顶到 iy+ih-8 那么长 — 会和文字字符的左缘"重叠"
    // 看起来像光标穿过字。压缩到 18px 高 + 文字基线对齐）
    if (focused && !box.hasSelection()) {
        float pre_w = measureW(app, box.displaySlice(0, box.cursor), tx_fmt);
        float cur_x = ix + 14.0f + pre_w;
        int phase = (int)(stages::g_time_in_stage * 1000) % 1000;
        if (phase < 500) {
            float cy_top = iy + 24.0f;
            float cy_bot = cy_top + 18.0f;
            if (cy_bot > iy + ih - 4.0f) cy_bot = iy + ih - 4.0f;
            prim::drawLine(ctx, cur_x, cy_top, cur_x, cy_bot,
                           br.solidA(pal.primary, op), 1.5f);
        }
    }

    hit(box.bounds, [idx](){
        g_form.focus = idx;
        if (idx != 0) g_form.username.clearSel();
        if (idx != 1) g_form.password.clearSel();
        if (idx != 2) g_form.invite.clearSel();
    }, true);
}

void paintAuthView(D2DApp& app, float W, float H) {
    hitClear();
    const Palette& pal = palette();
    auto* ctx = app.ctx();
    auto& br = app.brushes();

    ctx->Clear(argbToColorF(pal.bg));

    bool reg = (stages::g_auth_mode == AuthMode::Register);
    const float cw = 380.0f;
    // 高度按 fields 数动态算：header 100 + fields*62 + btn 60 + (error 32) + switch 28 + bottom 24
    int fields = reg ? 3 : 2;
    const float ch = 100.0f + fields * 62.0f + 60.0f
                   + (g_form.error_msg.empty() ? 0.0f : 32.0f)
                   + 28.0f + 24.0f;
    const float cx = (W - cw) * 0.5f;
    const float cy = (H - ch) * 0.5f + stages::g_auth_card_y.value();
    float op = stages::g_auth_card_op.value();
    if (op <= 0.001f) return;

    // 卡片阴影 + 卡身
    prim::drawShadow(ctx, br, cx, cy, cw, ch, 16.0f,
                     pal.shadow_card, op, 4.0f, 3);
    prim::fillRR(ctx, cx, cy, cw, ch, 16.0f, br.solidA(pal.card, op));

    // ===== Logo + h1 + tagline =====
    float lr = 14.0f, lx = cx + 30, ly = cy + 32;
    prim::fillCircle(ctx, lx + lr, ly + lr, lr, br.solidA(pal.primary, op));
    auto* logo_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(12.0f),
                                        DWRITE_FONT_WEIGHT_BOLD);
    prim::drawText_(ctx, L"L", logo_fmt,
                    lx, ly, lr * 2, lr * 2,
                    br.solidA(0xFFFFFF, op),
                    DWRITE_TEXT_ALIGNMENT_CENTER,
                    DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

    auto* h1 = app.texts().format(L"Microsoft YaHei UI", ptToDip(16.0f),
                                  DWRITE_FONT_WEIGHT_BOLD);
    auto* sub = app.texts().format(L"Microsoft YaHei UI", ptToDip(9.5f));
    prim::drawText_(ctx, reg ? trW("auth.register.title") : trW("auth.login.title"), h1,
                    lx + lr * 2 + 10, cy + 28, 220, 26,
                    br.solidA(pal.text, op));
    prim::drawText_(ctx, reg ? trW("auth.register.sub") : trW("auth.login.sub"), sub,
                    cx + 30, cy + 60, 320, 18,
                    br.solidA(pal.text_muted, op));

    // ===== Fields =====
    float fy = cy + 90;
    std::wstring ph_user = trW("auth.username");
    drawField(app, g_form.username, cx + 30, fy, cw - 60, 48,
              ph_user.c_str(), 0, op);
    fy += 62;
    g_form.password.password = true;
    std::wstring ph_pass = trW("auth.password");
    drawField(app, g_form.password, cx + 30, fy, cw - 60, 48,
              ph_pass.c_str(), 1, op);
    fy += 62;
    if (reg) {
        std::wstring ph_invite = trW("auth.invite");
        drawField(app, g_form.invite, cx + 30, fy, cw - 60, 48,
                  ph_invite.c_str(), 2, op);
        fy += 62;
    }

    // ===== Submit btn 立体 press 效果 =====
    LayoutRect btn{ cx + 30, fy + 6, cw - 60, 44 };
    bool bhov = btn.contains(g_mouse);
    bool bpress = bhov && g_mouse_pressed;
    float lift = bhov && !bpress ? -1.0f : (bpress ? 1.0f : 0.0f);

    uint32_t btn_rgb = g_form.busy
        ? 0x6B6A67
        : (bhov ? pal.primary_hover : pal.primary);
    // glow 阴影（press 时收紧）
    prim::drawShadow(ctx, br,
                     btn.x, btn.y + lift + (bpress ? 0.0f : 2.0f),
                     btn.w, btn.h, 10.0f,
                     pal.primary, op * (bpress ? 0.12f : 0.28f),
                     bpress ? 1.0f : 4.0f, bpress ? 1 : 3);
    prim::fillRR(ctx, btn.x, btn.y + lift, btn.w, btn.h, 10.0f,
                 br.solidA(btn_rgb, op));
    auto* btn_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(11.0f),
                                       DWRITE_FONT_WEIGHT_BOLD);
    std::wstring btn_label = g_form.busy
        ? trW("auth.busy")
        : (reg ? trW("auth.register") : trW("auth.login"));
    prim::drawText_(ctx, btn_label, btn_fmt,
                    btn.x, btn.y + lift, btn.w, btn.h,
                    br.solidA(0xFFFFFF, op),
                    DWRITE_TEXT_ALIGNMENT_CENTER,
                    DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    if (!g_form.busy) {
        hit(btn, [](){ PostMessageW(GetActiveWindow(), WM_APP + 1, 0, 0); }, true);
    }

    // ===== Error 提示 =====
    if (!g_form.error_msg.empty()) {
        LayoutRect errR{ btn.x, btn.y + 56, btn.w, 26 };
        prim::fillRR(ctx, errR.x, errR.y, errR.w, errR.h, 8.0f,
                     br.solidA(0xE34B4B, op * 0.11f));
        auto* err_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(9.0f));
        prim::drawText_(ctx, g_form.error_msg, err_fmt,
                        errR.x + 10, errR.y + 4, errR.w - 20, 18,
                        br.solidA(0xFF8A80, op));
    }

    // ===== Switch link (Login ↔ Register) — 紧跟在 btn / error 下方 =====
    auto* link_normal = app.texts().format(L"Microsoft YaHei UI", ptToDip(9.0f));
    auto* link_bold   = app.texts().format(L"Microsoft YaHei UI", ptToDip(9.0f),
                                           DWRITE_FONT_WEIGHT_BOLD);
    std::wstring prompt = reg ? trW("auth.to_login") : trW("auth.to_register");
    std::wstring link_label = reg ? trW("auth.go_login") : trW("auth.go_register");
    float link_y = btn.y + btn.h + 16.0f
                 + (g_form.error_msg.empty() ? 0.0f : 32.0f);
    // prompt 宽度按文案实测，link 紧跟其后（日文「既にアカウントがある？」比中英长，
    // 固定 +130 偏移会与 link 重叠）。
    float prompt_w = measureW(app, prompt, link_normal);
    prim::drawTextNoWrap(ctx, prompt, link_normal,
                    cx + 30, link_y, prompt_w + 4.0f, 18,
                    br.solidA(pal.text_muted, op));
    float link_w = measureW(app, link_label, link_bold) + 6.0f;
    LayoutRect link{ cx + 30 + prompt_w + 8.0f, link_y, link_w, 18 };
    bool lhov = link.contains(g_mouse);
    prim::drawText_(ctx, link_label, link_bold,
                    link.x, link.y + 1, link.w, 18,
                    br.solidA(lhov ? pal.primary_hover : pal.primary, op));
    hit(link, [](){ switchMode(); }, true);

    // ===== 登录成功大圆形打勾覆盖 =====
    if (stages::g_auth_succeeded && stages::g_check_anim.value() > 0.001f) {
        float ct = stages::g_check_anim.value();
        // 卡内裁剪 — D2D 用 PushAxisAlignedClip 矩形够用（卡是矩形）
        ctx->PushAxisAlignedClip(D2D1::RectF(cx, cy, cx + cw, cy + ch),
                                 D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        prim::fillRR(ctx, cx, cy, cw, ch, 16.0f,
                     br.solidA(pal.card, ct * 0.55f));
        // 中央绿圆
        float cr = 36.0f * ct;
        float ccx = cx + cw * 0.5f, ccy = cy + ch * 0.5f - 10.0f;
        prim::fillCircle(ctx, ccx, ccy, cr, br.solidA(0x4ADE80, ct));
        // 打勾两段
        if (ct > 0.4f) {
            float pp = (std::min)(1.0f, (ct - 0.4f) / 0.5f);
            float x1 = ccx - 14, y1 = ccy + 2;
            float x2 = ccx - 4,  y2 = ccy + 12;
            float x3 = ccx + 14, y3 = ccy - 8;
            float p1 = (std::min)(1.0f, pp * 2.0f);
            float p2 = (std::max)(0.0f, (pp - 0.5f) * 2.0f);
            auto* white = br.solidA(0xFFFFFF, ct);
            auto* round = app.strokes().round();
            prim::drawLine(ctx, x1, y1,
                           x1 + (x2 - x1) * p1, y1 + (y2 - y1) * p1,
                           white, 4.0f, round);
            if (p2 > 0) {
                prim::drawLine(ctx, x2, y2,
                               x2 + (x3 - x2) * p2, y2 + (y3 - y2) * p2,
                               white, 4.0f, round);
            }
        }
        if (ct > 0.6f) {
            float a = (ct - 0.6f) / 0.4f;
            auto* ok_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(12.0f),
                                              DWRITE_FONT_WEIGHT_BOLD);
            prim::drawText_(ctx, trW("auth.login_ok"), ok_fmt,
                            cx, ccy + 36, cw, 22,
                            br.solidA(pal.text, a),
                            DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        ctx->PopAxisAlignedClip();
    }
}

void switchMode() {
    stages::g_auth_mode =
        (stages::g_auth_mode == AuthMode::Login) ? AuthMode::Register : AuthMode::Login;
    g_form.error_msg.clear();
    g_form.focus = 0;
    stages::g_auth_card_op.start(0.0f, 1.0f, 0.32f, 0.0f, curve::easeOutCubic);
    stages::g_auth_card_y.start(20.0f, 0.0f, 0.40f, 0.0f, curve::easeOutQuint);
}

// 跨线程数据交换 — 后台线程写，主线程 onSubmitResult 读。
namespace {
struct SubmitArg {
    std::wstring user, pass, invite;
    bool reg;
    HWND hwnd;
};
std::mutex g_pending_mtx;
std::wstring g_pending_session_token;
std::wstring g_pending_user_id;
std::wstring g_pending_uid;
std::wstring g_pending_nick;
std::wstring g_pending_error;
bool g_pending_hwid_ok = true;

std::wstring utf8ToW(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(n - 1, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}
std::string wToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s(n - 1, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}
}  // anon

void handleSubmit(HWND hwnd) {
    if (g_form.busy) return;
    if (g_form.username.text.empty() || g_form.password.text.empty()) {
        g_form.error_msg = trW("auth.empty");
        return;
    }
    if (stages::g_auth_mode == AuthMode::Register && g_form.invite.text.empty()) {
        g_form.error_msg = trW("auth.empty_invite");
        return;
    }
    g_form.error_msg.clear();
    g_form.busy = true;

    auto* a = new SubmitArg{
        g_form.username.text,
        g_form.password.text,
        g_form.invite.text,
        stages::g_auth_mode == AuthMode::Register,
        hwnd
    };
    CreateThread(nullptr, 0, [](LPVOID lp) -> DWORD {
        std::unique_ptr<SubmitArg> a((SubmitArg*)lp);
        std::string u = net::jsonEscape(a->user);
        std::string p = net::jsonEscape(a->pass);
        std::string hwid = hwidHex();             // 64 hex SHA-256，后端强制
        // 地理国家码：ip-api.com 异步解析后写入 g_geo_country（如 L"CN"）。
        // 与 geoIP 竞争 —— 启动后首次登录可能尚未解析完，此时不带该字段（后端可选）。
        std::string geo;
        if (g_geo_country[0] != 0) {
            geo = ",\"geo_country\":\"" + net::jsonEscape(g_geo_country) + "\"";
        }
        std::string body;
        const wchar_t* path;
        if (a->reg) {
            std::string inv = net::jsonEscape(a->invite);
            body = "{\"username\":\"" + u
                 + "\",\"password\":\"" + p
                 + "\",\"hwid_hex\":\"" + hwid
                 + "\",\"client_ver\":\"0.1\""
                 + ",\"invite_code\":\"" + inv + "\"}";
            path = L"/api/auth/register";
        } else {
            body = "{\"username\":\"" + u
                 + "\",\"password\":\"" + p
                 + "\",\"hwid_hex\":\"" + hwid
                 + "\",\"client_ver\":\"0.1\""
                 + geo + "}";
            path = L"/api/auth/login";
        }
        auto resp = net::postJson(path, body);
        {
            std::lock_guard<std::mutex> lk(g_pending_mtx);
            if (resp.ok()) {
                g_pending_session_token = utf8ToW(net::jsonStr(resp.body, "session_token"));
                g_pending_user_id       = utf8ToW(net::jsonStr(resp.body, "user_id"));
                g_pending_uid           = utf8ToW(net::jsonStr(resp.body, "uid"));
                g_pending_nick          = utf8ToW(net::jsonStr(resp.body, "nickname"));
                // hwid_ok=false → 换机登录，功能受限（仅可发工单/重绑）。缺省视为 true。
                g_pending_hwid_ok       = net::jsonRaw(resp.body, "hwid_ok") != "false";
                g_pending_error.clear();
                // DPAPI 加密保存凭据，下次自动登录用
                persist::saveCreds(a->user, a->pass);
            } else {
                // 把 body 直接当错误消息（GDI+ Preview 同款做法），更接近后端真实错误
                std::string msg = resp.body;
                if (msg.empty()) {
                    msg = (resp.status == 0) ? tr("auth.err_no_server")
                                              : tr("auth.err_login_fail");
                }
                if (msg.size() > 80) msg = msg.substr(0, 80);
                g_pending_error = utf8ToW(msg);
            }
        }
        PostMessageW(a->hwnd, WM_APP + 2, resp.ok() ? 1 : 0, 0);
        return 0;
    }, a, 0, nullptr);
}

void onSubmitResult(HWND /*hwnd*/, bool success) {
    std::lock_guard<std::mutex> lk(g_pending_mtx);
    g_form.busy = false;
    if (success) {
        g_session_token = wToUtf8(g_pending_session_token);
        g_user_id       = wToUtf8(g_pending_user_id);
        // 切换账号：清掉上一个账号残留的消息缓存，避免它们在新账号下
        // 仍被当成"自己"而全部右对齐。必须在 g_user_id 更新之后调用。
        chat::resetForAccount();
        if (!g_pending_uid.empty())  g_user.uid       = g_pending_uid;
        if (!g_pending_nick.empty()) g_user.nickname  = g_pending_nick;
        g_user.username  = g_form.username.text;
        g_user.hwid_ok   = g_pending_hwid_ok;
        g_form.error_msg.clear();
        stages::simulateAuthSubmit();
    } else {
        g_form.error_msg = g_pending_error.empty() ? trW("auth.err_login_fail") : g_pending_error;
    }
}

bool onMouseLDown(HWND /*hwnd*/, POINT dip) {
    return dispatchClick(dip);
}

void onChar(HWND hwnd, wchar_t c, bool ctrl) {
    std::array<InputBox*, 3> boxes{ &g_form.username, &g_form.password, &g_form.invite };
    if (g_form.focus < 0 || g_form.focus >= 3) return;
    boxes[g_form.focus]->onChar(c, ctrl, hwnd);
}

void onKey(HWND hwnd, int vk, bool shift, bool ctrl) {
    if (vk == VK_RETURN) {
        handleSubmit(hwnd);
        return;
    }
    if (vk == VK_TAB) {
        // Tab 切下一 field；Shift+Tab 上一
        bool reg = (stages::g_auth_mode == AuthMode::Register);
        int max_idx = reg ? 2 : 1;
        int next = g_form.focus + (shift ? -1 : 1);
        if (next < 0) next = max_idx;
        if (next > max_idx) next = 0;
        g_form.focus = next;
        return;
    }
    std::array<InputBox*, 3> boxes{ &g_form.username, &g_form.password, &g_form.invite };
    if (g_form.focus < 0 || g_form.focus >= 3) return;
    boxes[g_form.focus]->onKey(vk, shift, ctrl);
}

}  // namespace launcher::d2d::auth
