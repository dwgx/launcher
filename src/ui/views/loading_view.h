#pragma once

// LoadingView：组合 LoadingCard + 后端连通性探测。
// 当后端心跳成功且本地存储就绪后，触发 onReady 回调（由 App 切到登录页）。

#include "app/common.h"
#include "ui/components/loading_card.h"
#include <functional>

namespace launcher::ui::render { class SkiaRenderer; class FontManager; }

namespace launcher::ui::views {

class LoadingView {
public:
    using OnReadyFn = std::function<void()>;

    void setOnReady(OnReadyFn fn) { m_on_ready = std::move(fn); }

    // 由 App 主循环驱动
    void tick(f32 dt_seconds);
    void draw(render::SkiaRenderer& r, render::FontManager& fonts);

    // 外部可强制完成（debug 用）
    void markReady();
    bool isReady() const { return m_ready; }

private:
    components::LoadingCard m_card;
    components::LoadingCardState m_state;
    f32  m_elapsed{0.0f};
    bool m_ready{false};
    OnReadyFn m_on_ready;

    // Phase 1：先用固定 1.5 秒模拟"连接后台"，Phase 7 接真实心跳
    static constexpr f32 kFakeDelaySec = 1.5f;
};

}  // namespace launcher::ui::views
