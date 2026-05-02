// Launcher 入口点。
// Phase 1：开 200x200 无边框窗口，画加载卡片，假装连接 1.5 秒后切到 Login 阶段（占位）。

#include "app/window.h"
#include "app/event_loop.h"
#include "ui/theme/theme_manager.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/rotating_file_sink.h>

namespace {

void initLogging() {
    auto console = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    auto file = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
        "launcher.log", 2 * 1024 * 1024, 4);
    auto logger = std::make_shared<spdlog::logger>(
        "launcher", spdlog::sinks_init_list{console, file});
    logger->set_pattern("%H:%M:%S.%e [%^%l%$] %v");
#ifdef NDEBUG
    logger->set_level(spdlog::level::info);
#else
    logger->set_level(spdlog::level::debug);
#endif
    spdlog::set_default_logger(logger);
}

int run() {
    initLogging();
    spdlog::info("Launcher start");

    launcher::theme::ThemeManager::instance().setMode(launcher::theme::Mode::System);

    launcher::app::Window window;
    launcher::app::WindowConfig cfg;
    cfg.width = 200;
    cfg.height = 200;
    cfg.decorated = false;
    cfg.always_on_top = false;
    cfg.title = "Launcher";

    if (auto st = window.create(cfg); !st.ok()) {
        spdlog::error("Window::create failed code={}", st.code);
        return 1;
    }

    launcher::app::EventLoop loop;
    if (auto st = loop.init(window); !st.ok()) {
        spdlog::error("EventLoop::init failed code={}", st.code);
        return 2;
    }

    loop.run(window);

    spdlog::info("Launcher exit clean");
    return 0;
}

}  // namespace

int APIENTRY wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    return run();
}

// 用 console 子系统调试时也能跑（Debug build）
int main() { return run(); }
