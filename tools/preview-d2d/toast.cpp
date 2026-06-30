#include "toast.h"
#include "palette.h"
#include "render/primitives.h"

#include <algorithm>
#include <cmath>

namespace launcher::d2d::toast {

Toast g_toast;

void show(const wchar_t* s) {
    g_toast.text = s;
    g_toast.live = 0.0f;
    g_toast.t.start(0.0f, 1.0f, 0.20f, 0, curve::easeOutCubic);
}

void tick(float dt) {
    g_toast.t.tick(dt);
    if (!g_toast.text.empty()) {
        g_toast.live += dt;
        if (g_toast.live > 2.5f && std::abs(g_toast.t.to - 0.0f) > 0.001f) {
            g_toast.t.start(g_toast.t.value(), 0.0f, 0.30f, 0, curve::easeOutCubic);
        }
        if (g_toast.t.value() < 0.001f && g_toast.t.to == 0.0f && g_toast.t.done()) {
            g_toast.text.clear();
        }
    }
}

void paint(D2DApp& app, float W, float H) {
    if (g_toast.text.empty() || g_toast.t.value() < 0.001f) return;
    const Palette& pal = palette();
    auto* ctx = app.ctx();
    auto& br = app.brushes();
    float t = g_toast.t.value();

    auto* fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(9.5f));
    // 先按窗口宽算出可用上限，再在该上限内测量 — 否则长文案（中/日/带计数）
    // 会让 tw 超过窗口宽，tx 变负把左侧文字推出屏幕外。
    float max_tw = (std::min)(W - 48.0f, 520.0f);
    if (max_tw < 120.0f) max_tw = 120.0f;
    DWRITE_TEXT_METRICS m{};
    app.texts().measure(fmt, g_toast.text, max_tw - 36.0f, 256, &m);
    float tw = (std::min)(m.width + 36.0f, max_tw);
    float th = 36.0f;
    float tx = (std::max)(8.0f, W - tw - 24.0f);
    float ty = H - th - 24.0f - 8.0f * (1.0f - t);

    prim::drawShadow(ctx, br, tx, ty, tw, th, 8.0f, 0x99000000, t, 4.0f, 3);
    prim::fillRR(ctx, tx, ty, tw, th, 8.0f, br.solidA(pal.card, t * 0.96f));
    prim::strokeRR(ctx, tx, ty, tw, th, 8.0f, br.solidA(pal.divider, t));
    prim::drawTextNoWrap(ctx, g_toast.text, fmt,
                    tx + 18, ty + 11, tw - 36, 18,
                    br.solidA(pal.text, t),
                    DWRITE_TEXT_ALIGNMENT_LEADING,
                    DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
}

}  // namespace launcher::d2d::toast
