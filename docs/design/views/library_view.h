#pragma once

// 游戏库：4 列卡片网格，每张 240×140 圆角 12，hover 上浮 2px + 阴影加深。
// 入场动画：每张卡片 stagger 30ms 浮入。

#include "app/common.h"
#include "ui/anim/animated_property.h"
#include "ui/views/view.h"
#include <vector>
#include <functional>

namespace launcher::ui::views {

struct GameItem {
    std::string game_id;
    std::string display_name;
    std::string version;
    std::string cover_url;        // CDN 上的封面图，本地 cache 后填路径
    bool        installed{false};
    bool        outdated{false};
};

class LibraryView : public View {
public:
    using OnLaunchFn = std::function<void(const std::string&)>;

    void setItems(std::vector<GameItem> items);
    void setOnLaunch(OnLaunchFn fn) { m_on_launch = std::move(fn); }

    void onEnter() override;
    void tick(f32 dt) override;
    void draw(render::SkiaRenderer& r, render::FontManager& f, Rect area) override;

    void onMouseMove(f32 x, f32 y, Rect area) override;
    bool onClick(f32 x, f32 y, Rect area) override;

private:
    std::vector<GameItem> m_items;
    int m_hover_index{-1};
    OnLaunchFn m_on_launch;
    anim::AnimatedProperty<f32> m_grid_opacity{0.0f};
};

}  // namespace launcher::ui::views
