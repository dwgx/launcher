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
#include <dxgi1_3.h>
#include <dcomp.h>
#include <wrl/client.h>
#include <chrono>
#include <cmath>
#include <cstdio>

#pragma comment(lib, "d3d11.lib")
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
static void render() {
    if (!g_app.rtv) rebuildRTV();
    // 时间驱动的颜色：演示 vsync 锁定下颜色脉冲不抖
    float t = elapsed();
    float pulse = 0.5f + 0.5f * std::sin(t * 2.0f);
    // BGRA premul alpha — DComp 要求 premul
    float r = 0.78f * pulse;
    float g = 0.39f * pulse;
    float b = 0.26f * pulse;
    float a = pulse;
    // 反 premul: dst = (r*a, g*a, b*a, a)
    float clr[4] = { b * a, g * a, r * a, a };
    g_app.ctx->OMSetRenderTargets(1, g_app.rtv.GetAddressOf(), nullptr);
    g_app.ctx->ClearRenderTargetView(g_app.rtv.Get(), clr);
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
