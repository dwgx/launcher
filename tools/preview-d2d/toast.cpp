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
// 卡片扩展态几何（Expanded/Broadcast）。
constexpr float kHCard    = 84.0f;          // 展开卡片高
constexpr float kRCard    = 20.0f;          // 卡片圆角
constexpr float kCardMinW = 240.0f;         // 卡片最小宽（避免标题过窄）

// 当前展开高度对应的隐藏 y = -(curH+10)。药丸(expand 未起)→ curH=kH → -46，与旧路径数值等价。
float hiddenYForCurrentHeight() {
    float e = g_toast.expand.started ? g_toast.expand.value() : 0.0f;
    float curH = kH + (kHCard - kH) * e;
    return -(curH + 10.0f);
}

// Swap/Flip 中点：把暂存内容落到存活字符串。pendTitle/pendBody 任一非空 → 卡片内容，否则药丸文字。
void applyPending() {
    if (!g_toast.pendTitle.empty() || !g_toast.pendBody.empty()) {
        g_toast.title = g_toast.pendTitle;
        g_toast.body  = g_toast.pendBody;
    } else {
        g_toast.text = g_toast.pendText;
    }
    g_toast.pendText.clear();
    g_toast.pendTitle.clear();
    g_toast.pendBody.clear();
}
}  // namespace

void startExit() {
    // 上滑离场 + 淡出；宽度保持全开（此时收宽会 reflow 文字，读作 jank）。
    // 隐藏 y 按当前高度算：药丸 = -46（与旧路径等价），卡片按 curH。
    g_toast.y.start(g_toast.y.value(), hiddenYForCurrentHeight(), 0.34f, 0, curve::easeOutCubic);
    g_toast.fade.start(g_toast.fade.value(), 0.0f, 0.30f, 0, curve::easeOutCubic);
}

void show(const wchar_t* s) {
    g_toast.text = s;
    g_toast.live = 0.0f;
    // 复位为默认药丸态：不触发任何卡片/翻转/交叉淡入 tween（保持 !started）。
    g_toast.variant = Variant::Toast;
    g_toast.anim    = Anim::Drop;
    g_toast.sticky  = false;
    g_toast.accent  = 0;
    g_toast.midDone = false;
    g_toast.title.clear();
    g_toast.body.clear();
    g_toast.pendText.clear();
    g_toast.pendTitle.clear();
    g_toast.pendBody.clear();
    g_toast.expand  = Tween{};
    g_toast.flipY   = Tween{};
    g_toast.content = Tween{};
    if (g_toast.phase != Toast::Phase::Hidden) {
        // 岛内容切换：不重播整段下滑，只做小幅 re-morph 到新文字宽度。
        g_toast.y.start(g_toast.y.value(), kTopY, 0.28f, 0, curve::easeOutBack);
        g_toast.width.start(0.55f, 1.0f, 0.30f, 0, curve::easeOutQuint);
        g_toast.fade.start(g_toast.fade.value(), 1.0f, 0.15f, 0, curve::easeOutCubic);
    } else {
        // 完整入场：下滑 easeOutBack 回弹 + 宽度 morph-open + 淡入。
        g_toast.y.start(kHiddenY, kTopY, 0.50f, 0, curve::easeOutBack);
        g_toast.width.start(0.0f, 1.0f, 0.44f, 0, curve::easeOutQuint);
        g_toast.fade.start(0.0f, 1.0f, 0.20f, 0, curve::easeOutCubic);
    }
    g_toast.phase = Toast::Phase::Enter;
}

