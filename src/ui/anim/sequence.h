#pragma once

// 串行 / 并行编排：用于"先淡入再上浮"这类组合动画，避免在组件里手写状态机。
//
// 用法：
//   Sequence s;
//   s.then(opacity.tweenTo(1.0f, 0.2f));     // 淡入 200ms
//   s.then(translate.tweenTo({0, 0}, 0.2f));  // 然后上浮
//   s.start();
//
// 暂时只支持 tween-of-f32 串行，复杂场景由 view 自管。

#include "app/common.h"
#include "ui/anim/tween.h"
#include <vector>
#include <functional>

namespace launcher::ui::anim {

class Sequence {
public:
    Sequence& then(Tween<f32> tw) { m_steps.push_back(std::move(tw)); return *this; }
    Sequence& delay(f32 sec)      { m_delay_after_step = sec; return *this; }
    Sequence& onComplete(std::function<void()> fn) {
        m_on_complete = std::move(fn);
        return *this;
    }

    void start() {
        m_index = 0;
        m_finished = m_steps.empty();
        if (!m_finished) m_steps[0].start();
        if (m_finished && m_on_complete) m_on_complete();
    }

    void tick(f32 dt) {
        if (m_finished) return;
        m_steps[m_index].tick(dt);
        if (m_steps[m_index].done()) {
            m_index++;
            if (m_index >= m_steps.size()) {
                m_finished = true;
                if (m_on_complete) m_on_complete();
            } else {
                m_steps[m_index].start();
            }
        }
    }

    f32 currentValue() const {
        if (m_finished || m_steps.empty()) return 0.0f;
        return m_steps[m_index].value();
    }
    int  currentIndex() const { return m_index; }
    bool finished() const { return m_finished; }

private:
    std::vector<Tween<f32>> m_steps;
    int m_index{0};
    bool m_finished{true};
    f32  m_delay_after_step{0.0f};
    std::function<void()> m_on_complete;
};

}  // namespace launcher::ui::anim
