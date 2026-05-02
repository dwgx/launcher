#include "app/event_loop.h"
#include "ui/theme/theme_manager.h"

#include <GLFW/glfw3.h>
#include <spdlog/spdlog.h>

namespace launcher::app {

Status EventLoop::init(Window& win) {
    auto st = m_fonts.loadFromDirectory("assets/fonts");
    if (!st.ok()) {
        spdlog::warn("Font load returned {}, continuing with system fallback", st.code);
    }
    m_loading.setOnReady([this]() {
        spdlog::info("Loading -> Login");
        m_phase = AppPhase::Login;
        // Phase 2: 切到 1100x720 大窗 + 显示 LoginView
    });
    m_last_time = static_cast<f32>(::glfwGetTime());
    (void)win;
    return Status::Ok();
}

void EventLoop::renderLoading(Window& win, f32 dt) {
    auto& r = win.renderer();
    const auto pal = theme::ThemeManager::instance().palette();
    r.beginFrame(pal.bg);
    m_loading.tick(dt);
    m_loading.draw(r, m_fonts);
    r.endFrame();
}

void EventLoop::run(Window& win) {
    while (!win.shouldClose()) {
        f32 now = static_cast<f32>(::glfwGetTime());
        f32 dt  = now - m_last_time;
        m_last_time = now;
        if (dt > 0.1f) dt = 0.1f;  // 别让卡顿后一次跳太大

        win.pollEvents();

        theme::ThemeManager::instance().tick(dt);

        switch (m_phase) {
            case AppPhase::Loading: renderLoading(win, dt); break;
            case AppPhase::Login:
            case AppPhase::Main:
                // Phase 1：暂时停在 loading 视图，但 phase 已切换可在日志看到
                renderLoading(win, dt);
                break;
        }
        win.swapBuffers();
    }
}

}  // namespace launcher::app
