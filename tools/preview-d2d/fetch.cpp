// 异步 fetch helpers 实现 — 详见 fetch.h。

#include "fetch.h"
#include "net.h"
#include "user_state.h"
#include "i18n.h"

#include <memory>
#include <mutex>
#include <ShlObj.h>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <ctime>
#include <cwctype>
#include <unordered_map>

#pragma comment(lib, "shell32.lib")

namespace launcher::d2d::fetch {

std::vector<Listing> g_market_listings;
bool g_market_loaded = false;
std::mutex g_market_mtx;
PeerProfile g_peer;
std::mutex g_peer_mtx;
MyProfileSnapshot g_pending_my_profile;
std::mutex g_my_profile_mtx;
ModerationMemberState g_moderation_member;
std::mutex g_moderation_mtx;

namespace {
struct StrArg { std::wstring s; HWND h; };
struct VoidArg { HWND h; };
struct LogoutArg { std::string tok; };
struct AvatarArg { std::wstring path; HWND h; };
struct AvatarDownload {
    bool ok = false;
    std::wstring path;
};

std::wstring utf8ToW(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(n - 1, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}
std::string wToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s(n - 1, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring asciiToW(const std::string& s) {
    std::wstring w;
    w.reserve(s.size());
    for (char c : s) w.push_back((wchar_t)(unsigned char)c);
    return w;
}

std::wstring sanitizeFilePart(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size());
    for (wchar_t c : s) {
        bool ok = (c >= L'0' && c <= L'9')
               || (c >= L'a' && c <= L'z')
               || (c >= L'A' && c <= L'Z')
               || c == L'-' || c == L'_';
        out.push_back(ok ? c : L'_');
    }
    return out.empty() ? L"unknown" : out;
}

std::wstring trimKey(const std::wstring& s) {
    size_t b = 0;
    while (b < s.size() && iswspace(s[b])) ++b;
    size_t e = s.size();
    while (e > b && iswspace(s[e - 1])) --e;
    return s.substr(b, e - b);
}

std::wstring launcherDataDir() {
    wchar_t base[MAX_PATH] = {0};
    if (!SHGetSpecialFolderPathW(nullptr, base, CSIDL_LOCAL_APPDATA, FALSE)) return {};
    std::wstring dir = std::wstring(base) + L"\\Launcher";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

std::wstring avatarDir(const wchar_t* scope) {
    std::wstring dir = launcherDataDir();
    if (dir.empty()) return {};
    dir += L"\\avatars";
    CreateDirectoryW(dir.c_str(), nullptr);
    dir += L"\\";
    dir += scope;
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

const char* avatarExtFromBody(const std::string& body) {
    if (body.size() >= 3 && (BYTE)body[0] == 0xFF && (BYTE)body[1] == 0xD8) return "jpg";
    if (body.size() >= 4 && body[0] == 'G' && body[1] == 'I' && body[2] == 'F') return "gif";
    if (body.size() >= 12 && body[8] == 'W' && body[9] == 'E' && body[10] == 'B' && body[11] == 'P') return "webp";
    if (body.size() >= 2 && body[0] == 'B' && body[1] == 'M') return "bmp";
    return "png";
}

void clearAvatarFiles(const std::wstring& dir, const std::wstring& stem) {
    if (dir.empty() || stem.empty()) return;
    for (const wchar_t* e : { L"png", L"jpg", L"jpeg", L"gif", L"webp", L"bmp" }) {
        std::wstring p = dir + L"\\" + stem + L"." + e;
        DeleteFileW(p.c_str());
    }
}

bool writeFileBytes(const std::wstring& path, const std::string& body) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD wn = 0;
    BOOL ok = WriteFile(f, body.data(), (DWORD)body.size(), &wn, nullptr);
    CloseHandle(f);
    return ok && wn == body.size();
}

bool writeFileBytes(const std::wstring& path, const std::vector<BYTE>& body) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD wn = 0;
    BOOL ok = WriteFile(f, body.data(), (DWORD)body.size(), &wn, nullptr);
    CloseHandle(f);
    return ok && wn == body.size();
}

AvatarDownload downloadAvatarTo(const std::string& api_path, const std::wstring& dir, const std::wstring& stem) {
    AvatarDownload out;
    if (api_path.empty() || dir.empty() || stem.empty()) return out;
    std::wstring wurl = asciiToW(api_path);
    auto r = net::request(L"GET", wurl.c_str(), {}, L"");
    if (!r.ok() || r.body.empty()) {
        clearAvatarFiles(dir, stem);
        return out;
    }
    const char* ext = avatarExtFromBody(r.body);
    clearAvatarFiles(dir, stem);
    std::wstring path = dir + L"\\" + stem + L"." + asciiToW(ext);
    if (!writeFileBytes(path, r.body)) return out;
    out.ok = true;
    out.path = std::move(path);
    return out;
}

std::string normalizeAvatarUrl(std::string url) {
    if (url.empty()) return {};
    if (url.rfind("/api/", 0) == 0) return url;
    if (url.rfind("/avatar/", 0) == 0) return "/api" + url;
    auto p = url.find("/api/");
    if (p != std::string::npos) return url.substr(p);
    return url;
}

std::wstring avatarStemFromUrlOrKey(const std::string& url, const std::wstring& key) {
    std::string norm = normalizeAvatarUrl(url);
    auto slash = norm.find_last_of('/');
    if (slash != std::string::npos && slash + 1 < norm.size()) {
        return sanitizeFilePart(asciiToW(norm.substr(slash + 1)));
    }
    return sanitizeFilePart(key);
}

std::string normalizeMediaUrl(std::string url) {
    if (url.empty()) return {};
    auto q = url.find('?');
    if (q != std::string::npos) url = url.substr(0, q);
    if (url.rfind("/api/media/", 0) == 0) return url;
    if (url.rfind("/media/", 0) == 0) return "/api" + url;
    auto p = url.find("/api/media/");
    if (p != std::string::npos) return url.substr(p);
    return url;
}

std::string shaFromMediaUrl(const std::string& media_url) {
    std::string norm = normalizeMediaUrl(media_url);
    const std::string marker = "/api/media/";
    auto p = norm.find(marker);
    if (p == std::string::npos) return {};
    p += marker.size();
    auto e = norm.find('/', p);
    if (e == std::string::npos || e <= p) return {};
    std::string sha = norm.substr(p, e - p);
    if (sha.size() != 64) return {};
    for (char c : sha) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
            return {};
        }
    }
    std::transform(sha.begin(), sha.end(), sha.begin(),
                   [](unsigned char c) { return (char)tolower(c); });
    return sha;
}

std::string extFromPath(const std::wstring& path) {
    auto dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos || dot + 1 >= path.size()) return {};
    std::wstring ext_w = path.substr(dot + 1);
    for (auto& c : ext_w) c = (wchar_t)towlower(c);
    return wToUtf8(ext_w);
}

