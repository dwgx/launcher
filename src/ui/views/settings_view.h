#pragma once

// 设置：语言切换 (en/zh-CN/ja-JP) + 主题 (light/dark/system) + 关于。
// 切换语言会广播到 i18n::Locale 的 listener，所有 view 重绘。

#include "app/common.h"
#include "ui/anim/animated_property.h"
#include "ui/i18n/locale.h"
#include "ui/theme/theme_manager.h"
#include "ui/views/view.h"

namespace launcher::ui::views {

class SettingsView : public View {
public:
    void onEnter() override;
    void tick(f32 dt) override;
    void draw(render::SkiaRenderer& r, render::FontManager& f, Rect area) override;

    void onMouseMove(f32 x, f32 y, Rect area) override;
    bool onClick(f32 x, f32 y, Rect area) override;

private:
    int m_hover_lang{-1};
    int m_hover_theme{-1};
    anim::AnimatedProperty<f32> m_opacity{0.0f};
};

}  // namespace launcher::ui::views
