#pragma once

// 圆形头像。优先用图片；图片缺失时画"主色背景 + 用户首字母"占位。
// Why: 头像只在 topbar / HomeView 用，独立组件方便日后接 OAuth 头像。

#include "app/common.h"
#include "ui/anim/animated_property.h"
#include "ui/theme/color_tokens.h"

namespace launcher::ui::render { class SkiaRenderer; class FontManager; }

namespace launcher::ui::components {

class Avatar {
public:
    void setName(std::string name) { m_name = std::move(name); }
    void setRadius(f32 r) { m_radius = r; }
    void setOnline(bool on) { m_online = on; }

    // image_path 为空 → 占位绘制
    void setImagePath(std::string path) { m_image_path = std::move(path); }

    void draw(render::SkiaRenderer& r, render::FontManager& f,
              f32 cx, f32 cy);

private:
    std::string m_name{"User"};
    std::string m_image_path;
    f32  m_radius{14.0f};
    bool m_online{false};
    anim::AnimatedProperty<f32> m_hover_scale{1.0f};
};

}  // namespace launcher::ui::components
