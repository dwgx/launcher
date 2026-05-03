// Sticker pack 实现 — 1:1 复刻 GDI+ Preview。

#include "sticker.h"
#include "net.h"
#include "user_state.h"

#include <ShlObj.h>
#include <memory>
#include <mutex>

#pragma comment(lib, "shell32.lib")

namespace launcher::d2d::sticker {

std::vector<Pack> g_packs = {
    { "", L"系统 emoji", {}, {}, true, false, "", 0 },
    { "", L"我的表情",   {}, {}, false, false, "", 0 },
};

namespace {
std::mutex g_mtx;

std::wstring utf8ToW(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(n - 1, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}
}

std::wstring cacheDir() {
    wchar_t base[MAX_PATH] = {0};
    if (!SHGetSpecialFolderPathW(nullptr, base, CSIDL_LOCAL_APPDATA, FALSE)) return L"";
    std::wstring dir = std::wstring(base) + L"\\Launcher\\stickers\\";
    CreateDirectoryW((std::wstring(base) + L"\\Launcher").c_str(), nullptr);
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

void fetchMyPacks(HWND notify) {
    if (g_session_token.empty()) return;
    struct A { HWND h; };
    auto* a = new A{ notify };
    CreateThread(nullptr, 0, [](LPVOID lp) -> DWORD {
        std::unique_ptr<A> a((A*)lp);
        std::string url = "/api/sticker/packs/mine?session_token=" + g_session_token;
        std::wstring wurl(url.begin(), url.end());
        auto r = net::request(L"GET", wurl.c_str(), {}, L"");
        if (!r.ok()) return 0;
        // body 形如 [{"id":"...","name":"...","short_name":"...","is_public":..,"install_count":..}, ...]
        std::vector<Pack> tmp;
        size_t pos = 0;
        while (true) {
            auto ob = r.body.find('{', pos);
            if (ob == std::string::npos) break;
            auto cb = r.body.find('}', ob);
            if (cb == std::string::npos) break;
            std::string obj = r.body.substr(ob, cb - ob + 1);
            Pack p;
            p.id = net::jsonStr(obj, "id");
            p.name = utf8ToW(net::jsonStr(obj, "name"));
            p.short_name = net::jsonStr(obj, "short_name");
            p.install_count = (int)net::jsonInt(obj, "install_count");
            // is_public 是 bool — 简单查 "is_public":true
            auto pb = obj.find("\"is_public\":");
            p.is_public = (pb != std::string::npos
                           && obj.find("true", pb) != std::string::npos);
            tmp.push_back(std::move(p));
            pos = cb + 1;
        }
        {
            std::lock_guard<std::mutex> lk(g_mtx);
            // 保留系统 + 我的，重建其他
            std::vector<Pack> kept;
            for (auto& p : g_packs) {
                if (p.is_system || p.name == L"我的表情") kept.push_back(p);
            }
            for (auto& p : tmp) kept.push_back(std::move(p));
            g_packs = std::move(kept);
        }
        PostMessageW(a->h, WM_APP + 22, 0, 0);
        // 拉每个 pack 的内容
        std::lock_guard<std::mutex> lk(g_mtx);
        for (auto& p : g_packs) {
            if (!p.id.empty()) fetchPackContents(a->h, p.id);
        }
        return 0;
    }, a, 0, nullptr);
}

void fetchMyStickers(HWND notify) {
    if (g_session_token.empty()) return;
    struct A { HWND h; };
    auto* a = new A{ notify };
    CreateThread(nullptr, 0, [](LPVOID lp) -> DWORD {
        std::unique_ptr<A> a((A*)lp);
        std::string url = "/api/sticker/mine?session_token=" + g_session_token;
        std::wstring wurl(url.begin(), url.end());
        auto r = net::request(L"GET", wurl.c_str(), {}, L"");
        if (!r.ok()) return 0;
        std::wstring dir = cacheDir();
        if (dir.empty()) return 0;
        // 找每个 media_url 下载到本地，并加进"我的表情"分组
        std::vector<std::wstring> downloaded;
        size_t pos = 0;
        while (true) {
            pos = r.body.find("\"media_url\":\"", pos);
            if (pos == std::string::npos) break;
            pos += 13;
            size_t e = r.body.find('"', pos);
            if (e == std::string::npos) break;
            std::string url_path = r.body.substr(pos, e - pos);
            pos = e;
            auto p1 = url_path.find("/api/media/");
            if (p1 == std::string::npos) continue;
            p1 += 11;
            auto p2 = url_path.find('/', p1);
            if (p2 == std::string::npos) continue;
            std::string sha = url_path.substr(p1, p2 - p1);
            auto dot = url_path.find_last_of('.');
            std::string ext = (dot != std::string::npos) ? url_path.substr(dot + 1) : "bin";
            std::wstring fname = std::wstring(sha.begin(), sha.end())
                + L"." + std::wstring(ext.begin(), ext.end());
            std::wstring local = dir + fname;
            if (GetFileAttributesW(local.c_str()) == INVALID_FILE_ATTRIBUTES) {
                std::wstring wpath(url_path.begin(), url_path.end());
                auto dr = net::request(L"GET", wpath.c_str(), {}, L"");
                if (!dr.ok()) continue;
                HANDLE f = CreateFileW(local.c_str(), GENERIC_WRITE, 0,
                                       nullptr, CREATE_ALWAYS, 0, nullptr);
                if (f == INVALID_HANDLE_VALUE) continue;
                DWORD wn = 0;
                WriteFile(f, dr.body.data(), (DWORD)dr.body.size(), &wn, nullptr);
                CloseHandle(f);
            }
            downloaded.push_back(local);
        }
        {
            std::lock_guard<std::mutex> lk(g_mtx);
            for (auto& p : g_packs) {
                if (p.name == L"我的表情") {
                    for (auto& s : downloaded) {
                        bool dup = false;
                        for (auto& e : p.stickers) if (e == s) { dup = true; break; }
                        if (!dup) p.stickers.push_back(s);
                    }
                    break;
                }
            }
        }
        PostMessageW(a->h, WM_APP + 24, (WPARAM)(int)downloaded.size(), 0);
        return 0;
    }, a, 0, nullptr);
}

void fetchPackContents(HWND notify, const std::string& pack_id) {
    if (g_session_token.empty() || pack_id.empty()) return;
    struct A { std::string id; HWND h; };
    auto* a = new A{ pack_id, notify };
    CreateThread(nullptr, 0, [](LPVOID lp) -> DWORD {
        std::unique_ptr<A> a((A*)lp);
        std::string url = "/api/sticker/pack/" + a->id;
        std::wstring wurl(url.begin(), url.end());
        auto r = net::request(L"GET", wurl.c_str(), {}, L"");
        if (!r.ok()) return 0;
        std::wstring dir = cacheDir();
        if (dir.empty()) return 0;
        std::vector<std::wstring> downloaded;
        size_t pos = 0;
        while (true) {
            pos = r.body.find("\"media_url\":\"", pos);
            if (pos == std::string::npos) break;
            pos += 13;
            size_t e = r.body.find('"', pos);
            if (e == std::string::npos) break;
            std::string url_path = r.body.substr(pos, e - pos);
            pos = e;
            auto p1 = url_path.find("/api/media/");
            if (p1 == std::string::npos) continue;
            p1 += 11;
            auto p2 = url_path.find('/', p1);
            if (p2 == std::string::npos) continue;
            std::string sha = url_path.substr(p1, p2 - p1);
            auto dot = url_path.find_last_of('.');
            std::string ext = (dot != std::string::npos) ? url_path.substr(dot + 1) : "bin";
            std::wstring fname = std::wstring(sha.begin(), sha.end())
                + L"." + std::wstring(ext.begin(), ext.end());
            std::wstring local = dir + fname;
            if (GetFileAttributesW(local.c_str()) == INVALID_FILE_ATTRIBUTES) {
                std::wstring wpath(url_path.begin(), url_path.end());
                auto dr = net::request(L"GET", wpath.c_str(), {}, L"");
                if (!dr.ok()) continue;
                HANDLE f = CreateFileW(local.c_str(), GENERIC_WRITE, 0,
                                       nullptr, CREATE_ALWAYS, 0, nullptr);
                if (f == INVALID_HANDLE_VALUE) continue;
                DWORD wn = 0;
                WriteFile(f, dr.body.data(), (DWORD)dr.body.size(), &wn, nullptr);
                CloseHandle(f);
            }
            downloaded.push_back(local);
        }
        {
            std::lock_guard<std::mutex> lk(g_mtx);
            for (auto& p : g_packs) {
                if (p.id == a->id) {
                    for (auto& s : downloaded) {
                        bool dup = false;
                        for (auto& e : p.stickers) if (e == s) { dup = true; break; }
                        if (!dup) p.stickers.push_back(s);
                    }
                    break;
                }
            }
        }
        PostMessageW(a->h, WM_APP + 25, (WPARAM)(int)downloaded.size(), 0);
        return 0;
    }, a, 0, nullptr);
}

void sharePack(HWND notify, const std::string& pack_id, bool is_public) {
    struct A { std::string id; bool pub; HWND h; };
    auto* a = new A{ pack_id, is_public, notify };
    CreateThread(nullptr, 0, [](LPVOID lp) -> DWORD {
        std::unique_ptr<A> a((A*)lp);
        std::string body = "{\"session_token\":\"" + g_session_token
                         + "\",\"pack_id\":\"" + a->id
                         + "\",\"is_public\":" + (a->pub ? "true" : "false") + "}";
        auto r = net::postJson(L"/api/sticker/pack/share", body);
        PostMessageW(a->h, WM_APP + 26, r.ok() ? 1 : 0, 0);
        return 0;
    }, a, 0, nullptr);
}

void deletePack(HWND notify, const std::string& pack_id) {
    struct A { std::string id; HWND h; };
    auto* a = new A{ pack_id, notify };
    CreateThread(nullptr, 0, [](LPVOID lp) -> DWORD {
        std::unique_ptr<A> a((A*)lp);
        std::string body = "{\"session_token\":\"" + g_session_token
                         + "\",\"pack_id\":\"" + a->id + "\"}";
        auto r = net::postJson(L"/api/sticker/pack/delete", body);
        if (r.ok()) {
            std::lock_guard<std::mutex> lk(g_mtx);
            for (auto it = g_packs.begin(); it != g_packs.end(); ++it) {
                if (it->id == a->id) { g_packs.erase(it); break; }
            }
        }
        PostMessageW(a->h, WM_APP + 27, r.ok() ? 1 : 0, 0);
        return 0;
    }, a, 0, nullptr);
}

void installPack(HWND notify, const std::string& short_name) {
    struct A { std::string sn; HWND h; };
    auto* a = new A{ short_name, notify };
    CreateThread(nullptr, 0, [](LPVOID lp) -> DWORD {
        std::unique_ptr<A> a((A*)lp);
        std::string body = "{\"session_token\":\"" + g_session_token
                         + "\",\"short_name\":\"" + a->sn + "\"}";
        auto r = net::postJson(L"/api/sticker/pack/install", body);
        PostMessageW(a->h, WM_APP + 28, r.ok() ? 1 : 0, 0);
        return 0;
    }, a, 0, nullptr);
}

}  // namespace launcher::d2d::sticker
