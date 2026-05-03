// 金标准: WS_EX_NOREDIRECTIONBITMAP + DComp + DXGI flip-model + waitable + D3D11
//
// 这一版没接 Skia — aseprite/skia bundle 是 GL only，没 D3D backend。
// 用 D3D11 直接 ClearRenderTargetView 验证 pipeline，证明：
//   1. 窗口完全跳过 DWM redirection bitmap (NOREDIRECT)
//   2. DComp 直挂 swap chain → DWM compositor
//   3. waitable swap chain 做 frame pacing (跟 vblank 同步，不忙等)
//   4. ALLOW_TEARING + FLIP_DISCARD 给 VRR / G-Sync / FreeSync 让路
//
// Skia 接入 — 两条路（这版还没做）:
//   A. WGL_NV_DX_interop2: 用 wglDXRegisterObjectNV 把 swap chain 的
//      ID3D11Texture2D 共享给 GL，Skia GL 渲染到那张 texture。
//      ~200 行，跟现有 aseprite/skia GL bundle 兼容。
//   B. 重 build Skia 带 skia_use_direct3d=true，用
//      GrDirectContexts::MakeDirect3D 直 render 到 swap chain back buffer。
//      ~50 行（最干净），但需 depot_tools + GN build (2-3h)。

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <d3d11.h>
#include <d2d1_1.h>
#include <d2d1.h>
#include <dxgi1_3.h>
#include <dcomp.h>
#include <wrl/client.h>
#include <chrono>
#include <cmath>
#include <cstdio>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dcomp.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "dwmapi.lib")

using Microsoft::WRL::ComPtr;

// ================== 状态 ==================
struct App {
    HWND hwnd{};
    int  width  = 800;
    int  height = 500;

    // D3D11
    ComPtr<ID3D11Device>           device;
    ComPtr<ID3D11DeviceContext>    ctx;

    // DXGI flip swap chain
    ComPtr<IDXGISwapChain1>        swap;
    ComPtr<IDXGISwapChain2>        swap2;          // 拿 waitable
    HANDLE                         frame_waitable = nullptr;
    bool                           tearing_supported = false;

    // RTV per frame (back buffer 重新 GetBuffer)
    ComPtr<ID3D11RenderTargetView> rtv;

    // DComp 视觉树
    ComPtr<IDCompositionDevice>    dcomp;
    ComPtr<IDCompositionTarget>    dcomp_target;
    ComPtr<IDCompositionVisual>    dcomp_visual;

    // Direct2D — 在同一个 D3D11 device / swap chain 上画矢量
    ComPtr<ID2D1Factory1>          d2d_factory;
    ComPtr<ID2D1Device>            d2d_device;
    ComPtr<ID2D1DeviceContext>     d2d_ctx;
    ComPtr<ID2D1SolidColorBrush>   brush_primary;
    ComPtr<ID2D1SolidColorBrush>   brush_text;
    ComPtr<ID2D1SolidColorBrush>   brush_card;

    bool resize_pending = false;
    int  pending_w = 0, pending_h = 0;
};
static App g_app;

// ================== 工具：HRESULT 检查 ==================
static void hr_check(HRESULT hr, const char* what) {
    if (FAILED(hr)) {
        char buf[256];
        sprintf_s(buf, "%s failed: 0x%08X", what, (unsigned)hr);
        MessageBoxA(nullptr, buf, "Skia D3D PoC", MB_OK | MB_ICONERROR);
        ExitProcess(1);
    }
}

// ================== 创建 D3D11 device ==================
static void initD3D() {
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;   // DComp 要 BGRA
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    D3D_FEATURE_LEVEL fl[] = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
    };
    D3D_FEATURE_LEVEL got_fl = {};
    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        flags, fl, _countof(fl), D3D11_SDK_VERSION,
        &g_app.device, &got_fl, &g_app.ctx);
    if (FAILED(hr) && (flags & D3D11_CREATE_DEVICE_DEBUG)) {
        // SDK Debug Layer 没装时退到非 debug 重试
        flags &= ~D3D11_CREATE_DEVICE_DEBUG;
        hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            flags, fl, _countof(fl), D3D11_SDK_VERSION,
            &g_app.device, &got_fl, &g_app.ctx);
    }
    hr_check(hr, "D3D11CreateDevice");
}

