#pragma once

// 类型安全的 lerp。Tween<T> 调用 lerp<T> 求中间值；
// 用户自定义类型如要走动画，特化 lerp<MyType>。

#include "app/common.h"
#include "ui/theme/color_tokens.h"

namespace launcher::ui::anim {

template <typename T>
inline T lerp(const T& a, const T& b, f32 t);

template <> inline f32 lerp<f32>(const f32& a, const f32& b, f32 t) {
    return a + (b - a) * t;
}
template <> inline f64 lerp<f64>(const f64& a, const f64& b, f32 t) {
    return a + (b - a) * static_cast<f64>(t);
}
template <> inline i32 lerp<i32>(const i32& a, const i32& b, f32 t) {
    return static_cast<i32>(a + (b - a) * t + 0.5f);
}

template <> inline theme::Color lerp<theme::Color>(
    const theme::Color& a, const theme::Color& b, f32 t) {
    auto blend = [t](u8 x, u8 y) {
        return static_cast<u8>(x + (y - x) * t + 0.5f);
    };
    return { blend(a.r, b.r), blend(a.g, b.g), blend(a.b, b.b), blend(a.a, b.a) };
}

struct Vec2 {
    f32 x{0.0f}, y{0.0f};
    constexpr Vec2() = default;
    constexpr Vec2(f32 ax, f32 ay) : x(ax), y(ay) {}
};

template <> inline Vec2 lerp<Vec2>(const Vec2& a, const Vec2& b, f32 t) {
    return { a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t };
}

}  // namespace launcher::ui::anim
