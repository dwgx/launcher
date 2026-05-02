#include "ui/views/loading_view.h"
#include "ui/render/skia_renderer.h"
#include "ui/render/font_manager.h"
#include "ui/theme/animation.h"

namespace launcher::ui::views {

void LoadingView::tick(f32 dt_seconds) {
    m_elapsed += dt_seconds;
    m_state.fade_in = theme::clamp01(m_elapsed / 0.18f);
    m_card.tick(dt_seconds);

    if (!m_ready && m_elapsed >= kFakeDelaySec) {
        markReady();
    }
}

void LoadingView::draw(render::SkiaRenderer& r, render::FontManager& fonts) {
    m_card.draw(r, fonts, m_state);
}

void LoadingView::markReady() {
    if (m_ready) return;
    m_ready = true;
    if (m_on_ready) m_on_ready();
}

}  // namespace launcher::ui::views