// ================== 检测 ALLOW_TEARING（VRR）支持 ==================
static bool checkTearingSupport(IDXGIFactory2* factory) {
    ComPtr<IDXGIFactory5> f5;
    if (FAILED(factory->QueryInterface(IID_PPV_ARGS(&f5)))) return false;
    BOOL allow = FALSE;
    if (FAILED(f5->CheckFeatureSupport(
            DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof(allow))))
        return false;
    return allow == TRUE;
}

// ================== 创建 flip-model 交换链 ==================
static void initSwapChain() {
    ComPtr<IDXGIDevice> dxgi_dev;
    hr_check(g_app.device.As(&dxgi_dev), "device.As<IDXGIDevice>");
    ComPtr<IDXGIAdapter> adapter;
    hr_check(dxgi_dev->GetAdapter(&adapter), "GetAdapter");
    ComPtr<IDXGIFactory2> factory;
    hr_check(adapter->GetParent(IID_PPV_ARGS(&factory)), "GetParent<IDXGIFactory2>");

    g_app.tearing_supported = checkTearingSupport(factory.Get());

    DXGI_SWAP_CHAIN_DESC1 d{};
    d.Width            = g_app.width;
    d.Height           = g_app.height;
    d.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;   // DComp 要 BGRA
    d.Stereo           = FALSE;
    d.SampleDesc       = {1, 0};                       // MSAA 走 Skia surface 自己
    d.BufferUsage      = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    d.BufferCount      = 3;                            // triple buffer
    d.Scaling          = DXGI_SCALING_STRETCH;
    d.SwapEffect       = DXGI_SWAP_EFFECT_FLIP_DISCARD;// 现代 flip 模式
    d.AlphaMode        = DXGI_ALPHA_MODE_PREMULTIPLIED;// DComp 要 premul alpha
    d.Flags            = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    if (g_app.tearing_supported) d.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

    // 关键：CreateSwapChainForComposition 而不是 ForHwnd —
    // ForHwnd 把 swap chain 绑死在窗口上，会跟 DComp 冲突。
    // ForComposition 出来的 swap chain 是 dangling，由我们手动挂到 DComp visual 上。
    hr_check(factory->CreateSwapChainForComposition(
        g_app.device.Get(), &d, nullptr, &g_app.swap),
        "CreateSwapChainForComposition");
    hr_check(g_app.swap.As(&g_app.swap2), "swap.As<IDXGISwapChain2>");

    // waitable: 1 帧最大延迟（配 BufferCount=3 给 GPU 双缓冲余量）
    hr_check(g_app.swap2->SetMaximumFrameLatency(1), "SetMaximumFrameLatency");
    g_app.frame_waitable = g_app.swap2->GetFrameLatencyWaitableObject();
}

// ================== 重新拿 back buffer 并建 RTV ==================
static void rebuildRTV() {
    g_app.rtv.Reset();
    ComPtr<ID3D11Texture2D> bb;
    hr_check(g_app.swap->GetBuffer(0, IID_PPV_ARGS(&bb)), "GetBuffer(0)");
    hr_check(g_app.device->CreateRenderTargetView(bb.Get(), nullptr, &g_app.rtv),
        "CreateRenderTargetView");
}

// ================== 让 D2D 把 swap chain back buffer 当 target ==================
// FLIP_DISCARD 每次 Present 后 buffer[0] 是 fresh 的（上一帧内容丢弃），
// 所以每帧 GetBuffer(0) → CreateBitmapFromDxgiSurface → SetTarget。
// 不能跨帧重用 ID2D1Bitmap1，否则 d2d 持有 buffer 让 Present 不能 flip。
static ComPtr<ID2D1Bitmap1> bindD2DTarget() {
    ComPtr<IDXGISurface> dxgi_back;
    hr_check(g_app.swap->GetBuffer(0, IID_PPV_ARGS(&dxgi_back)), "GetBuffer(IDXGISurface)");
    D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f, 96.0f);
    ComPtr<ID2D1Bitmap1> bmp;
    hr_check(g_app.d2d_ctx->CreateBitmapFromDxgiSurface(
        dxgi_back.Get(), &bp, &bmp),
        "D2D CreateBitmapFromDxgiSurface");
    g_app.d2d_ctx->SetTarget(bmp.Get());
    return bmp;
}

