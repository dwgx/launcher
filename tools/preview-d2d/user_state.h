// 用户登录后的全局状态 — UserInfo / UserStatus / session token / avatar 路径。
// 为简化，AvatarCache 走 D2DApp::images() 用文件路径作 key 缓存。
#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <string>
#include <atomic>
#include <vector>
#include <mutex>

namespace launcher::d2d {

struct UserInfo {
    std::wstring uid       = L"3277380";
    std::wstring username  = L"dwgx";
    std::wstring nickname  = L"dwgx";
    std::wstring email     = L"dwgx1337@outlook.com";
    std::wstring device_id = L"f8a1c2d4...e5b6";
    std::wstring expires   = L"2026-05-09";
    std::wstring status_text;          // 自定义状态消息（48 字以内）
    std::wstring bio;                   // 个人签名（240 字以内）
};

enum class UserStatus { Online, Busy, Away, Sleep, Offline };

extern UserInfo  g_user;
extern UserStatus g_status;

extern std::string g_session_token;
extern std::string g_user_id;

// 头像本地文件路径（已下载 / 上传过）；空则画首字母占位
extern std::wstring g_avatar_path;
extern std::vector<std::wstring> g_user_tags;
extern std::mutex g_user_tags_mtx;

// 异步 IP 地理位置 — fetch::geoIP() 启动后查 ip-api.com，写到这里
extern wchar_t g_geo_country[16];   // e.g. L"CN", L"US"，空表示未知

// Loading 阶段实时滚动日志（autologin 模式）
extern std::vector<std::wstring> g_login_log;
inline void logLine(const wchar_t* s) {
    g_login_log.emplace_back(s);
    if (g_login_log.size() > 8) g_login_log.erase(g_login_log.begin());
}

inline const wchar_t* statusKey(UserStatus s) {
    switch (s) {
    case UserStatus::Online:  return L"online";
    case UserStatus::Busy:    return L"busy";
    case UserStatus::Away:    return L"away";
    case UserStatus::Sleep:   return L"sleep";
    default:                  return L"offline";
    }
}
inline const wchar_t* statusLabel(UserStatus s) {
    switch (s) {
    case UserStatus::Online:  return L"在线";
    case UserStatus::Busy:    return L"繁忙";
    case UserStatus::Away:    return L"离开";
    case UserStatus::Sleep:   return L"睡眠";
    default:                  return L"离线";
    }
}

inline uint32_t statusColor(UserStatus s) {
    switch (s) {
    case UserStatus::Online:  return 0xFF4ADE80;
    case UserStatus::Busy:    return 0xFFE34B4B;
    case UserStatus::Away:    return 0xFFF5A524;
    case UserStatus::Sleep:   return 0xFF8B7BD9;
    default:                  return 0xFF6B6A67;
    }
}

}  // namespace launcher::d2d
