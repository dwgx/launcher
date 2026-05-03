// Launcher D2D Preview — Phase 2.1 主入口。
// 业务全部接通 GDI+ Preview 同款功能：autologin / WS / 托盘 / 拖文件 / Toast /
// 真后端登录-退出-改密 / status 同步 / login history / tags 增删 / 头像云同步。

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <windowsx.h>
#include <ShlObj.h>
#include <chrono>
#include <memory>

#pragma comment(lib, "shell32.lib")

#include "d2d_app.h"
#include "stages.h"
#include "palette.h"
#include "auth.h"
#include "hit.h"
#include "ui_main.h"
#include "chat.h"
#include "modals.h"
#include "fetch.h"
#include "persist.h"
#include "user_state.h"
#include "toast.h"
#include "ws_user.h"
#include "tray.h"
#include "i18n.h"
#include "steam.h"
#include "hwid.h"

#pragma comment(lib, "user32.lib")

using namespace launcher::d2d;

static D2DApp g_app;

static POINT physToDip(POINT phys) {
    float scale = g_app.dpi() / 96.0f;
    if (scale < 0.001f) scale = 1.0f;
    return { (LONG)(phys.x / scale + 0.5f), (LONG)(phys.y / scale + 0.5f) };
}

static bool inAuthOrMain() {
    return stages::g_stage == stages::Stage::Auth
        || stages::g_stage == stages::Stage::Main;
}