// ================== 初始化 Direct2D ==================
// D2D1 跟 D3D11 共享 GPU resource，零拷贝。在 swap chain back buffer 上画矢量。
static void initD2D() {
    D2D1_FACTORY_OPTIONS opts{};
#ifdef _DEBUG
    opts.debugLevel = D2D1_DEBUG_LEVEL_INFORMATION;
#endif
    hr_check(D2D1CreateFactory(
        D2D1_FACTORY_TYPE_SINGLE_THREADED,
        __uuidof(ID2D1Factory1),
        &opts,
        (void**)g_app.d2d_factory.GetAddressOf()),
        "D2D1CreateFactory");

    ComPtr<IDXGIDevice> dxgi_dev;
    hr_check(g_app.device.As(&dxgi_dev), "device.As<IDXGIDevice>");
    hr_check(g_app.d2d_factory->CreateDevice(dxgi_dev.Get(), &g_app.d2d_device),
        "D2D Factory->CreateDevice");
    hr_check(g_app.d2d_device->CreateDeviceContext(
        D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &g_app.d2d_ctx),
        "D2D Device->CreateDeviceContext");

    // 主色：跟 GDI+ Preview 一致 (#C96442 + 一档亮)
    g_app.d2d_ctx->CreateSolidColorBrush(
        D2D1::ColorF(0.85f, 0.42f, 0.27f, 1.0f), &g_app.brush_primary);
    g_app.d2d_ctx->CreateSolidColorBrush(
        D2D1::ColorF(0.96f, 0.94f, 0.91f, 0.85f), &g_app.brush_text);
    g_app.d2d_ctx->CreateSolidColorBrush(
        D2D1::ColorF(0.14f, 0.13f, 0.12f, 0.92f), &g_app.brush_card);
}

// ================== DComp 视觉树挂 swap chain ==================
static void initDComp() {
    ComPtr<IDXGIDevice> dxgi_dev;
    hr_check(g_app.device.As(&dxgi_dev), "device.As<IDXGIDevice>");
    hr_check(DCompositionCreateDevice(
        dxgi_dev.Get(), IID_PPV_ARGS(&g_app.dcomp)),
        "DCompositionCreateDevice");

    // 把 dcomp target 绑到我们的 hwnd（topmost=TRUE 让 dcomp 视觉盖在窗口客户区上）
    hr_check(g_app.dcomp->CreateTargetForHwnd(
        g_app.hwnd, TRUE, &g_app.dcomp_target),
        "CreateTargetForHwnd");

    hr_check(g_app.dcomp->CreateVisual(&g_app.dcomp_visual), "CreateVisual");
    hr_check(g_app.dcomp_visual->SetContent(g_app.swap.Get()), "SetContent(swap)");
    hr_check(g_app.dcomp_target->SetRoot(g_app.dcomp_visual.Get()), "SetRoot");
    hr_check(g_app.dcomp->Commit(), "DComp Commit");
}

// ================== 重建逻辑（窗口 resize） ==================
static void resizeBuffers(int w, int h) {
    if (w <= 0 || h <= 0) return;
    if (w == g_app.width && h == g_app.height) return;
    g_app.width = w; g_app.height = h;
    g_app.rtv.Reset();
    DXGI_SWAP_CHAIN_DESC1 d{}; g_app.swap->GetDesc1(&d);
    hr_check(g_app.swap->ResizeBuffers(
        d.BufferCount, w, h, d.Format, d.Flags),
        "ResizeBuffers");
}

