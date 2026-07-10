// WebView2 嵌入 — Edge Chromium 控件，用于在 D2D 主窗口内播 Steam mp4 视频 /
// 任意 H264 H5 video。WebView2 是独立 HWND 子窗口（z-order 在 D2D 之上覆盖）。
//
// 依赖：
//   * SDK 头：third_party/webview2/build/native/include/WebView2.h
//   * Loader: third_party/webview2/build/native/x64/WebView2Loader.dll.lib
//   * Runtime：Win10/11 装 Edge 自带；没装可装 Evergreen Bootstrapper。
//
// 用法：
//   webview::ensure(hwnd);            // 启动后异步初始化
//   webview::setBounds(l,t,r,b);     // 设位置（物理像素）
//   webview::navigate(L"https://...");
//   webview::show(true / false);     // 隐藏 = 不显示 + 不暂停（保持播放后台）
//   webview::isReady();              // env + controller 都好了
#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <string>

namespace launcher::d2d::webview {

bool ensure(HWND parent);          // 异步 init；返回是否调用成功（不代表已 ready）
bool isReady();
void setBounds(int left, int top, int right, int bottom);
void navigate(const std::wstring& url);
// NavigateToString — 内嵌 HTML 字符串（用来包 <video> 等 H5 标签播本地视频）
void navigateHtml(const std::wstring& html);
void show(bool visible);
// 检测 WebView2 Runtime 是否可用 — Loader.dll 能 LoadLibrary + 至少创建 env
bool runtimeAvailable();

}  // namespace launcher::d2d::webview
