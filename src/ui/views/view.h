#pragma once

// 所有 view 公共基类。
// View 不持有渲染器引用，由 Router 在 draw() 时传入。
// onEnter/onExit 用于切换时启动/取消动画。

#include "app/common.h"

namespace launcher::ui::render { class SkiaRenderer; class FontManager; }

namespace launcher::ui::views {

struct Rect { f32 x, y, w, h; };

class View {
public:
    virtual ~View() = default;
    virtual void onEnter() {}
    virtual void onExit() {}
    virtual void tick(f32 dt) = 0;
    virtual void draw(render::SkiaRenderer& r, render::FontManager& f, Rect area) = 0;

    // 鼠标事件转发；返回 true 表示 view 消费了事件
    virtual void onMouseMove(f32 x, f32 y, Rect area) { (void)x; (void)y; (void)area; }
    virtual bool onClick(f32 x, f32 y, Rect area) {
        (void)x; (void)y; (void)area; return false;
    }
};

}  // namespace launcher::ui::views