// ================== 渲染（D3D11 直接清屏 — 占位演示 pipeline） ==================
static auto g_t0 = std::chrono::steady_clock::now();
static float elapsed() {
    return std::chrono::duration<float>(
        std::chrono::steady_clock::now() - g_t0).count();
}
// 真正的渲染：D2D 在 swap chain back buffer 上画矢量图形 — 跟 Skia 同档次
// 的硬件 AA 矢量渲染（D2D 用 D3D11 GPU 后端，~16x analytic AA）。
static void render() {
    auto bmp = bindD2DTarget();   // 帧末走出作用域时自动 release，让 Present flip

    g_app.d2d_ctx->BeginDraw();
    // 整窗暗 bg (Launcher kDark.bg = 0x1A1816 + premul)
    g_app.d2d_ctx->Clear(D2D1::ColorF(0x1A1816, 1.0f));

    float t = elapsed();
    float W = (float)g_app.width, H = (float)g_app.height;
    float cx = W * 0.5f, cy = H * 0.5f;

    // ===== 1) 中心圆角卡 (240x140) — 跟 Launcher game-card 一档 =====
    float card_w = 240, card_h = 140;
    D2D1_ROUNDED_RECT card = {
        D2D1::RectF(cx - card_w * 0.5f, cy - card_h * 0.5f,
                    cx + card_w * 0.5f, cy + card_h * 0.5f),
        12, 12
    };
    // 阴影 — 用 8 层逐渐放大递减 alpha 模拟柔和高斯（廉价但视觉够用）
    for (int i = 0; i < 6; ++i) {
        float spread = 2.0f + i * 1.6f;
        float a = 0.06f / (i + 1);
        D2D1_ROUNDED_RECT sh = {
            D2D1::RectF(card.rect.left  - spread, card.rect.top   - spread + 4,
                        card.rect.right + spread, card.rect.bottom + spread + 4),
            12.0f + spread, 12.0f + spread
        };
        ComPtr<ID2D1SolidColorBrush> sb;
        g_app.d2d_ctx->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, a), &sb);
        g_app.d2d_ctx->FillRoundedRectangle(sh, sb.Get());
    }
    g_app.d2d_ctx->FillRoundedRectangle(card, g_app.brush_card.Get());

    // ===== 2) 中心转圈圈 spinner — 跟 Launcher loading 一致 =====
    // 旋转的 270° 弧 + 圆角端帽，1.4s 一圈
    float spin_r = 22.0f;
    float angle = std::fmod(t * 360.0f / 1.4f, 360.0f);
    float start = angle * 3.14159265f / 180.0f;
    float sweep = 270.0f * 3.14159265f / 180.0f;
    float end = start + sweep;

    // path geom：先在原点画弧（中心 0,0），后面 SetTransform 平移到 (cx, cy)
    ComPtr<ID2D1PathGeometry> arc_geo;
    g_app.d2d_factory->CreatePathGeometry(&arc_geo);
    ComPtr<ID2D1GeometrySink> sink;
    arc_geo->Open(&sink);
    sink->BeginFigure(
        D2D1::Point2F(spin_r * std::cos(start), spin_r * std::sin(start)),
        D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddArc(D2D1::ArcSegment(
        D2D1::Point2F(spin_r * std::cos(end), spin_r * std::sin(end)),
        D2D1::SizeF(spin_r, spin_r),
        0.0f,
        D2D1_SWEEP_DIRECTION_CLOCKWISE,
        D2D1_ARC_SIZE_LARGE));
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    sink->Close();

    // 圆角端帽用 stroke style
    ComPtr<ID2D1StrokeStyle> stroke;
    g_app.d2d_factory->CreateStrokeStyle(
        D2D1::StrokeStyleProperties(
            D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_FLAT,
            D2D1_LINE_JOIN_ROUND, 10.0f,
            D2D1_DASH_STYLE_SOLID, 0.0f),
        nullptr, 0, &stroke);

    g_app.d2d_ctx->SetTransform(D2D1::Matrix3x2F::Translation(cx, cy));
    g_app.d2d_ctx->DrawGeometry(arc_geo.Get(), g_app.brush_primary.Get(),
                                 3.0f, stroke.Get());
    g_app.d2d_ctx->SetTransform(D2D1::Matrix3x2F::Identity());

    // ===== 3) 中心打勾静态 (不旋转) — 给眼睛对比"动 vs 静" =====
    float dot_r = 4.0f;
    g_app.d2d_ctx->FillEllipse(
        D2D1::Ellipse(D2D1::Point2F(cx, cy), dot_r, dot_r),
        g_app.brush_primary.Get());

    // ===== 4) 左下角 FPS / 时间 戳记号 (用画的小 tick 表示动画顺) =====
    // 画 60 个小竖条，按时间相位左→右扫过 — 任何卡顿在这条尺上都会显出来
    float bar_y = H - 12;
    for (int i = 0; i < 60; ++i) {
        float phase = std::fmod(t * 0.5f + i / 60.0f, 1.0f);
        float bar_h = 2.0f + 6.0f * (1.0f - phase);
        float bar_x = 12 + i * 4.0f;
        ComPtr<ID2D1SolidColorBrush> bar_b;
        g_app.d2d_ctx->CreateSolidColorBrush(
            D2D1::ColorF(0.85f, 0.42f, 0.27f, 0.4f * (1.0f - phase)),
            &bar_b);
        g_app.d2d_ctx->FillRectangle(
            D2D1::RectF(bar_x, bar_y - bar_h, bar_x + 2, bar_y),
            bar_b.Get());
    }

    HRESULT hr = g_app.d2d_ctx->EndDraw();
    if (FAILED(hr)) {
        // 可能 device removed — 现实代码要重建
    }
    g_app.d2d_ctx->SetTarget(nullptr);   // release 掉，否则 Present 不能 flip
}

