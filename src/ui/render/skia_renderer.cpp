#include "ui/render/skia_renderer.h"
#include "ui/render/font_manager.h"

// Skia 头：使用 aseprite/skia 预编译版（third_party/skia/include/）
#include "include/core/SkCanvas.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkData.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPath.h"
#include "include/core/SkPoint.h"
#include "include/core/SkRect.h"
#include "include/core/SkRRect.h"
#include "include/core/SkSurface.h"
#include "include/core/SkSurfaceProps.h"
#include "include/effects/SkImageFilters.h"
#include "include/gpu/ganesh/GrDirectContext.h"
#include "include/gpu/ganesh/SkSurfaceGanesh.h"
#include "include/gpu/ganesh/gl/GrGLDirectContext.h"
#include "include/gpu/ganesh/gl/GrGLBackendSurface.h"
#include "include/gpu/ganesh/gl/GrGLInterface.h"

#include <spdlog/spdlog.h>

namespace launcher::ui::render {

struct SkiaRenderer::Impl {
    sk_sp<const GrGLInterface> gl_iface;
    sk_sp<GrDirectContext>     gr_ctx;
    sk_sp<SkSurface>           surface;
};

SkiaRenderer::SkiaRenderer()  : m_impl(new Impl()) {}
SkiaRenderer::~SkiaRenderer() { shutdown(); delete m_impl; }

static SkColor to_sk(theme::Color c) {
    return static_cast<SkColor>(c.toSkColor());
}

Status SkiaRenderer::init(int fb_width, int fb_height) {
    m_impl->gl_iface = GrGLMakeNativeInterface();
    if (!m_impl->gl_iface) {
        spdlog::error("GrGLMakeNativeInterface failed");
        return Status::Err(1);
    }
    m_impl->gr_ctx = GrDirectContexts::MakeGL(m_impl->gl_iface);
    if (!m_impl->gr_ctx) {
        spdlog::error("GrDirectContexts::MakeGL failed");
        return Status::Err(2);
    }
    resize(fb_width, fb_height);
    return Status::Ok();
}

void SkiaRenderer::resize(int fb_width, int fb_height) {
    m_width = fb_width;
    m_height = fb_height;

    GrGLFramebufferInfo fb_info{};
    fb_info.fFBOID = 0;            // 默认帧缓冲（GLFW 给的窗口 FBO）
    fb_info.fFormat = 0x8058;       // GL_RGBA8

    auto rt = GrBackendRenderTargets::MakeGL(
        fb_width, fb_height,
        /*sample_count=*/1,
        /*stencil_bits=*/8,
        fb_info);

    SkSurfaceProps props(0, kUnknown_SkPixelGeometry);
    m_impl->surface = SkSurfaces::WrapBackendRenderTarget(
        m_impl->gr_ctx.get(),
        rt,
        kBottomLeft_GrSurfaceOrigin,
        kRGBA_8888_SkColorType,
        nullptr,
        &props);

    m_surface = m_impl->surface.get();
    m_canvas  = m_surface ? m_surface->getCanvas() : nullptr;
}

void SkiaRenderer::shutdown() {
    if (!m_impl) return;
    m_impl->surface.reset();
    if (m_impl->gr_ctx) m_impl->gr_ctx->abandonContext();
    m_impl->gr_ctx.reset();
    m_impl->gl_iface.reset();
    m_canvas = nullptr;
    m_surface = nullptr;
}

SkCanvas* SkiaRenderer::beginFrame(theme::Color clear_color) {
    if (!m_canvas) return nullptr;
    m_canvas->clear(to_sk(clear_color));
    return m_canvas;
}

void SkiaRenderer::endFrame() {
    if (m_impl && m_impl->gr_ctx) {
        m_impl->gr_ctx->flushAndSubmit();
    }
}

void SkiaRenderer::drawRoundRect(f32 x, f32 y, f32 w, f32 h, f32 radius,
                                 theme::Color fill, const ShadowSpec* shadow) {
    if (!m_canvas) return;
    SkRect rect = SkRect::MakeXYWH(x, y, w, h);
    SkRRect rr; rr.setRectXY(rect, radius, radius);

    if (shadow) {
        SkPaint sp;
        sp.setAntiAlias(true);
        sp.setColor(to_sk(shadow->color));
        sp.setImageFilter(SkImageFilters::Blur(shadow->blur, shadow->blur, nullptr));
        m_canvas->save();
        m_canvas->translate(shadow->dx, shadow->dy);
        m_canvas->drawRRect(rr, sp);
        m_canvas->restore();
    }
    SkPaint p;
    p.setAntiAlias(true);
    p.setColor(to_sk(fill));
    m_canvas->drawRRect(rr, p);
}

void SkiaRenderer::drawCircle(f32 cx, f32 cy, f32 r, theme::Color fill) {
    if (!m_canvas) return;
    SkPaint p;
    p.setAntiAlias(true);
    p.setColor(to_sk(fill));
    m_canvas->drawCircle(cx, cy, r, p);
}

void SkiaRenderer::drawArc(f32 cx, f32 cy, f32 r, f32 start_deg, f32 sweep_deg,
                           f32 stroke, theme::Color color) {
    if (!m_canvas) return;
    SkPaint p;
    p.setAntiAlias(true);
    p.setStyle(SkPaint::kStroke_Style);
    p.setStrokeWidth(stroke);
    p.setStrokeCap(SkPaint::kRound_Cap);
    p.setColor(to_sk(color));
    SkRect oval = SkRect::MakeXYWH(cx - r, cy - r, r * 2.0f, r * 2.0f);
    m_canvas->drawArc(oval, start_deg, sweep_deg, /*useCenter=*/false, p);
}

void SkiaRenderer::drawText(const std::string& utf8, f32 x, f32 y, f32 size,
                            theme::Color color, FontManager* fonts) {
    if (!m_canvas || !fonts) return;
    fonts->drawShapedText(m_canvas, utf8, x, y, size, color.toSkColor());
}

}  // namespace launcher::ui::render
