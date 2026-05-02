#pragma once

// 云端管理：存档/配置文件备份。Phase 7 接通后端 /api/cloud/list 后填实际内容。

#include "app/common.h"
#include "ui/anim/animated_property.h"
#include "ui/views/view.h"

namespace launcher::ui::views {

class CloudView : public View {
public:
    void onEnter() override;
    void tick(f32 dt) override;
    void draw(render::SkiaRenderer& r, render::FontManager& f, Rect area) override;

private:
    anim::AnimatedProperty<f32> m_opacity{0.0f};
};

}  // namespace launcher::ui::views
