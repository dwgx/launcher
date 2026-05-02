#pragma once

// 无边框 GLFW 窗口 + Skia GL 后端。
// Phase 1 用 200×200 固定大小，Phase 2 切到 1100×720 主窗口并启用自绘标题栏。

#include "app/common.h"
#include "ui/render/skia_renderer.h"
#include <functional>

struct GLFWwindow;

namespace launcher::app {

struct WindowConfig {
    int width{200};
    int height{200};
    std::string title{"Launcher"};
    bool decorated{false};
    bool transparent{false};
    bool always_on_top{false};
};

class Window {
public:
    Window();
    ~Window();
    LAUNCHER_DISALLOW_COPY(Window);

    Status create(const WindowConfig& cfg);
    void destroy();

    bool shouldClose() const;
    void pollEvents();
    void swapBuffers();

    int  framebufferWidth()  const;
    int  framebufferHeight() const;
    f32  dpiScale() const;

    GLFWwindow*               raw()      { return m_glfw; }
    ui::render::SkiaRenderer& renderer() { return m_renderer; }

    // Phase 2 会接 hit-test 回调；目前留空
    using HitTestFn = std::function<int(int x, int y)>;
    void setHitTest(HitTestFn fn) { m_hit_test = std::move(fn); }

private:
    GLFWwindow*              m_glfw{nullptr};
    ui::render::SkiaRenderer m_renderer;
    HitTestFn                m_hit_test;
    int  m_fb_w{0};
    int  m_fb_h{0};
    f32  m_dpi{1.0f};

    static void s_onFramebufferResize(GLFWwindow* w, int width, int height);
};

}  // namespace launcher::app
