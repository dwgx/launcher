// 系统托盘 — 1:1 复刻 tools/preview/loading_demo.cpp::trayAdd / removeTray。
// 右键弹菜单 (显示主窗口 / 退出)；ESC 最小化到托盘 + 第一次 balloon 提示。
#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <shellapi.h>

#pragma comment(lib, "shell32.lib")

namespace launcher::d2d::tray {

constexpr UINT kTrayCallbackMsg = WM_APP + 100;

void add(HWND hwnd);
void remove();
// 最小化到托盘（隐藏窗口 + 第一次 balloon 提示）
void hideToTray(HWND hwnd);
void showFromTray(HWND hwnd);
// 处理 WM_APP+100；返回 true = 已处理
bool onCallback(HWND hwnd, WPARAM wp, LPARAM lp);

}  // namespace launcher::d2d::tray
