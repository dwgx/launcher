// Skia PoC — Win32 + WGL + Ganesh GL backend，画 Launcher loading dot 动画。
// 跟 tools/preview 的 GDI+ 版本同样画面，但走 GPU + 真 8× MSAA。
//
// 验证目标：
//   1. third_party/skia/ 静态库能 link
//   2. Skia GL Ganesh 能在 Win32 layered window 上渲染
//   3. 圆 / 圆角矩形 / 阴影 / 文字 比 GDI+ 看上去明显平滑（无锯齿、阴影柔和）
//   4. 动画跟 vsync / DWM 协作好（不卡顿）
//
// build: tools/preview-skia/build_skia_demo.bat

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <mmsystem.h>
#include <dwmapi.h>
#include <gl/GL.h>
#include <chrono>
#include <cmath>
#include <cstdio>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "opengl32.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "dwmapi.lib")

// WGL 扩展函数指针 — 用于开 vsync。glext 不在 Windows SDK 里所以手写。
typedef BOOL(WINAPI*PFNWGLSWAPINTERVALEXTPROC)(int);

#include "include/core/SkCanvas.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkPaint.h"
#include "include/core/SkRRect.h"
#include "include/core/SkSurface.h"
#include "include/core/SkSurfaceProps.h"
#include "include/core/SkFont.h"
#include "include/core/SkTypeface.h"
#include "include/core/SkFontMgr.h"
#include "include/effects/SkImageFilters.h"
// m124 layout: GrDirectContext.h 在 gpu/，gl 在 gpu/gl/，
// GL backend surface helpers 在 gpu/ganesh/gl/，SkSurfaceGanesh 在 gpu/ganesh/
#include "include/gpu/GrBackendSurface.h"     // GrBackendRenderTarget
#include "include/gpu/GrDirectContext.h"
#include "include/gpu/ganesh/SkSurfaceGanesh.h"
#include "include/gpu/ganesh/gl/GrGLDirectContext.h"
#include "include/gpu/ganesh/gl/GrGLBackendSurface.h"
#include "include/gpu/gl/GrGLInterface.h"
#include "include/ports/SkFontMgr_empty.h"

// ---------- zlib symbol trampoline ----------
// aseprite/skia m124 bundle 里 freetype2.lib 调未前缀 inflateInit2_/inflate/...
// 但同 bundle 的 zlib.lib + skia.lib 内部 zlib 都用 Cr_z_ 前缀（Chromium 惯例）。
// 这里写 4 个转发，让 freetype 链上。
extern "C" {
    int  Cr_z_inflateInit2_(void* strm, int windowBits, const char* version, int stream_size);
    int  Cr_z_inflate(void* strm, int flush);
    int  Cr_z_inflateEnd(void* strm);
    int  Cr_z_inflateReset(void* strm);
    int  inflateInit2_(void* strm, int windowBits, const char* version, int stream_size)
        { return Cr_z_inflateInit2_(strm, windowBits, version, stream_size); }
    int  inflate(void* strm, int flush)        { return Cr_z_inflate(strm, flush); }
    int  inflateEnd(void* strm)                { return Cr_z_inflateEnd(strm); }
    int  inflateReset(void* strm)              { return Cr_z_inflateReset(strm); }
}

// ---------- WGL minimal init ----------
static HGLRC g_glrc = nullptr;
static HDC   g_hdc  = nullptr;

static bool initGL(HWND hwnd) {
    g_hdc = GetDC(hwnd);
    PIXELFORMATDESCRIPTOR pfd{};
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;
    pfd.cStencilBits = 8;
    int pf = ChoosePixelFormat(g_hdc, &pfd);
    if (!pf || !SetPixelFormat(g_hdc, pf, &pfd)) return false;
    g_glrc = wglCreateContext(g_hdc);
    if (!g_glrc) return false;
    if (!wglMakeCurrent(g_hdc, g_glrc)) return false;

    // 关键：vsync 锁定 — SwapBuffers 内部 block 到下次 vblank
    // 60Hz 屏 16.67ms / 帧；120Hz 8.33ms / 帧；240Hz 4.17ms / 帧。
    // OS 帮我们定节奏，比 Sleep 准、比忙等省 CPU。
    auto wglSwapIntervalEXT = (PFNWGLSWAPINTERVALEXTPROC)
        wglGetProcAddress("wglSwapIntervalEXT");
    if (wglSwapIntervalEXT) wglSwapIntervalEXT(1);
    return true;
}

// ---------- Skia state ----------
static sk_sp<const GrGLInterface> g_iface;
static sk_sp<GrDirectContext>     g_ctx;
static sk_sp<SkSurface>           g_surf;
static int g_w = 0, g_h = 0;

static void initSkia() {
    g_iface = GrGLMakeNativeInterface();
    g_ctx   = GrDirectContexts::MakeGL(g_iface);
}
static void resizeSkia(int w, int h) {
    g_w = w; g_h = h;
    GrGLFramebufferInfo fb{};
    fb.fFBOID = 0;
    fb.fFormat = 0x8058;   // GL_RGBA8
    auto rt = GrBackendRenderTargets::MakeGL(w, h, /*samples=*/8, /*stencil=*/8, fb);
    SkSurfaceProps props(0, kUnknown_SkPixelGeometry);
    g_surf = SkSurfaces::WrapBackendRenderTarget(
        g_ctx.get(), rt, kBottomLeft_GrSurfaceOrigin, kRGBA_8888_SkColorType,
        nullptr, &props);
}

// ---------- 简易动画 ----------
static auto g_t0 = std::chrono::steady_clock::now();
static float elapsed() {
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration<float>(now - g_t0).count();
}

