#pragma once

// SkCanvas 高层封装：提供项目惯用的圆角矩形 / 阴影 / 文本 API。
// 业务层只用本类，禁止直接使用 SkCanvas 接口（保留 canvas() 仅供桥接 Clay 渲染指令）。

#include "app/common.h"
#include "ui/theme/color_tokens.h"

class SkCanvas;
class SkSurface;
class GrDirectContext;

namespace launcher::ui::render {

class FontManager;

struct ShadowSpec {
    f32 dx{0.0f};
    f32 dy{1.0f};
    f32 blur{3.0f};
    theme::Color color{0, 0, 0, 10};
};

class SkiaRenderer {
public:
    SkiaRenderer();
    ~SkiaRenderer();
    LAUNCHER_DISALLOW_COPY(SkiaRenderer);

    // GLFW 已 makeContextCurrent 后调用一次
    Status init(int fb_width, int fb_height);
    void resize(int fb_width, int fb_height);
    void shutdown();

    // 帧开始：清屏并返回 canvas；帧结束：flush + GLFW swap 由调用方处理
    SkCanvas* beginFrame(theme::Color clear_color);
    void endFrame();

    // 高层绘制
    void drawRoundRect(f32 x, f32 y, f32 w, f32 h, f32 radius,
                       theme::Color fill, const ShadowSpec* shadow = nullptr);

    void drawCircle(f32 cx, f32 cy, f32 r, theme::Color fill);

    // angle 单位：度；start_angle 起始角；sweep_angle 跨度；stroke 线宽
    void drawArc(f32 cx, f32 cy, f32 r, f32 start_angle, f32 sweep_angle,
                 f32 stroke, theme::Color color);

    void drawText(const std::string& utf8, f32 x, f32 y, f32 size,
                  theme::Color color, FontManager* fonts);

    int width() const  { return m_width; }
    int height() const { return m_height; }
    SkCanvas* canvas() const { return m_canvas; }

private:
    struct Impl;
    Impl* m_impl{nullptr};

    int m_width{0};
    int m_height{0};
    SkSurface* m_surface{nullptr};   // not owning; owned by Impl
    SkCanvas*  m_canvas{nullptr};    // not owning
};

}  // namespace launcher::ui::render
