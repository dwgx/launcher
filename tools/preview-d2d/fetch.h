// 异步 fetch helpers — 1:1 复刻 GDI+ Preview 各 fetchXxx。
// 全部用 CreateThread 起后台线程，PostMessage 通知 UI。
#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <cstdint>
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

// POST /api/community/checkin — 每日签到；body 只带 session_token（无用户文本）。
// 结果写 g_checkin + WM_APP+60 (wp = r.ok(), lp = r.status)。granted=当天首次点亮。
struct CheckinResult { int level = 1; int64_t xp = 0; bool granted = false; bool ok = false; };
extern CheckinResult g_checkin;
extern std::mutex g_checkin_mtx;
// 仅 UI 线程读写（hit 回调置 true，WM_APP+60 清 false）→ 无需原子；防止重复点触。
extern bool g_checkin_inflight;
void checkin(HWND notify);

// POST /api/auth/logout (best-effort 异步，不等结果)
void logout(const std::string& token);

// GET /api/profile/login-history → WM_APP+30 (lp = std::vector<HistRow>*)
void loginHistory(HWND notify);

// 头像上传 — 异步 multipart POST /api/profile/avatar → WM_APP+3 (wp = success)
void uploadAvatar(HWND notify, const std::wstring& path);

struct MediaUploadResult {
    bool ok = false;
    DWORD status = 0;
    int64_t media_id = 0;
    std::string sha256;
    std::string mime;
    std::string url;              // /api/media/<sha>/file.<ext>
    std::wstring local_path;      // source file path used for upload
    std::string blurhash;         // Wave3: BlurHash placeholder (may be empty)
    int width = 0;                // Wave3: intrinsic pixel width (0 if unknown)
    int height = 0;               // Wave3: intrinsic pixel height (0 if unknown)
    std::string error;
};

struct MediaDownloadResult {
    bool ok = false;
    DWORD status = 0;
    std::string sha256;
    std::string url;              // normalized /api/media URL, without session_token
    std::wstring path;            // cache path written/read
    std::string error;
};

// %LOCALAPPDATA%\Launcher\<scope>\, created if needed. Default scope is generic media.
std::wstring mediaCacheDir(const wchar_t* scope = L"media");

// Build the cache filename for an /api/media URL without downloading it.
std::wstring mediaCachePathForUrl(const std::string& media_url,
                                  const wchar_t* scope = L"media");

// Append session_token to /api/media URLs. Existing session_token is preserved.
std::string mediaUrlWithSessionToken(const std::string& media_url,
                                     const std::string& token = "");

// Synchronous helpers for chat/sticker/attachment flows.
MediaUploadResult uploadMediaFile(const std::wstring& path);
MediaDownloadResult downloadMediaToPath(const std::string& media_url,
                                        const std::wstring& local_path);
MediaDownloadResult downloadMediaToCache(const std::string& media_url,
                                         const wchar_t* scope = L"media");

// JSON object value for /api/chat/send payload; caller should not wrap this in quotes.
std::string mediaPayloadJson(const MediaUploadResult& media);

struct Listing {
    std::string  id;
    std::wstring title;
    std::wstring seller;
    int          price = 0;       // credits
    std::wstring summary;
};
extern std::vector<Listing> g_market_listings;
extern bool g_market_loaded;
extern std::mutex g_market_mtx;

// GET /api/market/listings → 写 g_market_listings + WM_APP+33
void marketListings(HWND notify);

// 商品详情 — GET /api/market/listings/:id 拿回后填这个，供 detail modal 渲染。
// price_cents 是 i64，必须用 int64 存（大额价格 int 会溢出）。
struct ListingDetail {
    std::string  id;
    std::wstring title;
    std::wstring description;
    std::wstring category;
    std::wstring item_type;
    std::wstring seller_id;
    std::wstring status;
    int64_t      price_cents = 0;
    int          purchase_count = 0;
    int          rating_count = 0;
    float        rating_avg = 0.0f;
    bool         loaded = false;
    std::string  error;              // 非空 = 拉取失败（HTTP body 前 120 字）
};
extern ListingDetail g_market_detail;
extern std::mutex g_market_detail_mtx;

