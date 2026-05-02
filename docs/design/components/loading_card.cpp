#include "ui/components/loading_card.h"
#include "ui/render/skia_renderer.h"
#include "ui/render/font_manager.h"
#include "ui/theme/theme_manager.h"

namespace launcher::ui::components {

void LoadingCard::tick(f32 dt_seconds) {
    m_spinner.tick(dt_seconds);
}

// 200×200 窗口里画一个 168×168 的卡片，居中；spinner 半径 22；caption 在 spinner 下方
void LoadingCard::draw(render::SkiaRenderer& r, render::FontManager& fonts,
                       const LoadingCardState& state) {
    const auto pal = theme::ThemeManager::instance().palette();

    const f32 cw = 168.0f, ch = 168.0f;
    const f32 cx_origin = (r.width()  - cw) * 0.5f;
    const f32 cy_origin = (r.height() - ch) * 0.5f;

    render::ShadowSpec shadow{0.0f, 4.0f, 12.0f, pal.shadow_hover};
    r.drawRoundRect(cx_origin, cy_origin, cw, ch,
                    theme::kRadiusMd, pal.card, &shadow);

    const f32 spinner_cx = cx_origin + cw * 0.5f;
    const f32 spinner_cy = cy_origin + ch * 0.5f - 10.0f;
    m_spinner.draw(r, spinner_cx, spinner_cy, /*radius*/22.0f,
                   /*stroke*/3.0f, pal.primary);

    if (!state.caption.empty()) {
        const f32 text_size = 12.0f;
        f32 w = fonts.measureWidth(state.caption, text_size);
        f32 tx = cx_origin + (cw - w) * 0.5f;
        f32 ty = cy_origin + ch - 36.0f;
        r.drawText(state.caption, tx, ty, text_size, pal.text_muted, &fonts);
    }
}

}  // namespace launcher::ui::components