std::string extFromMediaUrl(const std::string& media_url) {
    std::string norm = normalizeMediaUrl(media_url);
    auto slash = norm.find_last_of('/');
    auto dot = norm.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return "bin";
    std::string ext = norm.substr(dot + 1);
    if (ext.empty() || ext.size() > 8) return "bin";
    for (char& c : ext) c = (char)tolower((unsigned char)c);
    return ext;
}

std::string mimeFromExtension(const std::wstring& path) {
    std::string ext = extFromPath(path);
    if (ext == "png") return "image/png";
    if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
    if (ext == "gif") return "image/gif";
    if (ext == "webp") return "image/webp";
    if (ext == "mp4") return "video/mp4";
    if (ext == "webm") return "video/webm";
    return {};
}

std::wstring filenameFromPath(const std::wstring& path) {
    auto sl = path.find_last_of(L"\\/");
    if (sl == std::wstring::npos || sl + 1 >= path.size()) return L"file.bin";
    return path.substr(sl + 1);
}

bool readFileBytes(const std::wstring& path, std::vector<BYTE>* out) {
    if (!out) return false;
    out->clear();
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER li{};
    if (!GetFileSizeEx(f, &li) || li.QuadPart <= 0 || li.QuadPart > 100LL * 1024 * 1024) {
        CloseHandle(f);
        return false;
    }
    out->resize((size_t)li.QuadPart);
    DWORD rd = 0;
    BOOL ok = ReadFile(f, out->data(), (DWORD)out->size(), &rd, nullptr);
    CloseHandle(f);
    if (!ok || rd != out->size()) {
        out->clear();
        return false;
    }
    return true;
}

std::wstring bestPeerKey(const PeerProfile& p, const std::wstring& fallback) {
    if (!p.cache_key.empty()) return p.cache_key;
    if (!p.uid.empty()) return p.uid;
    if (!p.username.empty()) return p.username;
    return fallback;
}

UserStatus statusFromKey(const std::wstring& status) {
    if      (status == L"online")  return UserStatus::Online;
    else if (status == L"busy")    return UserStatus::Busy;
    else if (status == L"away")    return UserStatus::Away;
    else if (status == L"sleep")   return UserStatus::Sleep;
    else if (status == L"offline") return UserStatus::Offline;
    return g_status;
}

std::unordered_map<std::wstring, PeerProfile> g_peer_cache;
}

