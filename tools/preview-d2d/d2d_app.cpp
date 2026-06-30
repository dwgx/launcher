// D2DApp 实现 — 见 d2d_app.h 头部说明。
//
// 关键 invariant:
//   1. NOREDIRECT swap chain → 不能用 GDI 画客户区，PrintWindow 截图全黑（预期）
//   2. ResizeBuffers 前必须 SetTarget(nullptr) + 释放 back_buffer_，否则 E_INVALIDARG
//   3. 帧末必须 SetTarget(nullptr) 让 Present 能 flip（FLIP_DISCARD 不允许 RT 持有 buffer[0]）
//   4. waitable + Present(0, ALLOW_TEARING) 是节奏唯一来源；不要 Sleep / DwmFlush

#include "d2d_app.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dcomp.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "ole32.lib")

namespace launcher::d2d {

bool D2DApp::init(HWND hwnd, int physical_w, int physical_h) {
    if (initialized_) return true;
    hwnd_ = hwnd;
    width_ = physical_w;
    height_ = physical_h;

    // PER_MONITOR_AWARE_V2 在 wWinMain 早调；这里再读一次拿当前 monitor DPI
    HDC hdc = GetDC(hwnd_);
    if (hdc) {
        UINT d = GetDpiForWindow(hwnd_);
        if (d > 0) dpi_ = (float)d;
        ReleaseDC(hwnd_, hdc);
    }

    initD3D();
    initSwapChain();
    initD2D();
    initDComp();
    initDWrite();
    initWIC();

    // D2D RT 默认 96 DPI；告诉它实际 DPI 让逻辑像素 (DIP) 自动 scale
    d2d_ctx_->SetDpi(dpi_, dpi_);

    brushes_.init(d2d_ctx_.Get());
    texts_.init(dwrite_.Get());
    strokes_.init(d2d_factory_.Get());
    images_.init(d2d_ctx_.Get(), wic_.Get());
    gifs_.init(d2d_ctx_.Get(), wic_.Get());
    emojis_.init(d2d_ctx_.Get(), dwrite_.Get());

    initialized_ = true;
    return true;
}

void D2DApp::shutdown() {
    if (!initialized_) return;
    emojis_.release();
    gifs_.release();
    images_.release();
    strokes_.release();
    texts_.release();
    brushes_.release();

    back_buffer_.Reset();
    if (d2d_ctx_) d2d_ctx_->SetTarget(nullptr);

    dcomp_visual_.Reset();
    dcomp_target_.Reset();
    dcomp_.Reset();

    d2d_ctx_.Reset();
    d2d_device_.Reset();
    d2d_factory_.Reset();

    swap2_.Reset();
    swap_.Reset();
    d3d_ctx_.Reset();
    d3d_device_.Reset();

    if (frame_waitable_) { CloseHandle(frame_waitable_); frame_waitable_ = nullptr; }

    dwrite_.Reset();
    wic_.Reset();

    initialized_ = false;
}

void D2DApp::initD3D() {
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;   // DComp + D2D 都要 BGRA
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    D3D_FEATURE_LEVEL fl[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    D3D_FEATURE_LEVEL got{};
    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        flags, fl, _countof(fl), D3D11_SDK_VERSION,
        &d3d_device_, &got, &d3d_ctx_);
    // SDK Debug Layer 没装时退到非 debug
    if (FAILED(hr) && (flags & D3D11_CREATE_DEVICE_DEBUG)) {
        flags &= ~D3D11_CREATE_DEVICE_DEBUG;
        D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            flags, fl, _countof(fl), D3D11_SDK_VERSION,
            &d3d_device_, &got, &d3d_ctx_);
    }
}

void D2DApp::initSwapChain() {
    ComPtr<IDXGIDevice> dxgi_dev;
    d3d_device_.As(&dxgi_dev);
    ComPtr<IDXGIAdapter> adapter;
    dxgi_dev->GetAdapter(&adapter);
    ComPtr<IDXGIFactory2> factory;
    adapter->GetParent(IID_PPV_ARGS(&factory));

    // VRR / G-Sync / FreeSync 检测
    {
        ComPtr<IDXGIFactory5> f5;
        if (SUCCEEDED(factory.As(&f5))) {
            BOOL allow = FALSE;
            if (SUCCEEDED(f5->CheckFeatureSupport(
                    DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof(allow)))) {
                tearing_supported_ = (allow == TRUE);
            }
        }
    }

    DXGI_SWAP_CHAIN_DESC1 d{};
    d.Width            = width_;
    d.Height           = height_;
    d.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.Stereo           = FALSE;
    d.SampleDesc       = { 1, 0 };
    d.BufferUsage      = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    d.BufferCount      = 3;
    d.Scaling          = DXGI_SCALING_STRETCH;
    d.SwapEffect       = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    d.AlphaMode        = DXGI_ALPHA_MODE_PREMULTIPLIED;   // DComp 强制
    d.Flags            = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    if (tearing_supported_) d.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

    // ForComposition (不是 ForHwnd) — DComp 需要 dangling swap chain 由我们手动挂 visual
    factory->CreateSwapChainForComposition(d3d_device_.Get(), &d, nullptr, &swap_);
    swap_.As(&swap2_);
    swap2_->SetMaximumFrameLatency(1);   // 配 BufferCount=3 拿 throughput + 低延迟
    frame_waitable_ = swap2_->GetFrameLatencyWaitableObject();
}

