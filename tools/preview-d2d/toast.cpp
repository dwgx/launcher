#include "toast.h"
#include "palette.h"
#include "render/primitives.h"

#include <algorithm>
#include <cmath>

namespace launcher::d2d::toast {

Toast g_toast;

namespace {
// 几何常量（全 DIP，paint(W,H) 空间）。见 toast.h 头注的时间线。
constexpr float kH        = 36.0f;          // 胶囊高（药丸）
constexpr float kR        = kH * 0.5f;      // 圆角 = 半高 → 真胶囊 (fillRR 再钳一次)
constexpr float kTopY     = 14.0f;          // 静止 y，贴近顶边 dynamic-island 风
constexpr float kHiddenY  = -(kH + 10.0f);  // -46：完全藏在客户端上方（药丸）
}  // namespace

void startExit() {
    // 上滑离场 + 淡出；宽度保持全开（此时收宽会 reflow 文字，读作 jank）。
    g_toast.y.start(g_toast.y.value(), kHiddenY, 0.34f, 0, curve::easeOutCubic);
    g_toast.fade.start(g_toast.fade.value(), 0.0f, 0.30f, 0, curve::easeOutCubic);
}

void show(const wchar_t* s) {
    g_toast.text = s;
    g_toast.live = 0.0f;
    // 始终满宽(不做宽度 morph)—— 纯竖直:从顶部中心上方落到顶部中心,横向不动。
    g_toast.width.start(1.0f, 1.0f, 0.001f, 0, curve::easeOutQuint);
    if (g_toast.phase != Toast::Phase::Hidden) {
        // 岛内容切换：不重播整段下滑,只淡一下。
        g_toast.y.start(g_toast.y.value(), kTopY, 0.24f, 0, curve::easeOutCubic);
        g_toast.fade.start(g_toast.fade.value(), 1.0f, 0.15f, 0, curve::easeOutCubic);
    } else {
        // 完整入场：从上方竖直下滑 + 淡入(easeOutBack 轻微回弹)。
        g_toast.y.start(kHiddenY, kTopY, 0.42f, 0, curve::easeOutBack);
        g_toast.fade.start(0.0f, 1.0f, 0.20f, 0, curve::easeOutCubic);
    }
    g_toast.phase = Toast::Phase::Enter;
}

void tick(float dt) {
    if (g_toast.phase == Toast::Phase::Hidden) return;
    g_toast.y.tick(dt);
    g_toast.width.tick(dt);
    g_toast.fade.tick(dt);
    g_toast.live += dt;

    // Enter 完成：药丸等 y+width 落定 → Hold。
    if (g_toast.phase == Toast::Phase::Enter && g_toast.y.done() && g_toast.width.done())
        g_toast.phase = Toast::Phase::Hold;

    // 自动离场：药丸存活 > 2.9s 上滑退场。
    if (g_toast.phase != Toast::Phase::Exit && g_toast.live > 2.9f) {
        startExit();
        g_toast.phase = Toast::Phase::Exit;
    }
    if (g_toast.phase == Toast::Phase::Exit && g_toast.fade.done()) {
        g_toast.phase = Toast::Phase::Hidden;
        g_toast.text.clear();
    }
}

void paint(D2DApp& app, float W, float H) {
    (void)H;
    if (g_toast.phase == Toast::Phase::Hidden || g_toast.fade.value() < 0.003f) return;
    const Palette& pal = palette();
    auto* ctx = app.ctx();
    auto& br = app.brushes();

    auto* fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(9.5f));

    // morph 几何：wMin 折叠胶囊 → fullW 贴合内容（每帧测量）。
    const float kWMin      = 44.0f;
    const float kPadL      = 18.0f;
    const float kPadR      = 18.0f;
    const float kDotR      = 3.5f;
    const float kDotGap    = 9.0f;
    const float kTextStart = kPadL + 2.0f * kDotR + kDotGap;  // 34：文字相对胶囊左的 x
    float wMaxCap = (std::min)(W - 32.0f, 460.0f);
    if (wMaxCap < 120.0f) wMaxCap = 120.0f;

    // 内容测量：单行 text 宽度。
    DWRITE_TEXT_METRICS mLine{};
    app.texts().measure(fmt, g_toast.text, wMaxCap - kTextStart - kPadR, 256, &mLine);
    float fullW = kTextStart + mLine.width + kPadR;
    if (fullW < kWMin)    fullW = kWMin;
    if (fullW > wMaxCap)  fullW = wMaxCap;

    float wp = g_toast.width.value();
    float curW = kWMin + (fullW - kWMin) * wp;   // 当前胶囊宽
    float tx = (W - curW) * 0.5f;                // 始终水平居中
    float ty = g_toast.y.value();
    float opacity = g_toast.fade.value();
    // 文字/圆点在展开后半段才显形，避免玻璃字溢出过窄的胶囊。
    float textAlpha = opacity * (std::max)(0.0f, (std::min)(1.0f, (wp - 0.35f) / 0.65f));

    // 1. 阴影（轻薄悬浮感）。
    prim::drawShadow(ctx, br, tx, ty, curW, kH, kR, 0x60000000, opacity * 0.85f, 3.0f, 3);
    // 2. 深色填充。
    prim::fillRR(ctx, tx, ty, curW, kH, kR, br.solidA(pal.card, opacity * 0.98f));
    // 3. 边缘描边。
    prim::strokeRR(ctx, tx, ty, curW, kH, kR, br.solidA(pal.divider, opacity * 0.7f), 1.0f);

    // 4. 前导 accent 圆点。
    prim::fillCircle(ctx, tx + kPadL + kDotR, ty + kH * 0.5f, kDotR,
                     br.solidA(pal.primary, textAlpha));

    // 5. 单行药丸文字。
    if (textAlpha > 0.003f) {
        prim::drawTextNoWrap(ctx, g_toast.text, fmt,
                        tx + kTextStart, ty, curW - kTextStart - kPadR, kH,
                        br.solidA(pal.text, textAlpha),
                        DWRITE_TEXT_ALIGNMENT_LEADING,
                        DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
}

}  // namespace launcher::d2d::toast