void showBroadcast(const wchar_t* title, const wchar_t* body,
                   uint32_t accent_rgb, bool sticky) {
    g_toast.title  = title ? title : L"";
    g_toast.body   = body ? body : L"";
    g_toast.text.clear();
    g_toast.live   = 0.0f;
    g_toast.variant = Variant::Broadcast;
    g_toast.sticky  = sticky;
    g_toast.accent  = accent_rgb;
    g_toast.midDone = false;
    g_toast.pendText.clear();
    g_toast.pendTitle.clear();
    g_toast.pendBody.clear();
    g_toast.flipY   = Tween{};
    if (g_toast.phase == Toast::Phase::Hidden) {
        // BROADCAST-IN = DROP + expand + content：药丸落定后卡片展开，正文最后淡入。
        g_toast.anim = Anim::Drop;
        float hy = -(kHCard + 10.0f);  // 卡片隐藏 y = -94
        g_toast.y.start(hy, kTopY, 0.50f, 0, curve::easeOutBack);
        g_toast.width.start(0.0f, 1.0f, 0.44f, 0, curve::easeOutQuint);
        g_toast.fade.start(0.0f, 1.0f, 0.20f, 0, curve::easeOutCubic);
        g_toast.expand.start(0.0f, 1.0f, 0.42f, 0.14f, curve::easeOutQuint);
        g_toast.content.start(0.0f, 1.0f, 0.20f, 0.30f, curve::easeOutCubic);
    } else {
        // 已存活的岛 → EXPAND：药丸展开成卡片（unfold 轻微 overshoot），y 不动。
        g_toast.anim = Anim::Expand;
        float e0 = g_toast.expand.started ? g_toast.expand.value() : 0.0f;
        g_toast.expand.start(e0, 1.0f, 0.40f, 0, curve::easeOutBack);
        g_toast.width.start(g_toast.width.value(), 1.0f, 0.34f, 0, curve::easeOutQuint);
        g_toast.content.start(0.0f, 1.0f, 0.24f, 0.12f, curve::easeOutCubic);
        g_toast.y.start(g_toast.y.value(), kTopY, 0.34f, 0, curve::easeOutBack);
        g_toast.phase = Toast::Phase::Enter;  // 重新经 Enter 等待 expand 完成
    }
    if (g_toast.phase == Toast::Phase::Hidden)
        g_toast.phase = Toast::Phase::Enter;
}

// 交叉淡入淡出可见药丸内容；岛隐藏时退化为普通 show。
void swap(const wchar_t* s) {
    if (g_toast.phase == Toast::Phase::Hidden) { show(s); return; }
    g_toast.live = 0.0f;
    g_toast.anim = Anim::Swap;
    g_toast.midDone = false;
    g_toast.pendText  = s ? s : L"";
    g_toast.pendTitle.clear();
    g_toast.pendBody.clear();
    // Phase A：旧内容淡出。中点后由 tick 落定 pending 并淡入。
    float c0 = g_toast.content.started ? g_toast.content.value() : 1.0f;
    g_toast.content.start(c0, 0.0f, 0.13f, 0, curve::easeOutCubic);
}

// 交叉淡入淡出可见卡片内容；岛隐藏时退化为常驻广播。
void swapExpanded(const wchar_t* title, const wchar_t* body) {
    if (g_toast.phase == Toast::Phase::Hidden) {
        showBroadcast(title, body, g_toast.accent, g_toast.sticky);
        return;
    }
    g_toast.live = 0.0f;
    g_toast.anim = Anim::Swap;
    g_toast.midDone = false;
    g_toast.pendText.clear();
    g_toast.pendTitle = title ? title : L"";
    g_toast.pendBody  = body ? body : L"";
    float c0 = g_toast.content.started ? g_toast.content.value() : 1.0f;
    g_toast.content.start(c0, 0.0f, 0.13f, 0, curve::easeOutCubic);
}

// scaleY 挤压翻转（药丸）；岛隐藏时退化为普通 show。
void flip(const wchar_t* s) {
    if (g_toast.phase == Toast::Phase::Hidden) { show(s); return; }
    g_toast.live = 0.0f;
    g_toast.anim = Anim::Flip;
    g_toast.midDone = false;
    g_toast.pendText  = s ? s : L"";
    g_toast.pendTitle.clear();
    g_toast.pendBody.clear();
    // Phase A：flapY 合到细线（内容保持可见，只是被压扁）。
    float f0 = g_toast.flipY.started ? g_toast.flipY.value() : 1.0f;
    g_toast.flipY.start(f0, 0.02f, 0.16f, 0, curve::easeOutCubic);
}

// 动画离场（常驻广播或任意存活岛）。
void dismiss() {
    if (g_toast.phase == Toast::Phase::Hidden) return;
    g_toast.sticky = false;
    startExit();
    g_toast.phase = Toast::Phase::Exit;
}

