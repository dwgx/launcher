// 异步 fetch helpers 实现 — 详见 fetch.h。

#include "fetch.h"
#include "net.h"
#include "user_state.h"

#include <memory>
#include <mutex>
#include <ShlObj.h>

#pragma comment(lib, "shell32.lib")

namespace launcher::d2d::fetch {

std::vector<Listing> g_market_listings;
std::mutex g_market_mtx;
PeerProfile g_peer;
std::mutex g_peer_mtx;

namespace {
struct StrArg { std::wstring s; HWND h; };
struct VoidArg { HWND h; };
struct LogoutArg { std::string tok; };
struct AvatarArg { std::wstring path; HWND h; };

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
        std::wstring wurl(url.begin(), url.end());
        auto r = net::request(L"GET", wurl.c_str(), {}, L"");
        if (!r.ok() || r.body.empty()) return 0;
        const char* ext = "png";
        if (r.body.size() >= 3 && (BYTE)r.body[0] == 0xFF && (BYTE)r.body[1] == 0xD8) ext = "jpg";
        else if (r.body.size() >= 4 && r.body[0] == 'G' && r.body[1] == 'I' && r.body[2] == 'F') ext = "gif";

        wchar_t base[MAX_PATH] = {0};
        if (!SHGetSpecialFolderPathW(nullptr, base, CSIDL_LOCAL_APPDATA, FALSE)) return 0;
        std::wstring dir = std::wstring(base) + L"\\Launcher";
        CreateDirectoryW(dir.c_str(), nullptr);
        std::wstring path = dir + L"\\avatar." + std::wstring(ext, ext + strlen(ext));
        for (const wchar_t* e : { L"png", L"jpg", L"jpeg", L"gif", L"webp", L"bmp" }) {
            std::wstring p = dir + L"\\avatar." + e;
            DeleteFileW(p.c_str());
        }
        HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0,
                               nullptr, CREATE_ALWAYS, 0, nullptr);
        if (f == INVALID_HANDLE_VALUE) return 0;
        DWORD wn = 0;
        WriteFile(f, r.body.data(), (DWORD)r.body.size(), &wn, nullptr);
        CloseHandle(f);
        auto* p_arg = new std::wstring(std::move(path));
        PostMessageW(a->h, WM_APP + 23, 0, (LPARAM)p_arg);
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
    CreateThread(nullptr, 0, [](LPVOID lp) -> DWORD {
        std::unique_ptr<VoidArg> a((VoidArg*)lp);
        // 公共端点，不需 session
        auto r = net::request(L"GET", L"/api/market/listings", {}, L"");
        if (!r.ok()) return 0;
        std::vector<Listing> tmp;
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
        {
            std::lock_guard<std::mutex> lk(g_market_mtx);
            g_market_listings = std::move(tmp);
        }
        PostMessageW(a->h, WM_APP + 33, 0, 0);
        return 0;
    }, a, 0, nullptr);
}

void profileUpdate(HWND notify, const std::string& fields) {
    if (g_session_token.empty()) return;
    struct A { std::string f; HWND h; };
    auto* a = new A{ fields, notify };
    CreateThread(nullptr, 0, [](LPVOID lp) -> DWORD {
        std::unique_ptr<A> a((A*)lp);
        std::string body = "{\"session_token\":\"" + g_session_token + "\"," + a->f + "}";
        auto r = net::postJson(L"/api/profile/update", body);
        PostMessageW(a->h, WM_APP + 35, r.ok() ? 1 : 0, 0);
        return 0;
    }, a, 0, nullptr);
}

void peerProfile(HWND notify, const std::wstring& uid_or_nickname) {
    {
        std::lock_guard<std::mutex> lk(g_peer_mtx);
        g_peer = PeerProfile{};
        g_peer.uid = uid_or_nickname;
    }
    struct A { std::wstring k; HWND h; };
    auto* a = new A{ uid_or_nickname, notify };
    CreateThread(nullptr, 0, [](LPVOID lp) -> DWORD {
        std::unique_ptr<A> a((A*)lp);
        std::string key = wToUtf8(a->k);
        // backend route: /api/profile/peer/:key (key = uid 7-digit / username / user_id uuid)
        std::string url = "/api/profile/peer/" + key;
        if (!g_session_token.empty()) url += "?session_token=" + g_session_token;
        std::wstring wurl(url.begin(), url.end());
        auto r = net::request(L"GET", wurl.c_str(), {}, L"");
        {
            std::lock_guard<std::mutex> lk(g_peer_mtx);
            if (r.ok()) {
                g_peer.uid         = utf8ToW(net::jsonStr(r.body, "uid"));
                g_peer.username    = utf8ToW(net::jsonStr(r.body, "username"));
                g_peer.nickname    = utf8ToW(net::jsonStr(r.body, "nickname"));
                g_peer.status      = utf8ToW(net::jsonStr(r.body, "status"));
                g_peer.status_text = utf8ToW(net::jsonStr(r.body, "status_text"));
                g_peer.bio         = utf8ToW(net::jsonStr(r.body, "bio"));
                g_peer.tags.clear();
                // 简单解析 "tags":["a","b",...]
                auto p1 = r.body.find("\"tags\":[");
                if (p1 != std::string::npos) {
                    size_t pos = p1 + 8;
                    while (true) {
                        auto q1 = r.body.find('"', pos);
                        if (q1 == std::string::npos) break;
                        auto q2 = r.body.find('"', q1 + 1);
                        if (q2 == std::string::npos) break;
                        g_peer.tags.push_back(utf8ToW(
                            r.body.substr(q1 + 1, q2 - q1 - 1)));
                        pos = q2 + 1;
                        if (pos < r.body.size() && r.body[pos] == ']') break;
                    }
                }
                g_peer.loaded = true;
            } else {
                g_peer.err = utf8ToW(r.body.empty() ? "无法连接" : r.body.substr(0, 80));
                g_peer.loaded = true;
            }
        }
        PostMessageW(a->h, WM_APP + 36, r.ok() ? 1 : 0, 0);
        return 0;
    }, a, 0, nullptr);
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
        ReadFile(f, bytes.data(), sz, &rd, nullptr);
        CloseHandle(f);

        std::string mime = "image/png";
        auto dot = a->path.find_last_of(L'.');
        if (dot != std::wstring::npos) {
            std::wstring ext = a->path.substr(dot);
            if (ext == L".jpg" || ext == L".jpeg") mime = "image/jpeg";
            else if (ext == L".webp") mime = "image/webp";
            else if (ext == L".gif") mime = "image/gif";
        }
        auto r = net::uploadMultipart(L"/api/profile/avatar",
                                       g_session_token, L"file",
                                       L"avatar.bin", mime, bytes);
        PostMessageW(a->h, WM_APP + 3, r.ok() ? 1 : 0, 0);
        return 0;
    }, a, 0, nullptr);
}

}  // namespace launcher::d2d::fetch
