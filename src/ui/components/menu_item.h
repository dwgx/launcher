#pragma once

#include "app/common.h"
#include "ui/anim/animated_property.h"
#include <functional>

namespace launcher::ui::render { class SkiaRenderer; class FontManager; }

namespace launcher::ui::components {

class MenuItem {
public:
    using OnClickFn = std::function<void()>;

    void setLabel(std::string label) { m_label = std::move(label); }
    void setGlyph(std::string glyph) { m_glyph = std::move(glyph); }
    void setActive(bool active);
    void setOnClick(OnClickFn fn) { m_on_click = std::move(fn); }

    void tick(f32 dt);
    void draw(render::SkiaRenderer& r, render::FontManager& f,
              f32 x, f32 y, f32 w, f32 h);

    // 鼠标事件接入
    void onMouseMove(f32 mx, f32 my, f32 x, f32 y, f32 w, f32 h);
    bool onClick(f32 mx, f32 my, f32 x, f32 y, f32 w, f32 h);

private:
    std::string m_label;
    std::string m_glyph;
    bool m_active{false};
    bool m_hover{false};
    anim::AnimatedProperty<f32> m_bg_alpha{0.0f};
    anim::AnimatedProperty<f32> m_indicator_w{0.0f};   // 激活时左侧 3px 主色指示条
    OnClickFn m_on_click;
};

}  // namespace launcher::ui::components
