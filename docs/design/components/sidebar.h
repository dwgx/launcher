#pragma once

// 左侧 200px 侧栏：logo + 5 个菜单项 + 底部 logout。
// 入场动画：slide-in from -200px → 0，450ms easeOutQuint。

#include "app/common.h"
#include "ui/anim/animated_property.h"
#include "ui/components/menu_item.h"
#include <vector>
#include <functional>

namespace launcher::ui::render { class SkiaRenderer; class FontManager; }

namespace launcher::ui::components {

enum class NavTarget : u8 { Home = 0, Library = 1, Cloud = 2, Settings = 3, Logout = 4 };

class Sidebar {
public:
    using OnNavFn = std::function<void(NavTarget)>;

    Sidebar();
    void setOnNavigate(OnNavFn fn) { m_on_nav = std::move(fn); }
    void setActive(NavTarget t);

    void enter();   // 触发入场 slide-in
    void exit();    // 退场（用于 logout 切换）

    void tick(f32 dt);
    void draw(render::SkiaRenderer& r, render::FontManager& f,
              f32 top_y, f32 height);

    void onMouseMove(f32 x, f32 y);
    bool onClick(f32 x, f32 y);

    f32 width() const { return kWidth; }

private:
    static constexpr f32 kWidth = 200.0f;
    NavTarget                m_active{NavTarget::Home};
    std::vector<MenuItem>    m_items;
    MenuItem                 m_logout_item;
    OnNavFn                  m_on_nav;
    anim::AnimatedProperty<f32> m_offset_x{-kWidth};
};

}  // namespace launcher::ui::components
