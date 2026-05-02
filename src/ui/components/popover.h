#pragma once

// 通用 Popover/Dropdown 组件。
// design: card bg + radius 12, scale .96 → 1 + translateY(-6) → 0, 240ms ease-main.
// 用法：UserMenu / HeaderMenu / 频道右键 / 头像右键 等都共用。

#include "app/common.h"
#include "ui/anim/animated_property.h"

#include <functional>
#include <string>
#include <vector>

namespace launcher::ui::components {

struct PopoverItem {
    std::string label;
    std::string glyph;        // SVG 名 / unicode
    bool        danger{false};
    bool        is_divider{false};
    bool        checked{false};
    std::function<void()> on_click;
};

enum class PopoverAlign { TopRight, BottomRight, BottomLeft };

struct Popover {
    std::vector<PopoverItem> items;
    PopoverAlign align{PopoverAlign::BottomRight};
    bool open{false};
    anim::AnimatedProperty<f32> t{0.0f};
    f32 origin_x{0}, origin_y{0};
    f32 min_width{220.0f};

    void show(f32 ox, f32 oy);
    void hide();
};

}  // namespace launcher::ui::components