void tick(float dt) {
    if (g_toast.phase == Toast::Phase::Hidden) return;
    g_toast.y.tick(dt);
    g_toast.width.tick(dt);
    g_toast.fade.tick(dt);
    g_toast.expand.tick(dt);
    g_toast.flipY.tick(dt);
    g_toast.content.tick(dt);
    g_toast.live += dt;

    // Swap 两段式：A 段(content 1->0)结束 → 落定 pending → B 段(content 0->1 + 宽度 re-morph)。
    if (g_toast.anim == Anim::Swap && !g_toast.midDone && g_toast.content.done()) {
        applyPending();
        g_toast.midDone = true;
        g_toast.content.start(0.0f, 1.0f, 0.16f, 0, curve::easeOutCubic);
        g_toast.width.start(0.62f, 1.0f, 0.28f, 0, curve::easeOutQuint);
    }
    // Flip 两段式：A 段(flipY 1->0.02)结束 → 落定 pending → B 段(flipY 0.02->1 + 宽度 re-morph)。
    if (g_toast.anim == Anim::Flip && !g_toast.midDone && g_toast.flipY.done()) {
        applyPending();
        g_toast.midDone = true;
        g_toast.flipY.start(0.02f, 1.0f, 0.22f, 0, curve::easeOutBack);
        g_toast.width.start(0.70f, 1.0f, 0.26f, 0, curve::easeOutQuint);
    }
    // Swap/Flip B 段收尾 → 回到 Drop 稳态 + Hold。
    // 未启动的 tween 视作静止(已完成)：Swap 不动 flipY、Flip 不动 content。
    auto settled = [](const Tween& t) { return !t.started || t.done(); };
    if ((g_toast.anim == Anim::Swap || g_toast.anim == Anim::Flip)
        && g_toast.midDone && settled(g_toast.content) && settled(g_toast.flipY)
        && g_toast.width.done()) {
        g_toast.anim = Anim::Drop;
        if (g_toast.phase != Toast::Phase::Exit)
            g_toast.phase = Toast::Phase::Hold;
    }

    // Enter 完成：药丸只等 y+width；卡片还要等 expand 展开完毕。
    if (g_toast.phase == Toast::Phase::Enter && g_toast.y.done() && g_toast.width.done()
        && (g_toast.variant == Variant::Toast || g_toast.expand.done()))
        g_toast.phase = Toast::Phase::Hold;

    // 自动离场：常驻广播(sticky)永不自退；药丸 -46/2.9 路径不变。
    if (g_toast.phase != Toast::Phase::Exit && !g_toast.sticky && g_toast.live > 2.9f) {
        startExit();
        g_toast.phase = Toast::Phase::Exit;
    }
    if (g_toast.phase == Toast::Phase::Exit && g_toast.fade.done()) {
        g_toast.phase = Toast::Phase::Hidden;
        g_toast.text.clear();
        g_toast.title.clear();
        g_toast.body.clear();
    }
}