void userTags(HWND notify) {
    if (g_session_token.empty()) return;
    auto* a = new VoidArg{ notify };
    CreateThread(nullptr, 0, [](LPVOID lp) -> DWORD {
        std::unique_ptr<VoidArg> a((VoidArg*)lp);
        std::string url = "/api/profile/tags?session_token=" + g_session_token;
        std::wstring wurl(url.begin(), url.end());
        auto r = net::request(L"GET", wurl.c_str(), {}, L"");
        if (!r.ok()) return 0;
        std::vector<std::wstring> tags;
        auto p1 = r.body.find("\"tags\":[");
        if (p1 == std::string::npos) return 0;
        size_t pos = p1 + 8;
        while (true) {
            auto q1 = r.body.find('"', pos);
            if (q1 == std::string::npos) break;
            auto q2 = r.body.find('"', q1 + 1);
            if (q2 == std::string::npos) break;
            std::string s = r.body.substr(q1 + 1, q2 - q1 - 1);
            tags.push_back(utf8ToW(s));
            pos = q2 + 1;
            if (pos < r.body.size() && r.body[pos] == ']') break;
        }
        {
            std::lock_guard<std::mutex> lk(g_user_tags_mtx);
            g_user_tags = std::move(tags);
        }
        PostMessageW(a->h, WM_APP + 15, 1, 0);
        return 0;
    }, a, 0, nullptr);
}

void addTag(HWND notify, const std::wstring& tag) {
    if (g_session_token.empty()) return;
    auto* a = new StrArg{ tag, notify };
    CreateThread(nullptr, 0, [](LPVOID lp) -> DWORD {
        std::unique_ptr<StrArg> a((StrArg*)lp);
        std::string body = "{\"session_token\":\"" + g_session_token
                         + "\",\"tag\":\"" + net::jsonEscape(a->s) + "\"}";
        auto r = net::postJson(L"/api/profile/tags/add", body);
        PostMessageW(a->h, WM_APP + 16, r.ok() ? 1 : 0, (LPARAM)(intptr_t)r.status);
        return 0;
    }, a, 0, nullptr);
}

void removeTag(HWND notify, const std::wstring& tag) {
    if (g_session_token.empty()) return;
    auto* a = new StrArg{ tag, notify };
    CreateThread(nullptr, 0, [](LPVOID lp) -> DWORD {
        std::unique_ptr<StrArg> a((StrArg*)lp);
        std::string body = "{\"session_token\":\"" + g_session_token
                         + "\",\"tag\":\"" + net::jsonEscape(a->s) + "\"}";
        net::postJson(L"/api/profile/tags/remove", body);
        PostMessageW(a->h, WM_APP + 17, 1, 0);
        return 0;
    }, a, 0, nullptr);
}

void remoteAvatar(HWND notify) {
    if (g_session_token.empty() || g_user_id.empty()) return;
    struct A { std::string uid; HWND h; };
    auto* a = new A{ g_user_id, notify };
    CreateThread(nullptr, 0, [](LPVOID lp) -> DWORD {
        std::unique_ptr<A> a((A*)lp);
        std::string url = "/api/avatar/" + a->uid;
        std::wstring dir = avatarDir(L"self");
        std::wstring stem = sanitizeFilePart(asciiToW(a->uid));
        AvatarDownload got = downloadAvatarTo(url, dir, stem);
        auto* p_arg = new std::wstring(std::move(got.path));
        PostMessageW(a->h, WM_APP + 23, got.ok ? 1 : 0, (LPARAM)p_arg);
        return 0;
    }, a, 0, nullptr);
}

void statusSync(const wchar_t* status_key) {
    if (g_session_token.empty() || !status_key) return;
    struct A { std::wstring k; };
    auto* a = new A{ status_key };
    CreateThread(nullptr, 0, [](LPVOID lp) -> DWORD {
        std::unique_ptr<A> a((A*)lp);
        std::string body = "{\"session_token\":\"" + g_session_token
                         + "\",\"status\":\"" + net::jsonEscape(a->k) + "\"}";
        net::postJson(L"/api/profile/status", body);
        return 0;
    }, a, 0, nullptr);
}

void logout(const std::string& token) {
    if (token.empty()) return;
    auto* a = new LogoutArg{ token };
    CreateThread(nullptr, 0, [](LPVOID lp) -> DWORD {
        std::unique_ptr<LogoutArg> a((LogoutArg*)lp);
        std::string body = "{\"session_token\":\"" + a->tok + "\"}";
        net::postJson(L"/api/auth/logout", body);
        return 0;
    }, a, 0, nullptr);
}

void loginHistory(HWND notify) {
    if (g_session_token.empty()) return;
    auto* a = new VoidArg{ notify };
    CreateThread(nullptr, 0, [](LPVOID lp) -> DWORD {
        std::unique_ptr<VoidArg> a((VoidArg*)lp);
        // 后端是 GET（GDI+ Preview 没接通过这个端点；试 GET 失败再 POST）
        std::string url = "/api/profile/login-history?session_token=" + g_session_token;
        std::wstring wurl(url.begin(), url.end());
        auto r = net::request(L"GET", wurl.c_str(), {}, L"");
        if (!r.ok()) {
            // 退到 POST
            std::string body = "{\"session_token\":\"" + g_session_token + "\"}";
            r = net::postJson(L"/api/profile/login-history", body);
        }
        if (!r.ok()) return 0;
        auto* p = new std::string(std::move(r.body));
        PostMessageW(a->h, WM_APP + 30, 1, (LPARAM)p);
        return 0;
    }, a, 0, nullptr);
}

