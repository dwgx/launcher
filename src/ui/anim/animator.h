#pragma once

// 全局 Animator：注册自定义 tickable 对象（如 Sequence 实例）由它统一推进。
// AnimatedProperty 不需要注册，组件自己持有并 tick。Animator 主要服务于"游离的"
// 动画——比如 toast 显示、view 切换的 sequence。

#include "app/common.h"
#include <functional>
#include <vector>

namespace launcher::ui::anim {

using Tickable = std::function<bool(f32)>;   // return true => keep alive

class Animator {
public:
    static Animator& instance();

    // 注册一个每帧驱动的回调；回调返回 false 时移除
    void schedule(Tickable t);

    void tick(f32 dt);

    size_t pending() const { return m_items.size(); }

private:
    Animator() = default;
    LAUNCHER_DISALLOW_COPY(Animator);
    std::vector<Tickable> m_items;
};

}  // namespace launcher::ui::anim
