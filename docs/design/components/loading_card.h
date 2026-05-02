#pragma once

// 启动时 200x200 加载小卡片：圆角矩形 + 中央 Spinner + 下方 caption。
// 卡片状态由 LoadingView 推进，本组件只负责画。

#include "app/common.h"
#include "ui/components/spinner.h"

namespace launcher::ui::render { class SkiaRenderer; class FontManager; }

namespace launcher::ui::components {

struct LoadingCardState {
    std::string caption{"Connecting…"};
    f32 fade_in{0.0f};      // 0..1，启动时淡入
};

class LoadingCard {
public:
    void tick(f32 dt_seconds);
    void draw(render::SkiaRenderer& r, render::FontManager& fonts,
              const LoadingCardState& state);

private:
    Spinner m_spinner;
};

}  // namespace launcher::ui::components