void marketListings(HWND notify) {
    auto* a = new VoidArg{ notify };
    {
        std::lock_guard<std::mutex> lk(g_market_mtx);
        g_market_loaded = false;
        g_market_listings.clear();
    }
    CreateThread(nullptr, 0, [](LPVOID lp) -> DWORD {
        std::unique_ptr<VoidArg> a((VoidArg*)lp);
        // 公共端点，不需 session
        auto r = net::request(L"GET", L"/api/market/listings", {}, L"");
        std::vector<Listing> tmp;
        if (r.ok()) {
            size_t pos = 0;
            while (true) {
                auto ob = r.body.find('{', pos);
                if (ob == std::string::npos) break;
                auto cb = r.body.find('}', ob);
                if (cb == std::string::npos) break;
                std::string obj = r.body.substr(ob, cb - ob + 1);
                Listing l;
                l.id     = net::jsonStr(obj, "id");
                l.title  = utf8ToW(net::jsonStr(obj, "title"));
                l.seller = utf8ToW(net::jsonStr(obj, "seller"));
                l.price  = (int)net::jsonInt(obj, "price");
                l.summary= utf8ToW(net::jsonStr(obj, "summary"));
                if (!l.id.empty()) tmp.push_back(std::move(l));
                pos = cb + 1;
            }
        }
        {
            std::lock_guard<std::mutex> lk(g_market_mtx);
            g_market_listings = std::move(tmp);
            g_market_loaded = true;
        }
        PostMessageW(a->h, WM_APP + 33, 0, 0);
        return 0;
    }, a, 0, nullptr);
}

void profileUpdate(HWND notify, ProfileUpdateKind kind, const std::string& fields) {
    if (g_session_token.empty()) return;
    struct A { std::string f; HWND h; ProfileUpdateKind k; };
    auto* a = new A{ fields, notify, kind };
    CreateThread(nullptr, 0, [](LPVOID lp) -> DWORD {
        std::unique_ptr<A> a((A*)lp);
        std::string body = "{\"session_token\":\"" + g_session_token + "\"," + a->f + "}";
        auto r = net::postJson(L"/api/profile/update", body);
        PostMessageW(a->h, WM_APP + 35, r.ok() ? 1 : 0, (LPARAM)(int)a->k);
        return 0;
    }, a, 0, nullptr);
}

void profileUpdate(HWND notify, const std::string& fields) {
    profileUpdate(notify, ProfileUpdateKind::Unknown, fields);
}

std::wstring normalizePeerKey(const std::wstring& key) {
    return trimKey(key);
}

PeerProfile peerProfileCached(const std::wstring& key) {
    std::wstring norm = normalizePeerKey(key);
    if (norm.empty()) return {};
    std::lock_guard<std::mutex> lk(g_peer_mtx);
    auto it = g_peer_cache.find(norm);
    if (it != g_peer_cache.end()) return it->second;
    return {};
}

void updatePeerStatus(const std::wstring& key, const std::wstring& status) {
    std::wstring norm = normalizePeerKey(key);
    if (norm.empty() || status.empty()) return;
    std::lock_guard<std::mutex> lk(g_peer_mtx);
    bool matched = false;
    for (auto& kv : g_peer_cache) {
        PeerProfile& p = kv.second;
        if (normalizePeerKey(kv.first) == norm
            || normalizePeerKey(p.cache_key) == norm
            || normalizePeerKey(p.uid) == norm
            || normalizePeerKey(p.username) == norm) {
            p.status = status;
            matched = true;
        }
    }
    if (!matched) {
        auto& p = g_peer_cache[norm];
        p.cache_key = norm;
        p.status = status;
    }
    if (normalizePeerKey(g_peer.cache_key) == norm
        || normalizePeerKey(g_peer.uid) == norm
        || normalizePeerKey(g_peer.username) == norm) {
        g_peer.status = status;
    }
}

