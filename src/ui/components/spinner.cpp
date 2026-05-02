#include "ui/components/spinner.h"
#include "ui/render/skia_renderer.h"

namespace launcher::ui::components {

void Spinner::tick(f32 dt_seconds) {
    m_angle += kRotPerSec * dt_seconds;
    while (m_angle >= 360.0f) m_angle -= 360.0f;
}

void Spinner::draw(render::SkiaRenderer& r, f32 cx, f32 cy,
                   f32 radius, f32 stroke, theme::Color color) {
    r.drawArc(cx, cy, radius, m_angle, kSweepDeg, stroke, color);
}

}  // namespace launcher::ui::components
