// 游戏封面图加载 — 从 exe 同目录或 ../../assets/images/games/ 找。
// build_d2d.bat 把 assets/images/games/cs2_header.jpg 复制到 dist/。
#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <string>

namespace launcher::d2d {

// 返回 cs2_header.jpg 的完整路径（找不到返回空）
inline std::wstring cs2HeaderPath() {
    static std::wstring cached;
    if (!cached.empty()) {
        if (GetFileAttributesW(cached.c_str()) != INVALID_FILE_ATTRIBUTES) return cached;
        cached.clear();
    }
    wchar_t exe[MAX_PATH] = {0};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir = exe;
    auto p = dir.find_last_of(L'\\');
    if (p != std::wstring::npos) dir = dir.substr(0, p);

    // 优先：exe 同目录
    std::wstring c1 = dir + L"\\cs2_header.jpg";
    if (GetFileAttributesW(c1.c_str()) != INVALID_FILE_ATTRIBUTES) { cached = c1; return c1; }

    // 备选 1：dist 上一层 assets (开发模式直接跑 dist/LauncherD2D.exe)
    std::wstring c2 = dir + L"\\..\\assets\\images\\games\\cs2_header.jpg";
    if (GetFileAttributesW(c2.c_str()) != INVALID_FILE_ATTRIBUTES) { cached = c2; return c2; }

    // 备选 2：跑 tools/preview-d2d/LauncherD2D.exe 时 ../../assets
    std::wstring c3 = dir + L"\\..\\..\\assets\\images\\games\\cs2_header.jpg";
    if (GetFileAttributesW(c3.c_str()) != INVALID_FILE_ATTRIBUTES) { cached = c3; return c3; }

    return {};
}

// 返回 cs2_header.mp4 的完整路径（找不到返回空）。
// 与 cs2HeaderPath() 同样的探测顺序：exe 同目录 → ../assets → ../../assets。
// build_d2d.bat 把 assets/images/games/cs2_header.mp4（若存在）复制到 dist/。
// 缺文件时上层会回退到静态 cs2_header.jpg，绝不空白。
inline std::wstring cs2VideoPath() {
    static std::wstring cached;
    if (!cached.empty()) {
        if (GetFileAttributesW(cached.c_str()) != INVALID_FILE_ATTRIBUTES) return cached;
        cached.clear();
    }
    wchar_t exe[MAX_PATH] = {0};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir = exe;
    auto p = dir.find_last_of(L'\\');
    if (p != std::wstring::npos) dir = dir.substr(0, p);

    std::wstring c1 = dir + L"\\cs2_header.mp4";
    if (GetFileAttributesW(c1.c_str()) != INVALID_FILE_ATTRIBUTES) { cached = c1; return c1; }

    std::wstring c2 = dir + L"\\..\\assets\\images\\games\\cs2_header.mp4";
    if (GetFileAttributesW(c2.c_str()) != INVALID_FILE_ATTRIBUTES) { cached = c2; return c2; }

    std::wstring c3 = dir + L"\\..\\..\\assets\\images\\games\\cs2_header.mp4";
    if (GetFileAttributesW(c3.c_str()) != INVALID_FILE_ATTRIBUTES) { cached = c3; return c3; }

    return {};
}

}  // namespace launcher::d2d