void peerProfile(HWND notify, const std::wstring& uid_or_nickname) {
    std::wstring cache_key = normalizePeerKey(uid_or_nickname);
    if (cache_key.empty()) return;
    {
        std::lock_guard<std::mutex> lk(g_peer_mtx);
        auto it = g_peer_cache.find(cache_key);
        if (it != g_peer_cache.end()) {
            g_peer = it->second;
            g_peer.loading = true;
            g_peer.err.clear();
            it->second.loading = true;
        } else {
            g_peer = PeerProfile{};
            g_peer.cache_key = cache_key;
            g_peer.uid = uid_or_nickname;
            g_peer.loading = true;
            g_peer_cache[cache_key] = g_peer;
        }
    }
    struct A { std::wstring k; HWND h; };
    auto* a = new A{ cache_key, notify };
    CreateThread(nullptr, 0, [](LPVOID lp) -> DWORD {
        std::unique_ptr<A> a((A*)lp);
        std::string key = wToUtf8(a->k);
        // backend route: /api/profile/peer/:key (key = uid 7-digit / username / user_id uuid)
        std::string url = "/api/profile/peer/" + key;
        if (!g_session_token.empty()) url += "?session_token=" + g_session_token;
        std::wstring wurl(url.begin(), url.end());
        auto r = net::request(L"GET", wurl.c_str(), {}, L"");
        PeerProfile next;
        next.cache_key = a->k;
        next.uid = a->k;
        if (r.ok()) {
            next.uid         = utf8ToW(net::jsonStr(r.body, "uid"));
            if (next.uid.empty()) next.uid = a->k;
            next.username    = utf8ToW(net::jsonStr(r.body, "username"));
            next.nickname    = utf8ToW(net::jsonStr(r.body, "nickname"));
            next.status      = utf8ToW(net::jsonStr(r.body, "status"));
            next.status_text = utf8ToW(net::jsonStr(r.body, "status_text"));
            next.bio         = utf8ToW(net::jsonStr(r.body, "bio"));
            next.role        = utf8ToW(net::jsonStr(r.body, "role"));
            next.role_label  = utf8ToW(net::jsonStr(r.body, "role_label"));
            std::string avatar_url = normalizeAvatarUrl(net::jsonStr(r.body, "avatar_url"));
            next.avatar_url = utf8ToW(avatar_url);
            if (!avatar_url.empty()) {
                AvatarDownload got = downloadAvatarTo(
                    avatar_url,
                    avatarDir(L"peers"),
                    avatarStemFromUrlOrKey(avatar_url, bestPeerKey(next, a->k)));
                if (got.ok) next.avatar_path = std::move(got.path);
            }
            // 简单解析 "tags":["a","b",...]
            auto p1 = r.body.find("\"tags\":[");
            if (p1 != std::string::npos) {
                size_t pos = p1 + 8;
                size_t end = r.body.find(']', pos);
                if (end == std::string::npos) end = r.body.size();
                while (pos < end) {
                    auto q1 = r.body.find('"', pos);
                    if (q1 == std::string::npos || q1 >= end) break;
                    auto q2 = r.body.find('"', q1 + 1);
                    if (q2 == std::string::npos || q2 > end) break;
                    next.tags.push_back(utf8ToW(
                        r.body.substr(q1 + 1, q2 - q1 - 1)));
                    pos = q2 + 1;
                }
            }
            next.loaded = true;
            next.loading = false;
        } else {
            next.err = r.body.empty() ? trW("auth.err_no_server") : utf8ToW(r.body.substr(0, 80));
            next.loaded = true;
            next.loading = false;
        }
        {
            std::lock_guard<std::mutex> lk(g_peer_mtx);
            bool current_request =
                normalizePeerKey(g_peer.cache_key) == a->k
                || normalizePeerKey(g_peer.uid) == a->k
                || normalizePeerKey(g_peer.username) == a->k;
            if (r.ok()) {
                g_peer_cache[a->k] = next;
                if (!next.uid.empty()) g_peer_cache[normalizePeerKey(next.uid)] = next;
                if (!next.username.empty()) g_peer_cache[normalizePeerKey(next.username)] = next;
            } else if (auto it = g_peer_cache.find(a->k); it != g_peer_cache.end()) {
                it->second.loading = false;
                it->second.err = next.err;
            }
            if (current_request) g_peer = std::move(next);
        }
        PostMessageW(a->h, WM_APP + 36, r.ok() ? 1 : 0, 0);
        return 0;
    }, a, 0, nullptr);
}

void myProfile(HWND notify) {
    if (g_session_token.empty()) return;
    auto* a = new VoidArg{ notify };
    CreateThread(nullptr, 0, [](LPVOID lp) -> DWORD {
        std::unique_ptr<VoidArg> a((VoidArg*)lp);
        std::string url = "/api/profile?session_token=" + g_session_token;
        std::wstring wurl(url.begin(), url.end());
        auto r = net::request(L"GET", wurl.c_str(), {}, L"");
        if (!r.ok()) {
            PostMessageW(a->h, WM_APP + 54, 0, (LPARAM)r.status);
            return 0;
        }
        // 解析关键字段并写回 g_user / g_status
        MyProfileSnapshot next;
        next.nickname    = utf8ToW(net::jsonStr(r.body, "nickname"));
        next.uid         = utf8ToW(net::jsonStr(r.body, "uid"));
        next.username    = utf8ToW(net::jsonStr(r.body, "username"));
        next.status      = utf8ToW(net::jsonStr(r.body, "status"));
        next.status_text = utf8ToW(net::jsonStr(r.body, "status_text"));
        next.bio         = utf8ToW(net::jsonStr(r.body, "bio"));
        next.role        = utf8ToW(net::jsonStr(r.body, "role"));
        next.role_label  = utf8ToW(net::jsonStr(r.body, "role_label"));
        next.tier        = utf8ToW(net::jsonStr(r.body, "tier"));
        next.tier_expires_at = net::jsonInt(r.body, "tier_expires_at");
        next.is_admin    = net::jsonRaw(r.body, "is_admin") == "true";
        next.subscribed  = !next.tier.empty()
                         && (next.tier_expires_at <= 0 || next.tier_expires_at > (int64_t)time(nullptr));
        next.loaded      = true;
        {
            std::lock_guard<std::mutex> lk(g_my_profile_mtx);
            g_pending_my_profile = std::move(next);
        }
        PostMessageW(a->h, WM_APP + 54, 1, 0);
        return 0;
    }, a, 0, nullptr);
}

