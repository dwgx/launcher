// 高频渲染 helper — 钳圆角、矩形 / 椭圆 / 文字一次过。
//
// fillRR 强制 r ≤ min(w,h)/2 — GDI+ buildRoundRect pill r=999 的崩溃在 D2D 也得防。
//
// drawText_ 后缀下划线规避 user32.h DrawText 宏。
#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
// 见 d2d_app.h 同位置注释 — 必须在 d2d 头之前 #undef
#ifdef DrawText
#undef DrawText
#endif
#include <d2d1_1.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <string_view>
#include <algorithm>
#include <cstdint>

#include "brush_cache.h"

namespace launcher::d2d::prim {

inline D2D1_RECT_F xywh(float x, float y, float w, float h) {
    return D2D1::RectF(x, y, x + w, y + h);
}

inline void fillRR(ID2D1DeviceContext* ctx, float x, float y, float w, float h,
                   float r, ID2D1Brush* b) {
    if (!b) return;
    float rmax = (std::min)(w, h) * 0.5f;
    if (r > rmax) r = rmax;
    if (r < 0.0f) r = 0.0f;
    D2D1_ROUNDED_RECT rr{ xywh(x, y, w, h), r, r };
    ctx->FillRoundedRectangle(rr, b);
}

inline void strokeRR(ID2D1DeviceContext* ctx, float x, float y, float w, float h,
                     float r, ID2D1Brush* b, float thick = 1.0f,
                     ID2D1StrokeStyle* style = nullptr) {
    if (!b) return;
    float rmax = (std::min)(w, h) * 0.5f;
    if (r > rmax) r = rmax;
    if (r < 0.0f) r = 0.0f;
    D2D1_ROUNDED_RECT rr{ xywh(x, y, w, h), r, r };
    ctx->DrawRoundedRectangle(rr, b, thick, style);
}

inline void fillRect(ID2D1DeviceContext* ctx, float x, float y, float w, float h,
                     ID2D1Brush* b) {
    if (!b) return;
    ctx->FillRectangle(xywh(x, y, w, h), b);
}

inline void strokeRect(ID2D1DeviceContext* ctx, float x, float y, float w, float h,
                       ID2D1Brush* b, float thick = 1.0f,
                       ID2D1StrokeStyle* style = nullptr) {
    if (!b) return;
    ctx->DrawRectangle(xywh(x, y, w, h), b, thick, style);
}

inline void fillCircle(ID2D1DeviceContext* ctx, float cx, float cy, float r,
                       ID2D1Brush* b) {
    if (!b) return;
    ctx->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), b);
}

inline void strokeCircle(ID2D1DeviceContext* ctx, float cx, float cy, float r,
                         ID2D1Brush* b, float thick = 1.0f,
                         ID2D1StrokeStyle* style = nullptr) {
    if (!b) return;
    ctx->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), b, thick, style);
}

inline void drawText_(ID2D1DeviceContext* ctx, std::wstring_view text,
                      IDWriteTextFormat* fmt, float x, float y, float w, float h,
                      ID2D1Brush* b,
                      DWRITE_TEXT_ALIGNMENT halign = DWRITE_TEXT_ALIGNMENT_LEADING,
                      DWRITE_PARAGRAPH_ALIGNMENT valign = DWRITE_PARAGRAPH_ALIGNMENT_NEAR) {
    if (!fmt || !b || text.empty()) return;
    fmt->SetTextAlignment(halign);
    fmt->SetParagraphAlignment(valign);
    ctx->DrawText(text.data(), (UINT32)text.size(), fmt,
                  xywh(x, y, w, h), b,
                  D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT,
                  DWRITE_MEASURING_MODE_NATURAL);
}

