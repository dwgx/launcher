// Launcher D2D Preview — Phase 2.1 主入口。
//
// Step 1：D2D pipeline baseline（D2DApp + brush/text/stroke/image cache + DComp + waitable）
// Step 2：完整入场动画 1:1 复刻 GDI+ Preview。
// Step 3 (本步)：Auth view (AuthForm + InputBox + 浮动 label + 错误提示)。
// Step 4+：移植 Topbar / Sidebar / Home / Lunching / Chat / ... view 内容。

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <windowsx.h>
#include <chrono>

#include "d2d_app.h"
#include "stages.h"
#include "palette.h"
#include "auth.h"
#include "hit.h"
#include "ui_main.h"

#pragma comment(lib, "user32.lib")

using namespace launcher::d2d;

static D2DApp g_app;

// 物理 client 像素 → DIP（mouse 坐标 / 命中判定都用 DIP）
static POINT physToDip(POINT phys) {
    float scale = g_app.dpi() / 96.0f;
    if (scale < 0.001f) scale = 1.0f;
    return { (LONG)(phys.x / scale + 0.5f), (LONG)(phys.y / scale + 0.5f) };
}

static bool inAuthOrMain() {
    return stages::g_stage == stages::Stage::Auth
        || stages::g_stage == stages::Stage::Main;
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_SIZE: {
            int w = LOWORD(lp), h = HIWORD(lp);
            if (w > 0 && h > 0) g_app.requestResize(w, h);
            return 0;
        }
        case WM_ERASEBKGND: return 1;          // NOREDIRECT 没 GDI bitmap 可擦
        case WM_PAINT: {
            PAINTSTRUCT ps; BeginPaint(hwnd, &ps); EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_NCHITTEST: {
            // 入场期 (Dot/ExpandLoading/Loading/...) 全局拖动
            // Auth/Main 期：默认 HTCAPTION，鼠标在已注册 hits 区返 HTCLIENT
            if (!inAuthOrMain()) return HTCAPTION;
            POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            ScreenToClient(hwnd, &pt);
            POINT dip = physToDip(pt);
            return anyHover(dip) ? HTCLIENT : HTCAPTION;
        }
        case WM_MOUSEMOVE: {
            POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            g_mouse = physToDip(pt);
            return 0;
        }
        case WM_LBUTTONDOWN: {
            POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            g_mouse = physToDip(pt);
            g_mouse_pressed = true;
            if (inAuthOrMain()) dispatchClick(g_mouse);
            return 0;
        }
        case WM_LBUTTONUP: {
            g_mouse_pressed = false;
            return 0;
        }
        case WM_KEYDOWN: {
            bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            bool ctrl  = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            if (wp == VK_ESCAPE) { PostQuitMessage(0); return 0; }
            if (stages::g_stage == stages::Stage::Auth) {
                auth::onKey(hwnd, (int)wp, shift, ctrl);
                return 0;
            }
            // 入场期 / Main：保留快捷键
            if (wp == 'D') { g_dark = !g_dark; return 0; }
            if (wp == 'S' && stages::g_stage == stages::Stage::Loading) {
                stages::g_skip_auth_after_loading = true;
                stages::g_time_in_stage = 1.5f;
                return 0;
            }
            if (wp == VK_RETURN) {
                stages::simulateAuthSubmit();
                return 0;
            }
            return 0;
        }
        case WM_CHAR: {
            bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            if (stages::g_stage == stages::Stage::Auth) {
                auth::onChar(hwnd, (wchar_t)wp, ctrl);
                return 0;
            }
            return 0;
        }
        case WM_APP + 1: {                     // Auth submit
            auth::handleSubmit(hwnd);
            return 0;
        }
        case WM_APP + 2: {                     // Auth submit result (后台线程 PostMessage)
            auth::onSubmitResult(hwnd, wp != 0);
            return 0;
        }
        case WM_RBUTTONDOWN:
            // 右键空白处 → ESC 同效；托盘最小化留 Step 后做
            PostQuitMessage(0);
            return 0;
        case WM_CLOSE:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int APIENTRY wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = inst;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"LauncherD2DPreview";
    RegisterClassExW(&wc);

    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);

    UINT dpi_for_init = GetDpiForSystem();
    if (dpi_for_init == 0) dpi_for_init = 96;
    int W0 = (int)(40.0f * dpi_for_init / 96.0f);
    int H0 = (int)(40.0f * dpi_for_init / 96.0f);

    HWND hwnd = CreateWindowExW(
        WS_EX_NOREDIRECTIONBITMAP,
        wc.lpszClassName, L"Launcher",
        WS_POPUP,                                // 无边框 — 入场动画期间不要 OS 标题栏跳动
        (sw - W0) / 2, (sh - H0) / 2, W0, H0,
        nullptr, nullptr, inst, nullptr);
    if (!hwnd) return 1;

    RECT rc{};
    GetClientRect(hwnd, &rc);
    if (!g_app.init(hwnd, rc.right - rc.left, rc.bottom - rc.top)) {
        DestroyWindow(hwnd);
        return 1;
    }

    stages::enterDotStage();
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    auto last = std::chrono::steady_clock::now();
    MSG msg{};
    bool quit = false;
    while (!quit) {
        DWORD r = g_app.waitFrame(100);
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { quit = true; break; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (quit) break;
        if (r != WAIT_OBJECT_0) continue;

        auto now = std::chrono::steady_clock::now();
        float dt = std::chrono::duration<float>(now - last).count();
        last = now;
        if (dt > 0.1f) dt = 0.1f;

        stages::tick(dt);
        ui::tickMain(dt);
        stages::driveTransitions(g_app, sw, sh);

        if (g_app.beginFrame()) {
            stages::paint(g_app);
            g_app.endFrame();
        }
    }

    g_app.shutdown();
    return 0;
}
