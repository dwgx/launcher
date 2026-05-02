#pragma once

// 全局缓动函数库。组件动画必须从这里取曲线，禁止内联手算。

#include "app/common.h"
#include <cmath>

namespace launcher::theme {

// 项目主曲线：cubic-bezier(0.16, 1, 0.3, 1) 的近似
inline f32 easeOutQuint(f32 t) {
    f32 inv = 1.0f - t;
    return 1.0f - inv * inv * inv * inv * inv;
}

inline f32 easeOutCubic(f32 t) {
    f32 inv = 1.0f - t;
    return 1.0f - inv * inv * inv;
}

inline f32 easeInOutCubic(f32 t) {
    return t < 0.5f
        ? 4.0f * t * t * t
        : 1.0f - std::pow(-2.0f * t + 2.0f, 3.0f) * 0.5f;
}

inline f32 lerp(f32 a, f32 b, f32 t) { return a + (b - a) * t; }

inline f32 clamp01(f32 v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

constexpr f32 kDefaultDurationMs    = 200.0f;
constexpr f32 kFastDurationMs       = 150.0f;
constexpr f32 kThemeSwitchMs        = 200.0f;
constexpr f32 kScrollDamping        = 0.92f;

}  // namespace launcher::theme
