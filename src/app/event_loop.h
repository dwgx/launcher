#pragma once

#include "app/common.h"
#include "app/window.h"
#include "ui/render/font_manager.h"
#include "ui/views/loading_view.h"

namespace launcher::app {

enum class AppPhase : u8 {
    Loading = 0,
    Login   = 1,
    Main    = 2,
};

class EventLoop {
public:
    Status init(Window& win);
    void   run(Window& win);

private:
    void renderLoading(Window& win, f32 dt);

    AppPhase m_phase{AppPhase::Loading};
    ui::render::FontManager m_fonts;
    ui::views::LoadingView  m_loading;
    f32 m_last_time{0.0f};
};

}  // namespace launcher::app