void applyMyProfileResult() {
    MyProfileSnapshot next;
    {
        std::lock_guard<std::mutex> lk(g_my_profile_mtx);
        next = std::move(g_pending_my_profile);
        g_pending_my_profile = MyProfileSnapshot{};
    }
    if (!next.loaded) return;
    if (!next.nickname.empty()) g_user.nickname = next.nickname;
    if (!next.uid.empty())      g_user.uid      = next.uid;
    if (!next.username.empty()) g_user.username = next.username;
    g_user.status_text = next.status_text;
    g_user.bio = next.bio;
    g_user.role = next.role;
    g_user.role_label = next.role_label;
    g_user.is_admin = next.is_admin;
    g_user.subscribed = next.subscribed;
    if (!next.tier.empty()) g_user.expires = next.tier;
    g_status = statusFromKey(next.status);
}

void geoIP(HWND notify) {
    auto* a = new VoidArg{ notify };
    CreateThread(nullptr, 0, [](LPVOID lp) -> DWORD {
        std::unique_ptr<VoidArg> a((VoidArg*)lp);
        // ip-api.com 免费 IP 地理位置，无需 API key (45 req/min)
        // 返回 {"country":"China","countryCode":"CN","region":"BJ","city":"Beijing",...}
        // 启用 VPN 时返回 VPN 出口的国家
        auto r = net::requestAny(L"ip-api.com", 80, L"/json/", false);
        if (!r.ok()) return 0;
        std::string code = net::jsonStr(r.body, "countryCode");
        if (code.empty()) return 0;
        // 转 wstring 写到 g_geo_country
        if (code.size() < 16) {
            for (size_t i = 0; i < code.size(); ++i) g_geo_country[i] = (wchar_t)code[i];
            g_geo_country[code.size()] = 0;
        }
        PostMessageW(a->h, WM_APP + 38, 0, 0);
        return 0;
    }, a, 0, nullptr);
}

