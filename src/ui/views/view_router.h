#pragma once

// View 切换调度：跨 view fade-out → fade-in (250ms 总长，120ms overlap)。
// EventLoop 在主循环里调 tick + draw。

#include "app/common.h"
#include "ui/anim/animated_property.h"
#include "ui/views/view.h"
#include <memory>
#include <vector>
#include <unordered_map>

namespace launcher::ui::views {

enum class RouteId : u8 {
    Loading = 0,
    Login   = 1,
    Home    = 2,
    Library = 3,
    Cloud   = 4,
    Settings= 5,
};

class ViewRouter {
public:
    void registerView(RouteId id, std::unique_ptr<View> v);
    void navigateTo(RouteId id);

    void tick(f32 dt);
    void draw(render::SkiaRenderer& r, render::FontManager& f, Rect area);

    RouteId current() const { return m_current; }

private:
    std::unordered_map<RouteId, std::unique_ptr<View>> m_views;
    RouteId m_current{RouteId::Loading};
    RouteId m_next{RouteId::Loading};
    enum class Phase { Idle, FadingOut, FadingIn };
    Phase m_phase{Phase::Idle};
    anim::AnimatedProperty<f32> m_alpha{1.0f};
};

}  // namespace launcher::ui::views
