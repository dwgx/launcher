#pragma once

// 单组件 Spinner：逐帧旋转的弧形（不是补间动画）。
// Why: 加载占位用，需要持续旋转直到外部告知加载完成；用 angle 累计 + dt 简单可控。

#include "app/common.h"
#include "ui/theme/color_tokens.h"

namespace launcher::ui::render { class SkiaRenderer; }

namespace launcher::ui::components {

class Spinner {
public:
    void tick(f32 dt_seconds);
    void draw(render::SkiaRenderer& r,
              f32 cx, f32 cy, f32 radius, f32 stroke,
              theme::Color color);

private:
    f32 m_angle{0.0f};        // 度
    static constexpr f32 kRotPerSec = 320.0f;   // 一秒约 0.9 圈
    static constexpr f32 kSweepDeg  = 80.0f;
};

}  // namespace launcher::ui::components