static SkColor mix(SkColor a, SkColor b, float t) {
    auto lerp = [&](int x, int y){ return (int)(x + (y - x) * t + 0.5f); };
    return SkColorSetARGB(
        lerp(SkColorGetA(a), SkColorGetA(b)),
        lerp(SkColorGetR(a), SkColorGetR(b)),
        lerp(SkColorGetG(a), SkColorGetG(b)),
        lerp(SkColorGetB(a), SkColorGetB(b)));
}

static void render() {
    if (!g_surf) return;
    SkCanvas* c = g_surf->getCanvas();
    c->clear(SkColorSetARGB(255, 0x1A, 0x18, 0x16));   // dark bg

    float t = elapsed();
    float cx = g_w / 2.0f, cy = g_h / 2.0f;

    // 旋转 spinner: 一圈 1.4s
    float ang = std::fmod(t * 360.0f / 1.4f, 360.0f);
    SkPaint p;
    p.setAntiAlias(true);
    p.setStyle(SkPaint::kStroke_Style);
    p.setStrokeWidth(3.0f);
    p.setStrokeCap(SkPaint::kRound_Cap);
    p.setColor(SkColorSetARGB(255, 0xC9, 0x64, 0x42));   // primary

    SkRect oval = SkRect::MakeXYWH(cx - 22, cy - 22, 44, 44);
    c->drawArc(oval, ang, 270.0f, false, p);

    // 中心带阴影的圆角卡片（200x200 模拟 loading 卡）— 演示 Blur
    SkRRect rr;
    rr.setRectXY(SkRect::MakeXYWH(cx - 100, cy + 50, 200, 60), 12, 12);

    SkPaint sh;
    sh.setAntiAlias(true);
    sh.setColor(SkColorSetARGB(60, 0, 0, 0));
    sh.setImageFilter(SkImageFilters::Blur(8.0f, 8.0f, nullptr));
    c->save();
    c->translate(0, 4);
    c->drawRRect(rr, sh);
    c->restore();

    SkPaint card;
    card.setAntiAlias(true);
    card.setColor(SkColorSetARGB(255, 0x24, 0x22, 0x20));
    c->drawRRect(rr, card);

    // 文字 — 用空字体管理器拿默认 typeface（演示，不接 HarfBuzz）
    static sk_sp<SkFontMgr> fmgr = SkFontMgr_New_Custom_Empty();
    static sk_sp<SkTypeface> tf;
    if (!tf) {
        // 尝试系统默认；空 fmgr 没 typeface，下面 SkFont 会用兜底
        tf = nullptr;
    }
    SkFont font(tf, 14);
    SkPaint tp;
    tp.setAntiAlias(true);
    tp.setColor(SK_ColorWHITE);
    const char* msg = "Skia + Ganesh + 8x MSAA";
    SkRect bounds; font.measureText(msg, strlen(msg), SkTextEncoding::kUTF8, &bounds);
    c->drawSimpleText(msg, strlen(msg), SkTextEncoding::kUTF8,
                      cx - bounds.width() / 2.0f,
                      cy + 88, font, tp);

    g_ctx->flushAndSubmit();
    SwapBuffers(g_hdc);
}

// ---------- WndProc ----------
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_SIZE: {
            int w = LOWORD(lp), h = HIWORD(lp);
            if (w > 0 && h > 0 && g_ctx) {
                glViewport(0, 0, w, h);
                resizeSkia(w, h);
            }
            return 0;
        }
        case WM_CLOSE: PostQuitMessage(0); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: { PAINTSTRUCT ps; BeginPaint(hwnd, &ps); EndPaint(hwnd, &ps); return 0; }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int APIENTRY wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int) {
    timeBeginPeriod(1);

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"LauncherSkiaPoC";
    RegisterClassExW(&wc);

    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    int W = 600, H = 400;
    HWND hwnd = CreateWindowExW(
        0, wc.lpszClassName, L"Launcher Skia PoC",
        WS_OVERLAPPEDWINDOW,
        (sw - W) / 2, (sh - H) / 2, W, H,
        nullptr, nullptr, inst, nullptr);
    if (!hwnd) return 1;

    if (!initGL(hwnd)) {
        MessageBoxW(hwnd, L"WGL init failed", L"Skia PoC", MB_OK);
        return 2;
    }
    initSkia();
    if (!g_ctx) {
        MessageBoxW(hwnd, L"Skia GL context init failed", L"Skia PoC", MB_OK);
        return 3;
    }
    RECT rc; GetClientRect(hwnd, &rc);
    glViewport(0, 0, rc.right, rc.bottom);
    resizeSkia(rc.right, rc.bottom);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    // 帧循环 — 现代正确写法：
    //   1) PeekMessage 非阻塞处理输入
    //   2) render() 全 GPU 画
    //   3) SwapBuffers 内部锁 vsync（不会忙等，OS 帮 block 到 vblank）
    //   4) DwmFlush 等 DWM 把这帧真上屏，避免我们提前画好下一帧排队
    //   5) 不要 Sleep — vsync + DwmFlush 已经定准节奏
    MSG msg{};
    while (true) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) goto end;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        render();
        SwapBuffers(g_hdc);
        DwmFlush();
    }
end:
    g_surf.reset();
    if (g_ctx) g_ctx->abandonContext();
    g_ctx.reset();
    g_iface.reset();
    if (g_glrc) { wglMakeCurrent(nullptr, nullptr); wglDeleteContext(g_glrc); }
    if (g_hdc)  ReleaseDC(hwnd, g_hdc);
    timeEndPeriod(1);
    return 0;
}
