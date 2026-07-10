// D2DApp — Phase 2.1 金标准 pipeline 封装
//   WS_EX_NOREDIRECTIONBITMAP + D3D11 (BGRA) + DXGI flip-discard + waitable
//   + DComp visual + ID2D1DeviceContext (零拷贝同 D3D 后端)
//
// 业务调用顺序（每帧）：
//   waitFrame(timeout) → drain WM → beginFrame() → 渲染 → endFrame()
//
// 详细背景见 docs/PHASE_2_D2D_MIGRATION.md。参考实现 tools/preview-skia/skia_d3d.cpp。
#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
// 必须在 d2d1.h 之前 #undef，否则 ID2D1RenderTarget::DrawText 方法名
// 会被 Windows.h 的 #define DrawText DrawTextW 宏替换成 DrawTextW，
// 业务侧 ctx->DrawText(...) 就找不到方法。
#ifdef DrawText
#undef DrawText
#endif
#include <d3d11.h>
#include <d2d1_1.h>
#include <d2d1.h>
#include <dxgi1_3.h>
#include <dcomp.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <cstdint>

#include "render/brush_cache.h"
#include "render/text_cache.h"
#include "render/stroke_cache.h"
#include "render/image_cache.h"
#include "render/gif_cache.h"
#include "render/emoji_cache.h"

namespace launcher::d2d {

using Microsoft::WRL::ComPtr;

class D2DApp {
public:
    // 在 ShowWindow 之前调；hwnd 必须以 WS_EX_NOREDIRECTIONBITMAP 创建。
    bool init(HWND hwnd, int physical_w, int physical_h);
    void shutdown();

    // WM_SIZE 时调；resize 实际发生在下一次 beginFrame() 帧首
    // (DXGI ResizeBuffers 必须在所有 back-buffer ID2D1Bitmap 释放后)。
    void requestResize(int physical_w, int physical_h);

    // 等 swap chain waitable（vsync 同步）；返回 WAIT_OBJECT_0 表示能画。
    DWORD waitFrame(DWORD timeout_ms = 100);

    // bind back buffer 为 RT + BeginDraw；device lost 返 false（调用方应跳过本帧）。
    bool beginFrame();

    // EndDraw + 释放 RT 引用 + Present。device lost 时不 throw，业务自行处理。
    void endFrame();

    // 资源访问 — 视图代码持有 D2DApp& 后通过这些 accessor 拿 device / cache。
    ID2D1DeviceContext* ctx()     const { return d2d_ctx_.Get(); }
    ID2D1Factory1*      factory() const { return d2d_factory_.Get(); }
    IWICImagingFactory* wic()     const { return wic_.Get(); }
    HWND                hwnd()    const { return hwnd_; }

    // DIP 大小（逻辑布局像素）— D2D RT 自动 dpi-scale 到物理。
    float widthDip()  const { return width_  * 96.0f / dpi_; }
    float heightDip() const { return height_ * 96.0f / dpi_; }
    float dpi() const { return dpi_; }

    BrushCache&  brushes()  { return brushes_; }
    TextCache&   texts()    { return texts_; }
    StrokeCache& strokes()  { return strokes_; }
    ImageCache&  images()   { return images_; }
    GifCache&    gifs()     { return gifs_; }
    EmojiCache&  emojis()   { return emojis_; }

private:
    void initD3D();
    void initSwapChain();
    void initD2D();
    void initDComp();
    void initDWrite();
    void initWIC();
    void doResize();

    HWND  hwnd_{};
    int   width_{};
    int   height_{};
    float dpi_{96.0f};

    ComPtr<ID3D11Device>          d3d_device_;
    ComPtr<ID3D11DeviceContext>   d3d_ctx_;
    ComPtr<IDXGISwapChain1>       swap_;
    ComPtr<IDXGISwapChain2>       swap2_;
    HANDLE                        frame_waitable_{};
    bool                          tearing_supported_{};

    ComPtr<IDCompositionDevice>   dcomp_;
    ComPtr<IDCompositionTarget>   dcomp_target_;
    ComPtr<IDCompositionVisual>   dcomp_visual_;

    ComPtr<ID2D1Factory1>         d2d_factory_;
    ComPtr<ID2D1Device>           d2d_device_;
    ComPtr<ID2D1DeviceContext>    d2d_ctx_;

    ComPtr<IDWriteFactory>        dwrite_;
    ComPtr<IWICImagingFactory>    wic_;

    // 当前帧 back buffer 的 D2D wrapper。endFrame 必须 SetTarget(nullptr) 让 Present 能 flip。
    ComPtr<ID2D1Bitmap1>          back_buffer_;

    BrushCache   brushes_;
    TextCache    texts_;
    StrokeCache  strokes_;
    ImageCache   images_;
    GifCache     gifs_;
    EmojiCache   emojis_;

    int  pending_w_{};
    int  pending_h_{};
    bool resize_pending_{};
    bool initialized_{};
};

// hex 颜色转 D2D ColorF
//   argbToColorF(0x80FF8800) → r=1, g=0.53, b=0, a=0.5
inline D2D1_COLOR_F argbToColorF(uint32_t argb) {
    return D2D1::ColorF(
        ((argb >> 16) & 0xFFu) / 255.0f,
        ((argb >>  8) & 0xFFu) / 255.0f,
        ( argb        & 0xFFu) / 255.0f,
        ((argb >> 24) & 0xFFu) / 255.0f);
}
// pt → DIP (D2D 单位)；GDI+ UnitPoint 也是 96/72 换算
inline float ptToDip(float pt) { return pt * 96.0f / 72.0f; }

}  // namespace launcher::d2d