void D2DApp::initD2D() {
    D2D1_FACTORY_OPTIONS opts{};
#ifdef _DEBUG
    opts.debugLevel = D2D1_DEBUG_LEVEL_INFORMATION;
#endif
    D2D1CreateFactory(
        D2D1_FACTORY_TYPE_SINGLE_THREADED,
        __uuidof(ID2D1Factory1),
        &opts,
        (void**)d2d_factory_.GetAddressOf());

    ComPtr<IDXGIDevice> dxgi_dev;
    d3d_device_.As(&dxgi_dev);
    d2d_factory_->CreateDevice(dxgi_dev.Get(), &d2d_device_);
    d2d_device_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &d2d_ctx_);
}

void D2DApp::initDComp() {
    ComPtr<IDXGIDevice> dxgi_dev;
    d3d_device_.As(&dxgi_dev);
    DCompositionCreateDevice(dxgi_dev.Get(), IID_PPV_ARGS(&dcomp_));
    dcomp_->CreateTargetForHwnd(hwnd_, TRUE, &dcomp_target_);
    dcomp_->CreateVisual(&dcomp_visual_);
    dcomp_visual_->SetContent(swap_.Get());
    dcomp_target_->SetRoot(dcomp_visual_.Get());
    dcomp_->Commit();
}

void D2DApp::initDWrite() {
    DWriteCreateFactory(
        DWRITE_FACTORY_TYPE_SHARED,
        __uuidof(IDWriteFactory),
        (IUnknown**)dwrite_.GetAddressOf());
}

void D2DApp::initWIC() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    CoCreateInstance(
        CLSID_WICImagingFactory,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&wic_));
}

void D2DApp::requestResize(int physical_w, int physical_h) {
    if (physical_w <= 0 || physical_h <= 0) return;
    pending_w_ = physical_w;
    pending_h_ = physical_h;
    resize_pending_ = true;
}

void D2DApp::doResize() {
    if (!resize_pending_) return;
    resize_pending_ = false;
    if (pending_w_ == width_ && pending_h_ == height_) return;

    // 释放所有 back-buffer 引用，否则 ResizeBuffers 失败 (E_INVALIDARG)
    if (d2d_ctx_) d2d_ctx_->SetTarget(nullptr);
    back_buffer_.Reset();

    DXGI_SWAP_CHAIN_DESC1 d{};
    swap_->GetDesc1(&d);
    swap_->ResizeBuffers(d.BufferCount, pending_w_, pending_h_, d.Format, d.Flags);
    width_ = pending_w_;
    height_ = pending_h_;
}

DWORD D2DApp::waitFrame(DWORD timeout_ms) {
    if (!frame_waitable_) return WAIT_FAILED;
    return WaitForSingleObjectEx(frame_waitable_, timeout_ms, /*alertable=*/TRUE);
}

bool D2DApp::beginFrame() {
    if (!d2d_ctx_ || !swap_) return false;
    if (resize_pending_) doResize();

    ComPtr<IDXGISurface> surf;
    if (FAILED(swap_->GetBuffer(0, IID_PPV_ARGS(&surf)))) return false;

    D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        dpi_, dpi_);

    if (FAILED(d2d_ctx_->CreateBitmapFromDxgiSurface(surf.Get(), &bp, &back_buffer_)))
        return false;

    d2d_ctx_->SetTarget(back_buffer_.Get());
    d2d_ctx_->BeginDraw();
    return true;
}

void D2DApp::endFrame() {
    if (!d2d_ctx_ || !swap_) return;
    HRESULT hr = d2d_ctx_->EndDraw();
    (void)hr;   // device removed 等以后做（业务暂时不处理 device-lost）

    // 必须 release back-buffer 引用，否则 Present 不能 flip
    d2d_ctx_->SetTarget(nullptr);
    back_buffer_.Reset();

    UINT flags = tearing_supported_ ? DXGI_PRESENT_ALLOW_TEARING : 0;
    swap_->Present(0, flags);
}

}  // namespace launcher::d2d