// 单行文本 —— IDWriteTextFormat 默认 WORD_WRAPPING_WRAP，任何宽度不足的
// 按钮/标签/徽章在长文案（英/日）下会折到第二行被裁掉。UI chrome（非正文）
// 一律走这个：临时关掉换行，画完恢复（format 对象是 TextCache 共享缓存的）。
inline void drawTextNoWrap(ID2D1DeviceContext* ctx, std::wstring_view text,
                           IDWriteTextFormat* fmt, float x, float y, float w, float h,
                           ID2D1Brush* b,
                           DWRITE_TEXT_ALIGNMENT halign = DWRITE_TEXT_ALIGNMENT_LEADING,
                           DWRITE_PARAGRAPH_ALIGNMENT valign = DWRITE_PARAGRAPH_ALIGNMENT_NEAR) {
    if (!fmt || !b || text.empty()) return;
    DWRITE_WORD_WRAPPING old_wrap = fmt->GetWordWrapping();
    fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    drawText_(ctx, text, fmt, x, y, w, h, b, halign, valign);
    fmt->SetWordWrapping(old_wrap);
}

inline void drawLine(ID2D1DeviceContext* ctx, float x1, float y1, float x2, float y2,
                     ID2D1Brush* b, float thick = 1.0f,
                     ID2D1StrokeStyle* style = nullptr) {
    if (!b) return;
    ctx->DrawLine(D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2), b, thick, style);
}

// 圆角矩形裁剪 mask — 用 ID2D1RoundedRectangleGeometry + PushLayer，比
// PushAxisAlignedClip 多了真圆角 (PushAxisAlignedClip 只能矩形 4 直角)。
// 用法：
//   pushLayerRR(ctx, factory, x, y, w, h, 12);
//   ctx->DrawBitmap(bmp, dest, 1.0f, ...);   // bitmap 自动按圆角裁
//   popLayer(ctx);
inline void pushLayerRR(ID2D1DeviceContext* ctx, ID2D1Factory1* factory,
                        float x, float y, float w, float h, float r) {
    if (!ctx || !factory) return;
    float rmax = (std::min)(w, h) * 0.5f;
    if (r > rmax) r = rmax;
    if (r < 0.0f) r = 0.0f;
    Microsoft::WRL::ComPtr<ID2D1RoundedRectangleGeometry> geo;
    factory->CreateRoundedRectangleGeometry(
        D2D1::RoundedRect(D2D1::RectF(x, y, x + w, y + h), r, r), &geo);
    if (!geo) return;
    D2D1_LAYER_PARAMETERS lp = D2D1::LayerParameters(
        D2D1::InfiniteRect(),
        geo.Get(),
        D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
        D2D1::IdentityMatrix(),
        1.0f, nullptr, D2D1_LAYER_OPTIONS_NONE);
    ctx->PushLayer(lp, nullptr);
}
inline void popLayer(ID2D1DeviceContext* ctx) { if (ctx) ctx->PopLayer(); }

// 6 层假高斯阴影 — 跟 GDI+ Preview drawShadow 同款。D2D 1.1 Effects 真高斯
// 留 Phase 2.2 优化（需 off-screen bitmap target）。
//
// shadow_argb 的 alpha 是基底强度；opacity 在帧级 fade in/out 用。
inline void drawShadow(ID2D1DeviceContext* ctx, BrushCache& brushes,
                       float x, float y, float w, float h, float r,
                       uint32_t shadow_argb, float opacity = 1.0f,
                       float base_spread = 4.0f, int layers = 4) {
    uint32_t base_a = (shadow_argb >> 24) & 0xFFu;
    uint32_t rgb = shadow_argb & 0xFFFFFFu;
    float base_alpha = (base_a / 255.0f) * opacity;
    if (base_alpha < 1.0f / 255.0f) return;
    for (int i = 0; i < layers; ++i) {
        float spread_i = base_spread + i * 1.6f;
        float a = base_alpha * 0.7f / (float)(i + 1);
        fillRR(ctx,
               x - spread_i,
               y - spread_i + 2.0f,
               w + spread_i * 2.0f,
               h + spread_i * 2.0f,
               r + spread_i,
               brushes.solidA(rgb, a));
    }
}

}  // namespace launcher::d2d::prim
