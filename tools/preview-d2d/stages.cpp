// 入场动画 + stage state machine — 详见 stages.h。
// GDI+ 等价：tools/preview/loading_demo.cpp 的 paintDot / paintLoading /
// paintCheckSuccess / enter*Stage。这里是 1:1 D2D 翻译。

#include "stages.h"
#include "palette.h"
#include "auth.h"
#include "ui_main.h"
#include "render/primitives.h"
#include "i18n.h"

#include <cmath>
#include <algorithm>

namespace launcher::d2d {
bool g_dark = true;   // 默认 Dark — Claude Design 主稿就是暗色
}

namespace launcher::d2d::stages {

Stage    g_stage = Stage::Dot;
AuthMode g_auth_mode = AuthMode::Login;
View     g_view = View::Home;
bool     g_skip_auth_after_loading = false;
bool     g_auth_validation_pending = false;
float    g_time_in_stage = 0.0f;
float    g_spin_angle = 0.0f;
bool     g_auth_succeeded = false;

Tween g_card_scale, g_card_opacity, g_card_fade_out;
Tween g_window_w, g_window_h;
Tween g_sidebar_x, g_topbar_y, g_main_opacity;
Tween g_view_fade;
Tween g_dot_size, g_dot_alpha;
Tween g_auth_card_y, g_auth_card_op;
Tween g_check_anim;

// dpi_px helper — 业务用 96 DPI 逻辑像素，SetWindowPos 要物理 px
static int dpi_px(D2DApp& app, float logical) {
    return (int)(logical * (app.dpi() / 96.0f) + 0.5f);
}

// ============================== Stage 切换 ==============================
void enterDotStage() {
    g_stage = Stage::Dot;
    g_time_in_stage = 0.0f;
    g_dot_size.start(2, 14, 0.30f, 0.0f, curve::easeOutCubic);
    g_dot_alpha.start(0, 1, 0.25f, 0.0f, curve::easeOutCubic);
}

void enterExpandLoadingStage() {
    g_stage = Stage::ExpandLoading;
    g_time_in_stage = 0.0f;
    g_window_w.start(40, 200, 0.40f, 0.0f, curve::easeOutBack);
    g_window_h.start(40, 200, 0.40f, 0.0f, curve::easeOutBack);
}

void enterLoadingStage() {
    g_stage = Stage::Loading;
    g_time_in_stage = 0.0f;
    g_card_scale.start(0.85f, 1.0f, 0.30f, 0.0f, curve::easeOutBack);
    g_card_opacity.start(0.0f, 1.0f, 0.25f, 0.0f, curve::easeOutCubic);
}

void enterExpandAuthStage() {
    g_stage = Stage::ExpandAuth;
    g_time_in_stage = 0.0f;
    g_card_fade_out.start(0, 1, 0.25f, 0.0f, curve::easeOutCubic);
    g_window_w.start(200, 480, 0.50f, 0.05f, curve::easeOutQuint);
    g_window_h.start(200, 540, 0.50f, 0.05f, curve::easeOutQuint);
}

void enterAuthStage() {
    g_stage = Stage::Auth;
    g_time_in_stage = 0.0f;
    g_auth_card_op.start(0, 1, 0.40f, 0.05f, curve::easeOutCubic);
    g_auth_card_y.start(12, 0, 0.45f, 0.05f, curve::easeOutQuint);
}

void enterShrinkSuccessStage() {
    g_stage = Stage::ShrinkSuccess;
    g_time_in_stage = 0.0f;
    g_auth_card_op.start(g_auth_card_op.value(), 0, 0.30f, 0.0f, curve::easeOutCubic);
    g_window_w.start(480, 200, 0.45f, 0.05f, curve::easeOutQuint);
    g_window_h.start(540, 200, 0.45f, 0.05f, curve::easeOutQuint);
}

void enterCheckSuccessStage() {
    g_stage = Stage::CheckSuccess;
    g_time_in_stage = 0.0f;
    g_check_anim.start(0.0f, 1.0f, 0.55f, 0.0f, curve::easeOutBack);
}

void enterExpandMainStage() {
    g_stage = Stage::ExpandMain;
    g_time_in_stage = 0.0f;
    g_window_w.start(200, 1100, 0.55f, 0.05f, curve::easeOutQuint);
    g_window_h.start(200, 720,  0.55f, 0.05f, curve::easeOutQuint);
}

void enterMainStage() {
    g_stage = Stage::Main;
    g_time_in_stage = 0.0f;
    g_sidebar_x.start(0, 1, 0.40f, 0.05f, curve::easeOutQuint);
    g_topbar_y.start(0, 1, 0.35f, 0.10f, curve::easeOutCubic);
    g_main_opacity.start(0, 1, 0.45f, 0.15f, curve::easeOutQuint);
}

void simulateAuthSubmit() {
    if (g_stage != Stage::Auth) return;
    g_auth_succeeded = true;
    enterShrinkSuccessStage();
}

void enterAuthFromLogout() {
    // 从 Main 缩到 Auth — 不走 ShrinkSuccess + CheckSuccess 那条线（那是登录成功的路径）
    g_stage = Stage::ExpandAuth;
    g_time_in_stage = 0.0f;
    g_auth_succeeded = false;
    g_skip_auth_after_loading = false;
    g_auth_validation_pending = false;
    // 主窗 / topbar / sidebar 全归零（避免下次再进 Main 残留）
    g_sidebar_x.start(g_sidebar_x.value(), 0.0f, 0.20f, 0, curve::easeOutCubic);
    g_topbar_y.start(g_topbar_y.value(),  0.0f, 0.20f, 0, curve::easeOutCubic);
    g_main_opacity.start(g_main_opacity.value(), 0.0f, 0.20f, 0, curve::easeOutCubic);
    g_check_anim.start(0.0f, 0.0f, 0.001f, 0, curve::easeOutCubic);
    g_card_fade_out.start(0.0f, 0.0f, 0.001f, 0, curve::easeOutCubic);
    g_card_opacity.start(0.0f, 0.0f, 0.001f, 0, curve::easeOutCubic);
    g_auth_card_op.start(0.0f, 0.0f, 0.001f, 0, curve::easeOutCubic);
    g_auth_card_y.start(0.0f, 0.0f, 0.001f, 0, curve::easeOutCubic);
    // 缩窗 1100→480, 720→540（driveTransitions 会 SetWindowPos 跟到位 + 完成后 enterAuthStage）
    g_window_w.start(1100.0f, 480.0f, 0.45f, 0.0f, curve::easeOutQuint);
    g_window_h.start(720.0f,  540.0f, 0.45f, 0.0f, curve::easeOutQuint);
}

// ============================== tick / drive ==============================
void tick(float dt) {
    g_time_in_stage += dt;
    g_spin_angle = std::fmod(g_spin_angle + dt * 360.0f / 1.4f, 360.0f);

    g_card_scale.tick(dt);
    g_card_opacity.tick(dt);
    g_card_fade_out.tick(dt);
    g_window_w.tick(dt);
    g_window_h.tick(dt);
    g_sidebar_x.tick(dt);
    g_topbar_y.tick(dt);
    g_main_opacity.tick(dt);
    g_view_fade.tick(dt);
    g_dot_size.tick(dt);
    g_dot_alpha.tick(dt);
    g_auth_card_y.tick(dt);
    g_auth_card_op.tick(dt);
    g_check_anim.tick(dt);
    // Auth fields 的 floating label tween — 之前没 tick 过，结果 label 永远卡在初始值
    // 跟 caret 在中间重叠（用户图4 报的 BUG）
    auth::g_form.username.float_t.tick(dt);
    auth::g_form.password.float_t.tick(dt);
    auth::g_form.invite.float_t.tick(dt);
}

bool driveTransitions(D2DApp& app, int sw, int sh) {
    auto resize_to_tween = [&]() {
        int w = dpi_px(app, g_window_w.value());
        int h = dpi_px(app, g_window_h.value());
        int x = (sw - w) / 2, y = (sh - h) / 2;
        SetWindowPos(app.hwnd(), nullptr, x, y, w, h,
                     SWP_NOZORDER | SWP_NOSENDCHANGING | SWP_DEFERERASE);
    };

    bool transitioned = false;
    if (g_stage == Stage::Dot && g_time_in_stage > 0.45f) {
        enterExpandLoadingStage();
        transitioned = true;
    } else if (g_stage == Stage::ExpandLoading) {
        resize_to_tween();
        if (g_window_w.done()) { enterLoadingStage(); transitioned = true; }
    } else if (g_stage == Stage::Loading && g_time_in_stage > 1.4f && !g_auth_validation_pending) {
        if (g_skip_auth_after_loading) {
            // auto-login: 跳 Auth 直扩到 Main
            g_stage = Stage::ExpandMain;
            g_time_in_stage = 0.0f;
            g_card_fade_out.start(0, 1, 0.30f, 0.0f, curve::easeOutCubic);
            g_window_w.start(200, 1100, 0.55f, 0.05f, curve::easeOutQuint);
            g_window_h.start(200, 720,  0.55f, 0.05f, curve::easeOutQuint);
        } else {
            enterExpandAuthStage();
        }
        transitioned = true;
    } else if (g_stage == Stage::ExpandAuth) {
        resize_to_tween();
        if (g_window_w.done()) { enterAuthStage(); transitioned = true; }
    } else if (g_stage == Stage::ShrinkSuccess) {
        resize_to_tween();
        if (g_window_w.done()) { enterCheckSuccessStage(); transitioned = true; }
    } else if (g_stage == Stage::CheckSuccess) {
        if (g_check_anim.done() && g_time_in_stage > 0.85f) {
            enterExpandMainStage();
            transitioned = true;
        }
    } else if (g_stage == Stage::ExpandMain) {
        resize_to_tween();
        if (g_window_w.done()) { enterMainStage(); transitioned = true; }
    }
    return transitioned;
}

bool anyAnimating() {
    auto a = [](const Tween& t) { return t.started && !t.done(); };
    if (g_stage != Stage::Main) return true;   // 入场期一直要画
    return a(g_card_scale) || a(g_card_opacity) || a(g_card_fade_out)
        || a(g_window_w) || a(g_window_h)
        || a(g_sidebar_x) || a(g_topbar_y) || a(g_main_opacity)
        || a(g_view_fade)
        || a(g_dot_size) || a(g_dot_alpha)
        || a(g_auth_card_op) || a(g_auth_card_y)
        || a(g_check_anim);
}

// ============================== Paint helpers ==============================

// spinner 弧 — D2D PathGeometry + AddArc。GDI+ Graphics::DrawArc 等价。
static void drawSpinnerArc(D2DApp& app, float cx, float cy, float r, float thick,
                           float start_deg, float sweep_deg, ID2D1Brush* brush) {
    if (!brush) return;
    constexpr float kPi = 3.14159265358979323846f;
    float start = start_deg * kPi / 180.0f;
    float sweep = sweep_deg * kPi / 180.0f;
    float end = start + sweep;

    ComPtr<ID2D1PathGeometry> geo;
    if (FAILED(app.factory()->CreatePathGeometry(&geo))) return;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(geo->Open(&sink))) return;
    sink->BeginFigure(
        D2D1::Point2F(r * std::cos(start), r * std::sin(start)),
        D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddArc(D2D1::ArcSegment(
        D2D1::Point2F(r * std::cos(end), r * std::sin(end)),
        D2D1::SizeF(r, r),
        0.0f,
        D2D1_SWEEP_DIRECTION_CLOCKWISE,
        sweep_deg > 180.0f ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    sink->Close();

    auto* ctx = app.ctx();
    D2D1_MATRIX_3X2_F saved;
    ctx->GetTransform(&saved);
    ctx->SetTransform(D2D1::Matrix3x2F::Translation(cx, cy));
    ctx->DrawGeometry(geo.Get(), brush, thick, app.strokes().round());
    ctx->SetTransform(saved);
}

// ============================== Paint 函数 ==============================
// 接收 W/H 都是 DIP（D2D RT SetDpi 后逻辑像素 = client px / dpi_scale）
// 圆角窗口由 paint() dispatch 顶部 PushLayerRR 整体处理 — paintBg 直接 Clear 即可
// （Clear 在 layer 内 → 仅 layer 圆角内的像素 blit 回主 RT）
static void paintBg(D2DApp& app) {
    app.ctx()->Clear(argbToColorF(palette().bg));
}

static void paintDot(D2DApp& app, float W, float H) {
    paintBg(app);
    float sz = g_dot_size.value();
    float a = g_dot_alpha.value();
    if (a <= 0.001f) return;
    prim::fillCircle(app.ctx(), W * 0.5f, H * 0.5f, sz * 0.5f,
                     app.brushes().solidA(palette().primary, a));
}

static void paintLoading(D2DApp& app, float W, float H) {
    paintBg(app);
    const Palette& pal = palette();

    float scale = g_card_scale.value();
    float op = g_card_opacity.value();
    float exit = g_card_fade_out.value();
    float opacity = op * (1.0f - exit);
    if (opacity <= 0.001f) return;

    float cw = 168.0f * scale, ch = 168.0f * scale;
    float cx = (W - cw) * 0.5f;
    float cy = (H - ch) * 0.5f - 14.0f * exit;

    auto* ctx = app.ctx();
    auto& br = app.brushes();

    prim::drawShadow(ctx, br, cx, cy, cw, ch, 12.0f,
                     pal.shadow_card_hover, opacity, 4.0f, 4);
    prim::fillRR(ctx, cx, cy, cw, ch, 12.0f,
                 br.solidA(pal.card, opacity));

    float spinR = 22.0f * scale;
    float scx = cx + cw * 0.5f, scy = cy + ch * 0.5f - 10.0f * scale;
    drawSpinnerArc(app, scx, scy, spinR, 3.0f, g_spin_angle, 80.0f,
                   br.solidA(pal.primary, opacity));

    auto* fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(9.0f));
    if (fmt) {
        prim::drawText_(ctx, trW("loading.connecting"), fmt,
                        cx, cy + ch - 42.0f, cw, 24.0f,
                        br.solidA(pal.text_muted, opacity),
                        DWRITE_TEXT_ALIGNMENT_CENTER);
    }
}

static void paintCheckSuccess(D2DApp& app, float W, float H) {
    auto* ctx = app.ctx();
    auto& br = app.brushes();
    // 整窗 r=16 绿底（status_online 主色）
    prim::fillRR(ctx, 0, 0, W, H, 16.0f, br.solid(0xFF4ADE80));

    float ct = g_check_anim.value();
    if (ct < 0.001f) return;

    float ccx = W * 0.5f, ccy = H * 0.5f;
    float x1 = ccx - 24, y1 = ccy + 4;
    float x2 = ccx - 6,  y2 = ccy + 22;
    float x3 = ccx + 28, y3 = ccy - 18;
    float p1 = (std::min)(1.0f, ct * 2.0f);
    float p2 = (std::max)(0.0f, (ct - 0.5f) * 2.0f);

    auto* white = br.solidA(0xFFFFFF, (std::min)(1.0f, ct * 1.5f));
    auto* round = app.strokes().round();
    prim::drawLine(ctx, x1, y1, x1 + (x2 - x1) * p1, y1 + (y2 - y1) * p1,
                   white, 6.0f, round);
    if (p2 > 0) {
        prim::drawLine(ctx, x2, y2, x2 + (x3 - x2) * p2, y2 + (y3 - y2) * p2,
                       white, 6.0f, round);
    }
}

// Step 3 改用 auth::paintAuthView（真表单 + InputBox + 浮动 label + 切换登录注册）

// Step 4+ 已用 ui::paintMain — 见 ui_main.cpp

// ============================== Dispatch ==============================
void paint(D2DApp& app) {
    float W = app.widthDip();
    float H = app.heightDip();

    // 整窗圆角 mask — Clear 透明 + PushLayer 圆角 → 所有后续 draw 都被裁到圆角内
    auto* ctx = app.ctx();
    ctx->Clear(D2D1::ColorF(0, 0, 0, 0));
    float r = (W < 80 || H < 80) ? (std::min)(W, H) * 0.5f : 12.0f;
    prim::pushLayerRR(ctx, app.factory(), 0, 0, W, H, r);

    switch (g_stage) {
        case Stage::Dot:
            paintDot(app, W, H); break;
        case Stage::ExpandLoading:
        case Stage::Loading:
        case Stage::Expanding:
        case Stage::ExpandAuth:
            paintLoading(app, W, H); break;
        case Stage::Auth:
        case Stage::ShrinkSuccess:
            auth::paintAuthView(app, W, H); break;
        case Stage::CheckSuccess:
            paintCheckSuccess(app, W, H); break;
        case Stage::ExpandMain:
            // auto-login: Loading 卡片 fade out 期；非 auto-login 走 Auth 卡片 fade
            if (g_skip_auth_after_loading) paintLoading(app, W, H);
            else auth::paintAuthView(app, W, H);
            break;
        case Stage::Main:
        default:
            ui::paintMain(app, W, H); break;
    }

    prim::popLayer(ctx);
}

}  // namespace launcher::d2d::stages