// ================== Present（带 ALLOW_TEARING） ==================
static void present() {
    UINT flags = g_app.tearing_supported ? DXGI_PRESENT_ALLOW_TEARING : 0;
    HRESULT hr = g_app.swap->Present(0, flags);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        // 简化：现实代码要重建 device + 所有 GPU 资源
        ExitProcess(2);
    }
}

// ================== WndProc ==================
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_SIZE: {
            int w = LOWORD(lp), h = HIWORD(lp);
            if (g_app.swap && w > 0 && h > 0) {
                g_app.resize_pending = true;
                g_app.pending_w = w;
                g_app.pending_h = h;
            }
            return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: { PAINTSTRUCT ps; BeginPaint(hwnd, &ps); EndPaint(hwnd, &ps); return 0; }
        case WM_CLOSE: PostQuitMessage(0); return 0;
        // NCCALCSIZE — 让 NOREDIRECT 窗口仍有标准客户区
        // (NOREDIRECT 不影响 NC 区域，照常 default)
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ================== 入口 ==================
int APIENTRY wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int) {
    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = inst;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"LauncherSkiaD3D";
    RegisterClassExW(&wc);

    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    int W = g_app.width, H = g_app.height;

    // 关键：WS_EX_NOREDIRECTIONBITMAP — opt out DWM redirection bitmap
    // 普通窗口 OS 会给 GPU 一张 redirection bitmap 让 GDI 能直接画上去，
    // DWM 再把这张 bitmap 拷到合成层。我们走 DComp + DXGI flip → 绕过这步。
    // 没有 redirection bitmap 意味着 GDI 画不上去（不能 BeginPaint 直接画），
    // 但我们渲染走 D3D / DComp，本来就不要 GDI。
    g_app.hwnd = CreateWindowExW(
        WS_EX_NOREDIRECTIONBITMAP,
        wc.lpszClassName, L"Launcher Skia D3D PoC",
        WS_OVERLAPPEDWINDOW,
        (sw - W) / 2, (sh - H) / 2, W, H,
        nullptr, nullptr, inst, nullptr);
    if (!g_app.hwnd) { MessageBoxW(nullptr, L"CreateWindow failed", L"PoC", MB_OK); return 1; }

    initD3D();
    initSwapChain();
    initD2D();
    initDComp();
    rebuildRTV();

    ShowWindow(g_app.hwnd, SW_SHOW);
    UpdateWindow(g_app.hwnd);

    // ============== 帧循环（金标准） ==============
    // 模式：
    //   1. WaitForSingleObject(waitable) — DXGI 告诉我可以画下一帧（vsync 同步）
    //   2. PeekMessage 非阻塞处理输入（不会饿死消息）
    //   3. 处理 resize（把 ResizeBuffers 推到主线程）
    //   4. render → Present(0, ALLOW_TEARING) — sync_interval=0 让 waitable 决定节奏
    //   5. 不要 Sleep / DwmFlush / wglSwapInterval — waitable 是唯一节奏源
    //
    // 比 SwapBuffers + DwmFlush 强在哪：
    //   * waitable 是 DXGI 直接给的合成器信号，比 DwmFlush 准
    //   * 让 ALLOW_TEARING 让 G-Sync/FreeSync 接管节奏，VRR 屏不再绑死 60/120
    //   * BufferCount=3 给 GPU 提前画 1-2 帧的余量但 SetMaximumFrameLatency(1)
    //     限制延迟 — 拿到了 throughput + 低延迟两头
    MSG msg{};
    bool quit = false;
    while (!quit) {
        DWORD r = WaitForSingleObjectEx(
            g_app.frame_waitable, 100, /*alertable=*/TRUE);
        // r == WAIT_OBJECT_0 → 可以画
        // r == WAIT_TIMEOUT → 100ms 没等到（窗口最小化等情况），照常处理消息
        // 不管哪种都先 drain 消息
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { quit = true; break; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (quit) break;

        if (g_app.resize_pending) {
            g_app.resize_pending = false;
            resizeBuffers(g_app.pending_w, g_app.pending_h);
            rebuildRTV();
        }
        if (r == WAIT_OBJECT_0) {
            render();
            present();
        }
    }

    // 清理
    if (g_app.frame_waitable) CloseHandle(g_app.frame_waitable);
    return 0;
}
