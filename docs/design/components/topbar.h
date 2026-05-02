#pragma once

// 32-48px 高顶栏：左 logo + 标题，右 username + Avatar。
// Phase 2 扩展：自绘红绿灯按钮 + WM_NCHITTEST 拖拽区。

#include "app/common.h"
#include "ui/anim/animated_property.h"
#include "ui/components/avatar.h"

namespace launcher::ui::render { class SkiaRenderer; class FontManager; }

namespace launcher::ui::components {

class Topbar {
public:
    void setUsername(std::string name);

    void enter();
    void tick(f32 dt);
    void draw(render::SkiaRenderer& r, render::FontManager& f,
              f32 width);

    f32 height() const { return kHeight; }

    Avatar& avatar() { return m_avatar; }

private:
    static constexpr f32 kHeight = 48.0f;
    std::string m_username;
    Avatar      m_avatar;
    anim::AnimatedProperty<f32> m_offset_y{-kHeight};
};

}  // namespace launcher::ui::components