// purchase / review 的结果暂存，供 UI 线程做 toast（区分 402 余额不足 vs 400/404）。
struct MarketActionResult {
    DWORD       status = 0;
    std::string msg;                 // 失败时 HTTP body 前 120 字
};
extern MarketActionResult g_market_action;
extern std::mutex g_market_action_mtx;

// GET /api/market/listings/:id → 写 g_market_detail + WM_APP+62 (wp = success)
void getListing(HWND notify, const std::string& id);

// POST /api/market/purchase → WM_APP+63 (wp = success)。非幂等，UI 需在点击后禁用按钮。
void purchaseListing(HWND notify, const std::string& listing_id);

// POST /api/market/review → WM_APP+64 (wp = success)。204 空 body，只看 r.ok()。
// order_id 省略 → 后端自动选评价者最近一笔 delivered 订单。body 是自由文本，会 jsonEscape。
void reviewListing(HWND notify, const std::string& listing_id, int rating,
                   const std::wstring& body_text);

enum class ProfileUpdateKind { Unknown = 0, StatusText = 1, Bio = 2, Nickname = 3 };

// POST /api/profile/update — 更新 nickname / status_text / bio 任一字段
// fields = JSON object 片段，e.g. "\"bio\":\"...\""
void profileUpdate(HWND notify, const std::string& fields);
void profileUpdate(HWND notify, ProfileUpdateKind kind, const std::string& fields);

// POST /api/profile/nickname — 单独的改昵称端点（有冷却限流）。
// new_nickname_escaped 必须已经过 net::jsonEscape。完成后 PostMessage WM_APP+61，
// wParam = HTTP 状态码（204 成功 / 429 冷却中 / 400 长度非法 / 其他失败）。
void changeNickname(HWND notify, const std::string& new_nickname_escaped);

// GET ip-api.com/json/ — 异步查 IP 地理位置 → 写 g_geo_country (WM_APP+38)
void geoIP(HWND notify);

// GET /api/profile — 拿自己最新的 nickname / status / status_text / bio / uid
// 异步线程，完成后 PostMessage WM_APP+54 让 main 把结果写回 g_user / g_status
struct MyProfileSnapshot {
    std::wstring nickname, uid, username, status, status_text, bio, role, role_label;
    std::wstring tier;
    int64_t tier_expires_at = 0;
    bool is_admin = false;
    bool subscribed = false;
    bool loaded = false;
};
extern MyProfileSnapshot g_pending_my_profile;
extern std::mutex g_my_profile_mtx;
void myProfile(HWND notify);
void applyMyProfileResult();

// GET /api/profile/peer/:key — 拿别人的资料 (nickname/status/bio/avatar)
// cache key 使用真实 user_id / uid / username / peer_key；WS status 可通过
// updatePeerStatus() 先落缓存，后续主线接 ws_user 时不需要知道 UI modal 状态。
struct PeerProfile {
    std::wstring cache_key;
    std::wstring uid, username, nickname, status, status_text, bio;
    std::wstring role, role_label;
    std::wstring avatar_url;
    std::wstring avatar_path;
    std::vector<std::wstring> tags;     // 个人标签
    bool loaded = false;
    bool loading = false;
    std::wstring err;
};
extern PeerProfile g_peer;
extern std::mutex g_peer_mtx;
std::wstring normalizePeerKey(const std::wstring& key);
PeerProfile peerProfileCached(const std::wstring& key);
void updatePeerStatus(const std::wstring& key, const std::wstring& status);
void peerProfile(HWND notify, const std::wstring& uid_or_nickname);

struct ModerationMemberState {
    std::wstring target_user_id;
    std::wstring target_label;
    bool active = false;
    int64_t mute_id = 0;
    int64_t muted_until = 0;
    std::wstring reason;
    std::wstring muted_by_label;
    bool can_mute = false;
    bool can_unmute = false;
    bool is_super_admin = false;
    bool loaded = false;
    bool ok = false;
    std::wstring error;
};
extern ModerationMemberState g_moderation_member;
extern std::mutex g_moderation_mtx;

void moderationMember(HWND notify, const std::string& chat_id, const std::wstring& target_user_id);
void muteUser(HWND notify, const std::string& chat_id, const std::wstring& target_user_id,
              int64_t duration_seconds, const std::wstring& reason);
void unmuteUser(HWND notify, const std::string& chat_id, const std::wstring& target_user_id,
                const std::wstring& reason);

}  // namespace launcher::d2d::fetch
