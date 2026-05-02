#pragma once

// 缓动曲线库。所有 UI 动画必须从这里取曲线，不允许内联手算或抄 magic 数。
//
// 命名约定：easeOut* 起始快尾段慢，easeIn* 起始慢尾段快，easeInOut* S 形对称。
// 入参 t ∈ [0, 1]，出参 ∈ [0, 1]（spring/back 可能短暂越界，正常）。

#include "app/common.h"
#include <cmath>

namespace launcher::ui::anim {

using Curve = f32(*)(f32);

namespace curves {

inline f32 linear(f32 t) { return t; }

// 项目主曲线 — Claude Desktop 同款 cubic-bezier(0.16, 1, 0.3, 1)
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

inline f32 easeOutExpo(f32 t) {
    return t >= 1.0f ? 1.0f : 1.0f - std::pow(2.0f, -10.0f * t);
}

// 入场轻微回弹
inline f32 easeOutBack(f32 t) {
    constexpr f32 c1 = 1.70158f;
    constexpr f32 c3 = c1 + 1.0f;
    f32 inv = t - 1.0f;
    return 1.0f + c3 * inv * inv * inv + c1 * inv * inv;
}

// 弹性（按钮 press release 之类）
inline f32 easeOutElastic(f32 t) {
    constexpr f32 c4 = 6.2831853f / 3.0f;
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    return std::pow(2.0f, -10.0f * t) * std::sin((t * 10.0f - 0.75f) * c4) + 1.0f;
}

// 类弹簧：critically damped 近似，0.6 处过冲再回正
inline f32 spring(f32 t) {
    if (t >= 1.0f) return 1.0f;
    f32 d = 0.5f;
    return 1.0f - std::exp(-t / d) * std::cos(t * 6.0f);
}

inline f32 easeInQuad(f32 t)  { return t * t; }
inline f32 easeOutQuad(f32 t) { f32 inv = 1.0f - t; return 1.0f - inv * inv; }

}  // namespace curves

// 时长单位常量：避免散落的 200ms / 0.2s 写法
constexpr f32 kDurFast    = 0.150f;
constexpr f32 kDurDefault = 0.200f;
constexpr f32 kDurSlow    = 0.350f;
constexpr f32 kDurBig     = 0.500f;   // 切换 view 的大动画

}  // namespace launcher::ui::anim
