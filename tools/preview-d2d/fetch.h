// 异步 fetch helpers — 1:1 复刻 GDI+ Preview 各 fetchXxx。
// 全部用 CreateThread 起后台线程，PostMessage 通知 UI。
#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <string>
#include <vector>
#include <mutex>

namespace launcher::d2d::fetch {

// GET /api/profile/tags → g_user_tags + WM_APP+15
void userTags(HWND notify);

// POST /api/profile/tags/add → WM_APP+16 (wp = success)
void addTag(HWND notify, const std::wstring& tag);

// POST /api/profile/tags/remove
void removeTag(HWND notify, const std::wstring& tag);

// GET /api/avatar/:uid → 写本地 → WM_APP+23 (lp = std::wstring* path)
void remoteAvatar(HWND notify);

// POST /api/profile/status → 后端写 + WS broadcast
void statusSync(const wchar_t* status_key);

// POST /api/auth/logout (best-effort 异步，不等结果)
void logout(const std::string& token);

// GET /api/profile/login-history → WM_APP+30 (lp = std::vector<HistRow>*)
void loginHistory(HWND notify);

// 头像上传 — 异步 multipart POST /api/profile/avatar → WM_APP+3 (wp = success)
void uploadAvatar(HWND notify, const std::wstring& path);

struct Listing {
    std::string  id;
    std::wstring title;
    std::wstring seller;
    int          price = 0;       // credits
    std::wstring summary;
};
extern std::vector<Listing> g_market_listings;
extern std::mutex g_market_mtx;

// GET /api/market/listings → 写 g_market_listings + WM_APP+33
void marketListings(HWND notify);

// POST /api/profile/update — 更新 nickname / status_text / bio 任一字段
// fields = JSON object 片段，e.g. "\"bio\":\"...\""
void profileUpdate(HWND notify, const std::string& fields);

// GET ip-api.com/json/ — 异步查 IP 地理位置 → 写 g_geo_country (WM_APP+38)
void geoIP(HWND notify);

// GET /api/profile/:uid — 拿别人的资料 (nickname/status/bio/avatar)
struct PeerProfile {
    std::wstring uid, username, nickname, status, status_text, bio;
    std::wstring avatar_path;
    bool loaded = false;
    std::wstring err;
};
extern PeerProfile g_peer;
extern std::mutex g_peer_mtx;
void peerProfile(HWND notify, const std::wstring& uid_or_nickname);

}  // namespace launcher::d2d::fetch