void paint(D2DApp& app, float W, float H) {
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

    // expand 混合：e==0 纯药丸（像素与旧版一致），e==1 卡片。
    float e    = g_toast.expand.started ? g_toast.expand.value() : 0.0f;
    float curH = kH + (kHCard - kH) * e;
    float curR = kR + (kRCard - kR) * e;   // fillRR 会再钳
    uint32_t acc = g_toast.accent ? g_toast.accent : pal.primary;

    // 内容测量：药丸看 text，卡片看 title/body 更宽者。
    DWRITE_TEXT_METRICS mLine{};
    app.texts().measure(fmt, g_toast.text, wMaxCap - kTextStart - kPadR, 256, &mLine);
    float fullW = kTextStart + mLine.width + kPadR;
    if (e > 0.001f) {
        auto* fmtTitle = app.texts().format(L"Microsoft YaHei UI", ptToDip(10.5f),
                                            DWRITE_FONT_WEIGHT_BOLD);
        DWRITE_TEXT_METRICS mt{}, mb{};
        app.texts().measure(fmtTitle, g_toast.title, wMaxCap - kPadL - kPadR, 256, &mt);
        app.texts().measure(fmt, g_toast.body, wMaxCap - kPadL - kPadR, 256, &mb);
        float cardW = kPadL + (std::max)(mt.width, mb.width) + kPadR;
        if (cardW < kCardMinW) cardW = kCardMinW;
        fullW = fullW + (cardW - fullW) * e;   // 药丸宽 → 卡片宽 随 e 混合
    }
    if (fullW < kWMin)    fullW = kWMin;
    if (fullW > wMaxCap)  fullW = wMaxCap;

    float wp = g_toast.width.value();
    float curW = kWMin + (fullW - kWMin) * wp;   // 当前胶囊/卡片宽
    float tx = (W - curW) * 0.5f;                // 始终水平居中
    float ty = g_toast.y.value();
    float opacity = g_toast.fade.value();
    // 文字/圆点在展开后半段才显形，避免玻璃字溢出过窄的胶囊。
    float textAlpha = opacity * (std::max)(0.0f, (std::min)(1.0f, (wp - 0.35f) / 0.65f));
    // Swap 内容交叉淡入淡出乘子（静止=1）。
    float contentMul = g_toast.content.started ? g_toast.content.value() : 1.0f;

    // Flip：绕胶囊中心竖向缩放（镜像 stages.cpp:248-252，乘 saved 保留 RT 基变换）。
    float sy = g_toast.flipY.started ? g_toast.flipY.value() : 1.0f;
    D2D1_MATRIX_3X2_F saved;
    bool flipped = (sy < 0.999f);
    if (flipped) {
        ctx->GetTransform(&saved);
        ctx->SetTransform(D2D1::Matrix3x2F::Scale(
                              D2D1::SizeF(1.0f, sy),
                              D2D1::Point2F(tx + curW * 0.5f, ty + curH * 0.5f)) * saved);
    }

    // 1. 阴影（轻薄悬浮感）。
    prim::drawShadow(ctx, br, tx, ty, curW, curH, curR, 0x60000000, opacity * 0.85f, 3.0f, 3);
    // 2. 深色填充。
    prim::fillRR(ctx, tx, ty, curW, curH, curR, br.solidA(pal.card, opacity * 0.98f));
    // 3. 边缘描边。
    prim::strokeRR(ctx, tx, ty, curW, curH, curR, br.solidA(pal.divider, opacity * 0.7f), 1.0f);

    // Broadcast 左侧 accent 竖条（仅卡片态显形）。
    if (e > 0.001f && g_toast.variant == Variant::Broadcast) {
        float barX = tx + 6.0f, barW = 3.0f;
        float barY = ty + curH * 0.28f, barH = curH * 0.44f;
        prim::fillRR(ctx, barX, barY, barW, barH, barW * 0.5f,
                     br.solidA(acc, textAlpha * e));
    }

    // 4. 前导 accent 圆点（药丸态；卡片态随 e 淡出让位给标题）。
    prim::fillCircle(ctx, tx + kPadL + kDotR, ty + curH * 0.5f, kDotR,
                     br.solidA(acc, textAlpha * (1.0f - e)));

    // 5a. 单行药丸文字（e==0 时唯一路径，像素与旧版一致；随 e 淡出）。
    float singleAlpha = textAlpha * (1.0f - e) * contentMul;
    if (singleAlpha > 0.003f) {
        prim::drawTextNoWrap(ctx, g_toast.text, fmt,
                        tx + kTextStart, ty, curW - kTextStart - kPadR, curH,
                        br.solidA(pal.text, singleAlpha),
                        DWRITE_TEXT_ALIGNMENT_LEADING,
                        DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    // 5b. 卡片文字：标题(bold 10.5)上、正文(wrap)下（随 e 淡入）。
    float cardAlpha = textAlpha * e * contentMul;
    if (cardAlpha > 0.003f) {
        auto* fmtTitle = app.texts().format(L"Microsoft YaHei UI", ptToDip(10.5f),
                                            DWRITE_FONT_WEIGHT_BOLD);
        float pad = kPadL + (g_toast.variant == Variant::Broadcast ? 6.0f : 0.0f);
        prim::drawTextNoWrap(ctx, g_toast.title, fmtTitle,
                        tx + pad, ty + 12.0f, curW - pad - kPadR, 22.0f,
                        br.solidA(pal.text, cardAlpha),
                        DWRITE_TEXT_ALIGNMENT_LEADING,
                        DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        prim::drawText_(ctx, g_toast.body, fmt,
                        tx + pad, ty + 36.0f, curW - pad - kPadR, curH - 42.0f,
                        br.solidA(pal.text_muted, cardAlpha),
                        DWRITE_TEXT_ALIGNMENT_LEADING,
                        DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        // 常驻广播的关闭提示符（纯视觉；点击接线是后续任务，dismiss() 是程序化路径）。
        if (g_toast.sticky && e > 0.5f) {
            prim::drawTextNoWrap(ctx, L"✕", fmt,
                            tx + curW - kPadR - 6.0f, ty, 14.0f, curH,
                            br.solidA(pal.text_muted, cardAlpha * 0.6f),
                            DWRITE_TEXT_ALIGNMENT_LEADING,
                            DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
    }

    if (flipped) ctx->SetTransform(saved);
}

}  // namespace launcher::d2d::toast

