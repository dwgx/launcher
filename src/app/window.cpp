#include "app/window.h"

#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <dwmapi.h>

#include <spdlog/spdlog.h>

namespace launcher::app {

Window::Window() = default;
Window::~Window() { destroy(); }

void Window::s_onFramebufferResize(GLFWwindow* w, int width, int height) {
    auto* self = static_cast<Window*>(::glfwGetWindowUserPointer(w));
    if (!self) return;
    self->m_fb_w = width;
    self->m_fb_h = height;
    self->m_renderer.resize(width, height);
}

Status Window::create(const WindowConfig& cfg) {
    if (!::glfwInit()) {
        spdlog::error("glfwInit failed");
        return Status::Err(1);
    }

    ::glfwWindowHint(GLFW_DECORATED, cfg.decorated ? GLFW_TRUE : GLFW_FALSE);
    ::glfwWindowHint(GLFW_TRANSPARENT_FRAMEBUFFER, cfg.transparent ? GLFW_TRUE : GLFW_FALSE);
    ::glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    ::glfwWindowHint(GLFW_FLOATING, cfg.always_on_top ? GLFW_TRUE : GLFW_FALSE);
    ::glfwWindowHint(GLFW_VISIBLE, GLFW_TRUE);
    ::glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    ::glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    ::glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    ::glfwWindowHint(GLFW_SAMPLES, 0);  // Skia 自己管 AA
    ::glfwWindowHint(GLFW_STENCIL_BITS, 8);

    m_glfw = ::glfwCreateWindow(cfg.width, cfg.height, cfg.title.c_str(),
                                nullptr, nullptr);
    if (!m_glfw) {
        spdlog::error("glfwCreateWindow failed");
        return Status::Err(2);
    }
    ::glfwSetWindowUserPointer(m_glfw, this);
    ::glfwSetFramebufferSizeCallback(m_glfw, &Window::s_onFramebufferResize);
    ::glfwMakeContextCurrent(m_glfw);
    ::glfwSwapInterval(1);  // vsync

    ::glfwGetFramebufferSize(m_glfw, &m_fb_w, &m_fb_h);

    f32 xs = 1.0f, ys = 1.0f;
    ::glfwGetWindowContentScale(m_glfw, &xs, &ys);
    m_dpi = xs;

    if (auto st = m_renderer.init(m_fb_w, m_fb_h); !st.ok()) {
        return st;
    }

    // Win11：开启 mica/round corner（无边框窗口也保留圆角）
    HWND hwnd = ::glfwGetWin32Window(m_glfw);
    if (hwnd) {
        DWM_WINDOW_CORNER_PREFERENCE pref = DWMWCP_ROUND;
        ::DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE,
                                &pref, sizeof(pref));
    }

    return Status::Ok();
}

void Window::destroy() {
    m_renderer.shutdown();
    if (m_glfw) {
        ::glfwDestroyWindow(m_glfw);
        m_glfw = nullptr;
        ::glfwTerminate();
    }
}

bool Window::shouldClose() const {
    return m_glfw && ::glfwWindowShouldClose(m_glfw);
}

void Window::pollEvents() { ::glfwPollEvents(); }
void Window::swapBuffers() { if (m_glfw) ::glfwSwapBuffers(m_glfw); }

int Window::framebufferWidth()  const { return m_fb_w; }
int Window::framebufferHeight() const { return m_fb_h; }
f32 Window::dpiScale()          const { return m_dpi;  }

}  // namespace launcher::app
