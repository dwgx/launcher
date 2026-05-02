#pragma once

// 自动管理 Tween 的属性容器：UI 组件直接定义
//   AnimatedProperty<f32> opacity{0.0f};
//   opacity.animateTo(1.0f, kDurDefault);
// Animator::tick() 会在每帧自动推进所有注册过的 property。

#include "app/common.h"
#include "ui/anim/tween.h"
#include <optional>

namespace launcher::ui::anim {

template <typename T>
class AnimatedProperty {
public:
    AnimatedProperty() = default;
    explicit AnimatedProperty(T initial) : m_value(initial) {}

    void set(T v) {
        m_value = v;
        m_tween.reset();
    }

    void animateTo(T target, f32 duration = kDurDefault,
                   Curve c = curves::easeOutQuint, f32 delay_sec = 0.0f) {
        if (duration <= 0.0f) { set(target); return; }
        Tween<T> tw(m_value, target, duration, c);
        if (delay_sec > 0.0f) tw.delay(delay_sec);
        tw.start();
        m_tween = std::move(tw);
    }

    // 立刻取消正在跑的 tween，保留当前值
    void freeze() { m_tween.reset(); }

    void tick(f32 dt) {
        if (!m_tween) return;
        m_tween->tick(dt);
        m_value = m_tween->value();
        if (m_tween->done()) m_tween.reset();
    }

    T get() const { return m_value; }
    operator T() const { return m_value; }
    bool animating() const { return m_tween.has_value(); }

private:
    T                  m_value{};
    std::optional<Tween<T>> m_tween;
};

}  // namespace launcher::ui::anim