// 启动后异步：tags + 头像云同步
static void afterLogin(HWND hwnd) {
    persist::saveSession(g_session_token, g_user_id);
    ws::start(hwnd);
    chat::fetchOfficialChannels(hwnd);
    fetch::userTags(hwnd);
    fetch::remoteAvatar(hwnd);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_SIZE: {
            int w = LOWORD(lp), h = HIWORD(lp);
            if (w > 0 && h > 0) g_app.requestResize(w, h);
            return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: { PAINTSTRUCT ps; BeginPaint(hwnd, &ps); EndPaint(hwnd, &ps); return 0; }
        case WM_NCHITTEST: {
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
            if (modal::onMouseLDown(hwnd, g_mouse)) return 0;
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
            if (wp == VK_ESCAPE) {
                if (modal::onKey(hwnd, (int)wp, shift, ctrl)) return 0;
                if (chat::g_picker_open) {
                    chat::g_picker_open = false;
                    chat::g_picker_t.start(chat::g_picker_t.value(), 0, 0.18f,
                                           0, curve::easeOutCubic);
                    return 0;
                }
                if (stages::g_stage == stages::Stage::Main) {
                    // ESC 最小化到托盘，不真退出
                    tray::hideToTray(hwnd);
                    return 0;
                }
                PostQuitMessage(0);
                return 0;
            }
            if (modal::onKey(hwnd, (int)wp, shift, ctrl)) return 0;
            if (stages::g_stage == stages::Stage::Auth) {
                auth::onKey(hwnd, (int)wp, shift, ctrl);
                return 0;
            }
            if (stages::g_stage == stages::Stage::Main
                && stages::g_view == stages::View::Chat
                && chat::g_focus_composer) {
                chat::onKey(hwnd, (int)wp, shift, ctrl);
                return 0;
            }
            if (wp == 'D') { g_dark = !g_dark; persist::saveTheme(g_dark); return 0; }
            if (wp == 'S' && stages::g_stage == stages::Stage::Loading) {
                stages::g_skip_auth_after_loading = true;
                stages::g_time_in_stage = 1.5f;
                return 0;
            }
            return 0;
        }
        case WM_CHAR: {
            bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            if (modal::onChar(hwnd, (wchar_t)wp, ctrl)) return 0;
            if (stages::g_stage == stages::Stage::Auth) {
                auth::onChar(hwnd, (wchar_t)wp, ctrl);
                return 0;
            }
            if (stages::g_stage == stages::Stage::Main
                && stages::g_view == stages::View::Chat
                && chat::g_focus_composer) {
                chat::onChar(hwnd, (wchar_t)wp, ctrl);
                return 0;
            }
            return 0;
        }
        case WM_DROPFILES: {
            if (stages::g_stage == stages::Stage::Main
                && stages::g_view == stages::View::Chat) {
                HDROP drop = (HDROP)wp;
                UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
                for (UINT i = 0; i < n; ++i) {
                    wchar_t path[MAX_PATH];
                    if (DragQueryFileW(drop, i, path, MAX_PATH)) {
                        chat::appendMedia(path);
                    }
                }
                DragFinish(drop);
            }
            return 0;
        }
        case WM_APP + 1: {                     // Auth submit
            auth::handleSubmit(hwnd);
            return 0;
        }
        case WM_APP + 2: {                     // Auth submit result
            auth::onSubmitResult(hwnd, wp != 0);
            if (wp != 0) afterLogin(hwnd);
            return 0;
        }
        case WM_APP + 3: {                     // 头像上传结果
            toast::show(wp ? L"头像已同步到云端 ✓" : L"头像上传失败");
            return 0;
        }
        case WM_APP + 4: {                     // ChangePw result
            modal::onChangePwResult(wp != 0);
            if (wp) toast::show(L"密码已修改，请重新登录");
            return 0;
        }
        case WM_APP + 5: {                     // Chat official channels result
            chat::applyOfficialResult();
            return 0;
        }
        case WM_APP + 10: {                    // WS message arrived
            ws::drain();
            return 0;
        }
        case WM_APP + 15: {                    // user tags fetched
            return 0;
        }
        case WM_APP + 16: {                    // tag add result
            modal::onAddTagResult(wp != 0, (int)lp);
            return 0;
        }
        case WM_APP + 17: {                    // tag remove result
            fetch::userTags(hwnd);     // 重新拉
            return 0;
        }
        case WM_APP + 19: {                    // pack create result
            modal::onCreatePackResult(wp != 0);
            if (wp) toast::show(L"表情包已创建");
            return 0;
        }
        case WM_APP + 20: {                    // pack rename result
            modal::onRenamePackResult(wp != 0);
            if (wp) toast::show(L"已重命名");
            return 0;
        }
        case WM_APP + 21: {                    // chat picker → 新建表情包
            modal::openCreatePack();
            return 0;
        }
        case WM_APP + 23: {                    // 远程头像下载完成；lp = std::wstring*
            std::unique_ptr<std::wstring> p((std::wstring*)lp);
            if (p && !p->empty()) {
                g_avatar_path = *p;
                g_app.images().invalidate();   // 强制下次重新解码
                toast::show(L"头像已从云端同步 ✓");
            }
            return 0;
        }
        case WM_APP + 30: {                    // login history result; lp = std::string*
            std::unique_ptr<std::string> p((std::string*)lp);
            if (p) modal::onHistoryResult(*p);
            return 0;
        }
        case tray::kTrayCallbackMsg: {
            if (tray::onCallback(hwnd, wp, lp)) return 0;
            return 0;
        }
        case WM_RBUTTONDOWN:
            if (stages::g_stage == stages::Stage::Main) {
                tray::hideToTray(hwnd);
                return 0;
            }
            PostQuitMessage(0);
            return 0;
        case WM_DESTROY:
            ws::stop();
            tray::remove();
            PostQuitMessage(0);
            return 0;
        case WM_CLOSE:
            ws::stop();
            tray::remove();
            DestroyWindow(hwnd);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int APIENTRY wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // 加载持久化设置
    g_dark = persist::loadTheme(true);
    int lang = persist::loadLang((int)Lang::ZhCN);
    if (lang >= 0 && lang <= 2) g_lang = (Lang)lang;
    readSteamInfo();

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
        WS_EX_NOREDIRECTIONBITMAP | WS_EX_ACCEPTFILES,
        wc.lpszClassName, L"Launcher",
        WS_POPUP,
        (sw - W0) / 2, (sh - H0) / 2, W0, H0,
        nullptr, nullptr, inst, nullptr);
    if (!hwnd) return 1;
    DragAcceptFiles(hwnd, TRUE);

    RECT rc{};
    GetClientRect(hwnd, &rc);
    if (!g_app.init(hwnd, rc.right - rc.left, rc.bottom - rc.top)) {
        DestroyWindow(hwnd);
        return 1;
    }

    // 启动后尝试 autologin — 有 saved session 且 saved creds 时直接走主页
    {
        std::string tok, uid;
        if (persist::loadSession(tok, uid) && !tok.empty()) {
            g_session_token = tok;
            g_user_id = uid;
            stages::g_skip_auth_after_loading = true;
            // 头像如果之前下载过，本地路径还在
            wchar_t base[MAX_PATH] = {0};
            if (SHGetSpecialFolderPathW(nullptr, base, CSIDL_LOCAL_APPDATA, FALSE)) {
                std::wstring dir = std::wstring(base) + L"\\Launcher";
                for (const wchar_t* e : { L"png", L"jpg", L"jpeg", L"gif", L"webp", L"bmp" }) {
                    std::wstring p = dir + L"\\avatar." + e;
                    if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) {
                        g_avatar_path = p;
                        break;
                    }
                }
            }
            // 启动后异步刷新
            PostMessageW(hwnd, WM_APP + 50, 0, 0);   // 自定义：稍后 afterLogin
        }
    }

    stages::enterDotStage();
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    auto last = std::chrono::steady_clock::now();
    MSG msg{};
    bool quit = false;
    bool autologin_kicked = false;
    while (!quit) {
        DWORD r = g_app.waitFrame(100);
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { quit = true; break; }
            if (msg.message == WM_APP + 50 && !autologin_kicked) {
                autologin_kicked = true;
                afterLogin(hwnd);
                continue;
            }
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
        toast::tick(dt);
        stages::driveTransitions(g_app, sw, sh);

        if (g_app.beginFrame()) {
            stages::paint(g_app);
            g_app.endFrame();
        }
    }

    g_app.shutdown();
    return 0;
}
