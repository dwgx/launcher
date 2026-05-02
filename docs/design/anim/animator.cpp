#include "ui/anim/animator.h"

namespace launcher::ui::anim {

Animator& Animator::instance() { static Animator s; return s; }

void Animator::schedule(Tickable t) {
    m_items.push_back(std::move(t));
}

void Animator::tick(f32 dt) {
    if (m_items.empty()) return;
    // Why: 边遍历边删除，用 swap-pop 避免迭代失效
    for (size_t i = 0; i < m_items.size();) {
        if (!m_items[i](dt)) {
            m_items[i] = std::move(m_items.back());
            m_items.pop_back();
        } else {
            ++i;
        }
    }
}

}  // namespace launcher::ui::anim
