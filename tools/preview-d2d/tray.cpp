#include "tray.h"

namespace launcher::d2d::tray {

namespace {
constexpr UINT kTrayUid = 1;
NOTIFYICONDATAW g_nid{};
bool g_added = false;
HMENU g_menu = nullptr;
bool g_balloon_shown = false;
}

void add(HWND hwnd) {
    if (g_added) return;
    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = hwnd;
    g_nid.uID = kTrayUid;
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_nid.uCallbackMessage = kTrayCallbackMsg;
    g_nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(g_nid.szTip, L"Launcher");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
    g_added = true;

    if (!g_menu) {
        g_menu = CreatePopupMenu();
        AppendMenuW(g_menu, MF_STRING, 1, L"显示主窗口");
        AppendMenuW(g_menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(g_menu, MF_STRING, 2, L"退出");
    }
}

void remove() {
    if (!g_added) return;
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    g_added = false;
    if (g_menu) { DestroyMenu(g_menu); g_menu = nullptr; }
}

void hideToTray(HWND hwnd) {
    add(hwnd);
    ShowWindow(hwnd, SW_HIDE);
    if (!g_balloon_shown) {
        g_balloon_shown = true;
        NOTIFYICONDATAW b{};
        b.cbSize = sizeof(b);
        b.hWnd = hwnd;
        b.uID = kTrayUid;
        b.uFlags = NIF_INFO;
        b.dwInfoFlags = NIIF_INFO;
        wcscpy_s(b.szInfoTitle, L"Launcher");
        wcscpy_s(b.szInfo, L"程序已最小化到托盘，右键退出");
        Shell_NotifyIconW(NIM_MODIFY, &b);
    }
}

void showFromTray(HWND hwnd) {
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
}

bool onCallback(HWND hwnd, WPARAM /*wp*/, LPARAM lp) {
    UINT msg = LOWORD(lp);
    if (msg == WM_LBUTTONUP) {
        showFromTray(hwnd);
        return true;
    }
    if (msg == WM_RBUTTONUP) {
        if (!g_menu) return true;
        POINT pt; GetCursorPos(&pt);
        SetForegroundWindow(hwnd);
        UINT cmd = TrackPopupMenu(g_menu,
            TPM_RIGHTBUTTON | TPM_RETURNCMD,
            pt.x, pt.y, 0, hwnd, nullptr);
        if (cmd == 1) showFromTray(hwnd);
        else if (cmd == 2) PostMessageW(hwnd, WM_CLOSE, 0, 0);
        return true;
    }
    return false;
}

}  // namespace launcher::d2d::tray
