// Steam 集成 — 从 HKCU\Software\Valve\Steam 读 PersonaName + LastGameNameUsed。
// 1:1 复刻 tools/preview/loading_demo.cpp::SteamInfo + readSteamInfo。
#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <shellapi.h>
#include <string>

#pragma comment(lib, "shell32.lib")

namespace launcher::d2d {

struct SteamInfo {
    std::wstring persona;
    std::wstring last_game;
    std::wstring last_played;
    std::wstring playtime_label;
    bool resolved = false;
};

extern SteamInfo g_steam;

inline void readSteamInfo() {
    if (g_steam.resolved) return;
    g_steam.resolved = true;
    HKEY hk;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", 0, KEY_READ, &hk) == ERROR_SUCCESS) {
        wchar_t buf[256]; DWORD cb;
        cb = sizeof(buf);
        if (RegQueryValueExW(hk, L"AutoLoginUser", nullptr, nullptr, (LPBYTE)buf, &cb) == ERROR_SUCCESS) {
            g_steam.persona = buf;
        }
        cb = sizeof(buf);
        if (RegQueryValueExW(hk, L"LastGameNameUsed", nullptr, nullptr, (LPBYTE)buf, &cb) == ERROR_SUCCESS) {
            g_steam.last_game = buf;
        }
        RegCloseKey(hk);
    }
    if (g_steam.persona.empty()) g_steam.persona = L"未登录";
    if (g_steam.last_played.empty())
        g_steam.last_played = g_steam.last_game.empty() ? L"—" : g_steam.last_game;
    if (g_steam.playtime_label.empty()) g_steam.playtime_label = L"—";
}

inline void launchCS2() {
    // steam://rungameid/730 — Steam 协议启动 CS2
    ShellExecuteW(nullptr, L"open", L"steam://rungameid/730",
                  nullptr, nullptr, SW_SHOWNORMAL);
}

inline void openCS2Store() {
    ShellExecuteW(nullptr, L"open", L"https://store.steampowered.com/app/730/",
                  nullptr, nullptr, SW_SHOWNORMAL);
}

}  // namespace launcher::d2d
