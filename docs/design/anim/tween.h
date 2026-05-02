#pragma once

// 单段插值动画。链式 API：
//   tween.from(0).to(1).over(0.2f).curve(curves::easeOutQuint).start();
//   每帧调 tick(dt)，value() 取当前值，done() 判完成。

#include "app/common.h"
#include "ui/anim/curve.h"
#include "ui/anim/lerp.h"
#include <functional>

namespace launcher::ui::anim {

template <typename T>
class Tween {
public:
    Tween() = default;
    Tween(T from_v, T to_v, f32 duration, Curve c = curves::easeOutQuint)
        : m_from(from_v), m_to(to_v), m_duration(duration), m_curve(c) {}

    Tween& from(T v)         { m_from = v; return *this; }
    Tween& to(T v)           { m_to = v; return *this; }
    Tween& over(f32 sec)     { m_duration = sec; return *this; }
    Tween& curve(Curve c)    { m_curve = c; return *this; }
    Tween& delay(f32 sec)    { m_delay = sec; return *this; }
    Tween& onDone(std::function<void()> fn) { m_on_done = std::move(fn); return *this; }

    void start() {
        m_elapsed = -m_delay;
        m_done = false;
        m_started = true;
    }
    void cancel() { m_done = true; m_started = false; }
    bool started() const { return m_started; }
    bool done() const { return m_done; }

    void tick(f32 dt) {
        if (!m_started || m_done) return;
        m_elapsed += dt;
        if (m_elapsed < 0.0f) return;             // 还在 delay
        if (m_duration <= 0.0f || m_elapsed >= m_duration) {
            m_elapsed = m_duration;
            m_done = true;
            if (m_on_done) m_on_done();
        }
    }

    T value() const {
        if (!m_started || m_elapsed <= 0.0f || m_duration <= 0.0f) return m_from;
        if (m_done) return m_to;
        f32 t = m_elapsed / m_duration;
        if (t > 1.0f) t = 1.0f;
        return lerp<T>(m_from, m_to, m_curve(t));
    }

    f32 progress() const {
        if (m_duration <= 0.0f || !m_started) return 0.0f;
        f32 e = m_elapsed > 0.0f ? m_elapsed : 0.0f;
        f32 p = e / m_duration;
        return p > 1.0f ? 1.0f : p;
    }

private:
    T     m_from{}, m_to{};
    f32   m_duration{kDurDefault};
    f32   m_delay{0.0f};
    f32   m_elapsed{0.0f};
    Curve m_curve{curves::easeOutQuint};
    bool  m_started{false};
    bool  m_done{false};
    std::function<void()> m_on_done;
};

}  // namespace launcher::ui::anim