std::wstring mediaCacheDir(const wchar_t* scope) {
    std::wstring dir = launcherDataDir();
    if (dir.empty()) return {};
    dir += L"\\";
    dir += (scope && *scope) ? scope : L"media";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

std::wstring mediaCachePathForUrl(const std::string& media_url, const wchar_t* scope) {
    std::wstring dir = mediaCacheDir(scope);
    if (dir.empty()) return {};
    std::string sha = shaFromMediaUrl(media_url);
    if (sha.empty()) return {};
    std::string ext = extFromMediaUrl(media_url);
    return dir + L"\\" + asciiToW(sha) + L"." + asciiToW(ext);
}

std::string mediaUrlWithSessionToken(const std::string& media_url, const std::string& token) {
    std::string url = media_url;
    if (url.empty()) return {};
    std::string effective = token.empty() ? g_session_token : token;
    if (effective.empty()) return url;
    if (url.find("session_token=") != std::string::npos) return url;
    url += (url.find('?') == std::string::npos) ? "?session_token=" : "&session_token=";
    url += effective;
    return url;
}

MediaUploadResult uploadMediaFile(const std::wstring& path) {
    MediaUploadResult out;
    out.local_path = path;
    if (g_session_token.empty()) {
        out.error = "session_token missing";
        return out;
    }
    if (path.empty()) {
        out.error = "path missing";
        return out;
    }
    std::vector<BYTE> bytes;
    if (!readFileBytes(path, &bytes)) {
        out.error = "read file failed";
        return out;
    }
    std::string mime = mimeFromExtension(path);
    if (mime.empty()) {
        out.error = "unsupported media extension";
        return out;
    }
    auto r = net::uploadMultipart(L"/api/media/upload",
                                  g_session_token, L"file",
                                  filenameFromPath(path), mime, bytes);
    out.status = r.status;
    if (!r.ok()) {
        out.error = r.body.empty() ? "upload failed" : r.body;
        return out;
    }
    out.media_id = net::jsonInt(r.body, "media_id");
    out.sha256 = net::jsonStr(r.body, "sha256");
    out.mime = net::jsonStr(r.body, "mime");
    out.url = normalizeMediaUrl(net::jsonStr(r.body, "url"));
    out.ok = out.media_id > 0 && !out.sha256.empty() && !out.url.empty();
    if (!out.ok) out.error = "bad upload response";
    return out;
}

MediaDownloadResult downloadMediaToPath(const std::string& media_url,
                                        const std::wstring& local_path) {
    MediaDownloadResult out;
    out.url = normalizeMediaUrl(media_url);
    out.sha256 = shaFromMediaUrl(out.url);
    out.path = local_path;
    if (out.url.empty()) {
        out.error = "media_url missing";
        return out;
    }
    if (out.path.empty()) {
        out.error = "local_path missing";
        return out;
    }
    if (GetFileAttributesW(out.path.c_str()) != INVALID_FILE_ATTRIBUTES) {
        out.ok = true;
        return out;
    }
    std::string authed = mediaUrlWithSessionToken(out.url);
    std::wstring wurl = asciiToW(authed);
    auto r = net::request(L"GET", wurl.c_str(), {}, L"");
    out.status = r.status;
    if (!r.ok() || r.body.empty()) {
        out.error = r.body.empty() ? "download failed" : r.body;
        return out;
    }
    if (!writeFileBytes(out.path, r.body)) {
        DeleteFileW(out.path.c_str());
        out.error = "write file failed";
        return out;
    }
    out.ok = true;
    return out;
}

MediaDownloadResult downloadMediaToCache(const std::string& media_url, const wchar_t* scope) {
    std::wstring path = mediaCachePathForUrl(media_url, scope);
    return downloadMediaToPath(media_url, path);
}

std::string mediaPayloadJson(const MediaUploadResult& media) {
    if (!media.ok) return "{}";
    char id_buf[64]{};
    sprintf_s(id_buf, "%lld", (long long)media.media_id);
    return std::string("{\"media_id\":") + id_buf
        + ",\"sha256\":\"" + media.sha256
        + "\",\"mime\":\"" + media.mime
        + "\",\"url\":\"" + media.url
        + "\",\"media_url\":\"" + media.url + "\"}";
}

void uploadAvatar(HWND notify, const std::wstring& path) {
    if (g_session_token.empty() || path.empty()) return;
    auto* a = new AvatarArg{ path, notify };
    CreateThread(nullptr, 0, [](LPVOID lp) -> DWORD {
        std::unique_ptr<AvatarArg> a((AvatarArg*)lp);
        HANDLE f = CreateFileW(a->path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                               nullptr, OPEN_EXISTING, 0, nullptr);
        if (f == INVALID_HANDLE_VALUE) {
            PostMessageW(a->h, WM_APP + 3, 0, 0);
            return 0;
        }
        DWORD sz = GetFileSize(f, nullptr);
        std::vector<BYTE> bytes(sz);
        DWORD rd = 0;
        BOOL read_ok = ReadFile(f, bytes.data(), sz, &rd, nullptr);
        CloseHandle(f);
        if (!read_ok || rd != sz) {
            PostMessageW(a->h, WM_APP + 3, 0, 0);
            return 0;
        }

        std::string mime;
        auto dot = a->path.find_last_of(L'.');
        if (dot != std::wstring::npos) {
            std::wstring ext = a->path.substr(dot);
            for (auto& c : ext) c = (wchar_t)towlower(c);
            if (ext == L".png") mime = "image/png";
            else if (ext == L".jpg" || ext == L".jpeg") mime = "image/jpeg";
            else if (ext == L".gif") mime = "image/gif";
        }
        if (mime.empty()) {
            PostMessageW(a->h, WM_APP + 3, 0, 0);
            return 0;
        }
        auto r = net::uploadMultipart(L"/api/profile/avatar",
                                       g_session_token, L"file",
                                       L"avatar.bin", mime, bytes);
        auto* saved = r.ok() ? new std::wstring(a->path) : nullptr;
        PostMessageW(a->h, WM_APP + 3, r.ok() ? 1 : 0, (LPARAM)saved);
        return 0;
    }, a, 0, nullptr);
}

namespace {
std::wstring userLabelFromObject(const std::string& obj) {
    std::wstring nickname = utf8ToW(net::jsonStr(obj, "nickname"));
    if (!nickname.empty()) return nickname;
    std::wstring username = utf8ToW(net::jsonStr(obj, "username"));
    if (!username.empty()) return username;
    std::wstring uid = utf8ToW(net::jsonStr(obj, "uid"));
    if (!uid.empty()) return uid;
    return utf8ToW(net::jsonStr(obj, "user_id"));
}

ModerationMemberState parseModerationState(const std::string& body, const std::wstring& target_id) {
    ModerationMemberState st;
    st.target_user_id = target_id;
    st.active = net::jsonRaw(body, "active") == "true";
    st.mute_id = net::jsonInt(body, "mute_id");
    st.muted_until = net::jsonInt(body, "muted_until");
    st.reason = utf8ToW(net::jsonStr(body, "reason"));
    st.can_mute = net::jsonRaw(body, "can_mute") == "true";
    st.can_unmute = net::jsonRaw(body, "can_unmute") == "true";
    st.is_super_admin = net::jsonRaw(body, "is_super_admin") == "true";
    std::string target = net::jsonObject(body, "target");
    st.target_label = target.empty() ? target_id : userLabelFromObject(target);
    std::string muted_by = net::jsonObject(body, "muted_by");
    st.muted_by_label = muted_by.empty() ? L"" : userLabelFromObject(muted_by);
    st.ok = true;
    st.loaded = true;
    return st;
}
}

void moderationMember(HWND notify, const std::string& chat_id, const std::wstring& target_user_id) {
    if (g_session_token.empty() || chat_id.empty() || target_user_id.empty()) return;
    struct A { HWND h; std::string chat_id; std::wstring target; };
    auto* a = new A{ notify, chat_id, target_user_id };
    CreateThread(nullptr, 0, [](LPVOID lp) -> DWORD {
        std::unique_ptr<A> a((A*)lp);
        std::string url = "/api/chat/moderation/member?session_token=" + g_session_token
            + "&chat_id=" + a->chat_id
            + "&target_user_id=" + wToUtf8(a->target);
        std::wstring wurl(url.begin(), url.end());
        auto r = net::request(L"GET", wurl.c_str(), {}, L"");
        ModerationMemberState st;
        if (r.ok()) {
            st = parseModerationState(r.body, a->target);
        } else {
            st.target_user_id = a->target;
            st.loaded = true;
            st.ok = false;
            st.error = utf8ToW(r.body.empty() ? "moderation query failed" : r.body.substr(0, 120));
        }
        {
            std::lock_guard<std::mutex> lk(g_moderation_mtx);
            g_moderation_member = std::move(st);
        }
        PostMessageW(a->h, WM_APP + 57, r.ok() ? 1 : 0, 0);
        return 0;
    }, a, 0, nullptr);
}

void muteUser(HWND notify, const std::string& chat_id, const std::wstring& target_user_id,
              int64_t duration_seconds, const std::wstring& reason) {
    if (g_session_token.empty() || chat_id.empty() || target_user_id.empty()) return;
    struct A { HWND h; std::string chat_id; std::wstring target; int64_t duration; std::wstring reason; };
    auto* a = new A{ notify, chat_id, target_user_id, duration_seconds, reason };
    CreateThread(nullptr, 0, [](LPVOID lp) -> DWORD {
        std::unique_ptr<A> a((A*)lp);
        std::string body = "{\"session_token\":\"" + g_session_token
            + "\",\"chat_id\":\"" + a->chat_id
            + "\",\"target_user_id\":\"" + net::jsonEscape(a->target)
            + "\",\"duration_seconds\":" + std::to_string(a->duration)
            + ",\"reason\":\"" + net::jsonEscape(a->reason) + "\"}";
        auto r = net::postJson(L"/api/chat/moderation/mute", body);
        ModerationMemberState st;
        if (r.ok()) {
            st = parseModerationState(r.body, a->target);
        } else {
            st.target_user_id = a->target;
            st.loaded = true;
            st.ok = false;
            st.error = utf8ToW(r.body.empty() ? "mute failed" : r.body.substr(0, 120));
        }
        {
            std::lock_guard<std::mutex> lk(g_moderation_mtx);
            g_moderation_member = std::move(st);
        }
        PostMessageW(a->h, WM_APP + 58, r.ok() ? 1 : 0, 0);
        return 0;
    }, a, 0, nullptr);
}

void unmuteUser(HWND notify, const std::string& chat_id, const std::wstring& target_user_id,
                const std::wstring& reason) {
    if (g_session_token.empty() || chat_id.empty() || target_user_id.empty()) return;
    struct A { HWND h; std::string chat_id; std::wstring target; std::wstring reason; };
    auto* a = new A{ notify, chat_id, target_user_id, reason };
    CreateThread(nullptr, 0, [](LPVOID lp) -> DWORD {
        std::unique_ptr<A> a((A*)lp);
        std::string body = "{\"session_token\":\"" + g_session_token
            + "\",\"chat_id\":\"" + a->chat_id
            + "\",\"target_user_id\":\"" + net::jsonEscape(a->target)
            + "\",\"reason\":\"" + net::jsonEscape(a->reason) + "\"}";
        auto r = net::postJson(L"/api/chat/moderation/unmute", body);
        ModerationMemberState st;
        if (r.ok()) {
            st = parseModerationState(r.body, a->target);
        } else {
            st.target_user_id = a->target;
            st.loaded = true;
            st.ok = false;
            st.error = utf8ToW(r.body.empty() ? "unmute failed" : r.body.substr(0, 120));
        }
        {
            std::lock_guard<std::mutex> lk(g_moderation_mtx);
            g_moderation_member = std::move(st);
        }
        PostMessageW(a->h, WM_APP + 59, r.ok() ? 1 : 0, 0);
        return 0;
    }, a, 0, nullptr);
}

}  // namespace launcher::d2d::fetch
