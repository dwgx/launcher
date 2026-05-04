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
    { "", L"系统 emoji", {}, {}, true,  false, false, L"", "", 0 },
    { "", L"我的表情",   {}, {}, false, false, true,  L"", "", 0 },
};
std::mutex g_packs_mtx;

PackPreview g_pack_preview;
std::mutex  g_pack_preview_mtx;

namespace {
auto& g_mtx = g_packs_mtx;

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
            p.creator_name = utf8ToW(net::jsonStr(obj, "creator_name"));
            p.install_count = (int)net::jsonInt(obj, "install_count");
            // is_public / is_owner 是 bool — 找 "key":true / false
            auto find_bool = [&](const char* key) -> bool {
                std::string k = "\""; k += key; k += "\":";
                auto pb = obj.find(k);
                if (pb == std::string::npos) return false;
                pb += k.size();
                return obj.compare(pb, 4, "true") == 0;
            };
            p.is_public = find_bool("is_public");
            p.is_owner  = find_bool("is_owner");
            tmp.push_back(std::move(p));
            pos = cb + 1;
        }
        {
            std::lock_guard<std::mutex> lk(g_mtx);
            std::unordered_map<std::string, Pack> by_id;
            for (auto& p : g_packs) if (!p.id.empty()) by_id[p.id] = p;

            std::vector<Pack> merged;
            // 系统 emoji 永远第一
            for (auto& p : g_packs) if (p.is_system) merged.push_back(p);

            // 加 backend 返回的 packs（含真"我的表情" 如果有）
            bool found_mine = false;
            for (auto& np : tmp) {
                auto it = by_id.find(np.id);
                if (it != by_id.end()) {
                    np.stickers = std::move(it->second.stickers);
                    np.sticker_ids = std::move(it->second.sticker_ids);
                }
                if (np.name == L"我的表情") found_mine = true;
                merged.push_back(std::move(np));
            }

            // backend 没"我的表情" → 保留本地 placeholder（ensureMyStickersPack
            // 会异步创建，下次 fetchMyPacks 就有了）。避免重复"我的表情"。
            if (!found_mine) {
                for (auto& p : g_packs) {
                    if (!p.is_system && p.name == L"我的表情" && p.id.empty()) {
                        merged.insert(merged.begin() + 1, p);
                        break;
                    }
                }
            }

            g_packs = std::move(merged);
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
        // 拿 short_name，写回 g_packs；总是把链接复制到剪贴板（不再开关）
        static std::string g_pending_sn;
        g_pending_sn.clear();
        if (r.ok()) {
            std::string sn = net::jsonStr(r.body, "short_name");
            if (!sn.empty()) {
                std::lock_guard<std::mutex> lk(g_mtx);
                for (auto& p : g_packs) if (p.id == a->id) {
                    p.short_name = sn;
                    if (a->pub) p.is_public = true;
                    break;
                }
                g_pending_sn = sn;     // 主线程复制 launcher://pack/sn 到剪贴板
            }
        }
        PostMessageW(a->h, WM_APP + 26, r.ok() ? 1 : 0,
                     g_pending_sn.empty() ? 0 : (LPARAM)&g_pending_sn);
        return 0;
    }, a, 0, nullptr);
}

void reorderPacks(HWND notify, const std::vector<std::string>& pack_ids) {
    if (g_session_token.empty() || pack_ids.empty()) return;
    struct A { std::vector<std::string> ids; HWND h; };
    auto* a = new A{ pack_ids, notify };
    CreateThread(nullptr, 0, [](LPVOID lp) -> DWORD {
        std::unique_ptr<A> a((A*)lp);
        std::string body = "{\"session_token\":\"" + g_session_token
                         + "\",\"pack_ids\":[";
        bool first = true;
        for (auto& id : a->ids) {
            if (!first) body += ",";
            body += "\"" + id + "\"";
            first = false;
        }
        body += "]}";
        auto r = net::postJson(L"/api/sticker/pack/reorder", body);
        PostMessageW(a->h, WM_APP + 48, r.ok() ? 1 : 0, 0);
        return 0;
    }, a, 0, nullptr);
}

void exportPackToFolder(HWND notify, const std::string& pack_id) {
    // 先弹文件夹选择对话框
    BROWSEINFOW bi{};
    bi.hwndOwner = notify;
    bi.lpszTitle = L"选择导出目标文件夹";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return;
    wchar_t target[MAX_PATH] = {0};
    bool ok_path = SHGetPathFromIDListW(pidl, target);
    CoTaskMemFree(pidl);
    if (!ok_path) return;

    struct A { std::string id; std::wstring dst; HWND h; };
    auto* a = new A{ pack_id, target, notify };
    CreateThread(nullptr, 0, [](LPVOID lp) -> DWORD {
        std::unique_ptr<A> a((A*)lp);
        // 找到对应 pack 的本地 stickers 路径副本
        std::vector<std::wstring> srcs;
        std::wstring pack_name;
        {
            std::lock_guard<std::mutex> lk(g_mtx);
            for (auto& p : g_packs) {
                if (p.id == a->id) {
                    srcs = p.stickers;
                    pack_name = p.name;
                    break;
                }
            }
        }
        // 创建子目录 = pack 名（去掉文件夹非法字符）
        std::wstring sub = pack_name;
        for (auto& c : sub) {
            if (c == L'\\' || c == L'/' || c == L':' || c == L'*' || c == L'?'
                || c == L'"' || c == L'<' || c == L'>' || c == L'|') c = L'_';
        }
        std::wstring out_dir = a->dst + L"\\" + sub;
        CreateDirectoryW(out_dir.c_str(), nullptr);
        int success = 0;
        for (auto& src : srcs) {
            // 取 basename
            auto sl = src.find_last_of(L"\\/");
            std::wstring fname = (sl != std::wstring::npos) ? src.substr(sl + 1) : src;
            std::wstring dst = out_dir + L"\\" + fname;
            if (CopyFileW(src.c_str(), dst.c_str(), FALSE)) success++;
        }
        PostMessageW(a->h, WM_APP + 43, (WPARAM)success, 0);
        return 0;
    }, a, 0, nullptr);
}

void previewPackByShort(HWND notify, const std::string& short_name) {
    {
        std::lock_guard<std::mutex> lk(g_pack_preview_mtx);
        g_pack_preview = PackPreview{};
        g_pack_preview.short_name = short_name;
    }
    struct A { std::string sn; HWND h; };
    auto* a = new A{ short_name, notify };
    CreateThread(nullptr, 0, [](LPVOID lp) -> DWORD {
        std::unique_ptr<A> a((A*)lp);
        std::string url = "/api/sticker/pack/by-short/" + a->sn;
        std::wstring wurl(url.begin(), url.end());
        auto r = net::request(L"GET", wurl.c_str(), {}, L"");
        if (!r.ok()) {
            std::lock_guard<std::mutex> lk(g_pack_preview_mtx);
            g_pack_preview.err = r.body.empty() ? "无法连接" : r.body.substr(0, 80);
            g_pack_preview.loaded = true;
            PostMessageW(a->h, WM_APP + 44, 0, 0);
            return 0;
        }

        std::wstring dir = cacheDir();
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
                if (dr.ok()) {
                    HANDLE f = CreateFileW(local.c_str(), GENERIC_WRITE, 0,
                                           nullptr, CREATE_ALWAYS, 0, nullptr);
                    if (f != INVALID_HANDLE_VALUE) {
                        DWORD wn = 0;
                        WriteFile(f, dr.body.data(), (DWORD)dr.body.size(), &wn, nullptr);
                        CloseHandle(f);
                    }
                }
            }
            if (GetFileAttributesW(local.c_str()) != INVALID_FILE_ATTRIBUTES) {
                downloaded.push_back(local);
            }
        }
        std::string id = net::jsonStr(r.body, "id");
        std::wstring name = utf8ToW(net::jsonStr(r.body, "name"));
        std::wstring creator = utf8ToW(net::jsonStr(r.body, "creator_name"));
        long long install_count = net::jsonInt(r.body, "install_count");
        bool already = false;
        {
            std::lock_guard<std::mutex> lk(g_mtx);
            for (auto& p : g_packs) if (p.id == id) { already = true; break; }
        }
        {
            std::lock_guard<std::mutex> lk(g_pack_preview_mtx);
            g_pack_preview.id = id;
            g_pack_preview.name = name;
            g_pack_preview.creator_name = creator;
            g_pack_preview.install_count = (int)install_count;
            g_pack_preview.sticker_paths = downloaded;
            g_pack_preview.cover_path = downloaded.empty() ? L"" : downloaded.front();
            g_pack_preview.already_installed = already;
            g_pack_preview.loaded = true;
        }
        PostMessageW(a->h, WM_APP + 44, 1, 0);
        return 0;
    }, a, 0, nullptr);
}

void installPackByShort(HWND notify, const std::string& short_name) {
    struct A { std::string sn; HWND h; };
    auto* a = new A{ short_name, notify };
    CreateThread(nullptr, 0, [](LPVOID lp) -> DWORD {
        std::unique_ptr<A> a((A*)lp);
        // 通过 by-short 拿到 pack id（已经走过 previewPackByShort 也行；这里再调一次确保 id 最新）
        std::string url = "/api/sticker/pack/by-short/" + a->sn;
        std::wstring wurl(url.begin(), url.end());
        auto r = net::request(L"GET", wurl.c_str(), {}, L"");
        if (!r.ok()) {
            PostMessageW(a->h, WM_APP + 28, 0, 0);
            return 0;
        }
        std::string id = net::jsonStr(r.body, "id");
        if (id.empty()) {
            PostMessageW(a->h, WM_APP + 28, 0, 0);
            return 0;
        }
        std::string body = "{\"session_token\":\"" + g_session_token
                         + "\",\"pack_id\":\"" + id + "\"}";
        auto ir = net::postJson(L"/api/sticker/pack/install", body);
        PostMessageW(a->h, WM_APP + 28, ir.ok() ? 1 : 0, 0);
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

void importFromFolder(HWND notify, const std::wstring& folder_path,
                      const std::string& pack_id) {
    struct A { std::wstring folder; std::string pid; HWND h; };
    auto* a = new A{ folder_path, pack_id, notify };
    CreateThread(nullptr, 0, [](LPVOID lp) -> DWORD {
        std::unique_ptr<A> a((A*)lp);
        // 扫描文件夹
        std::vector<std::wstring> files;
        std::wstring pattern = a->folder + L"\\*";
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                std::wstring name = fd.cFileName;
                auto dot = name.find_last_of(L'.');
                if (dot == std::wstring::npos) continue;
                std::wstring ext = name.substr(dot);
                for (auto& c : ext) c = (wchar_t)towlower(c);
                if (ext == L".png" || ext == L".jpg" || ext == L".jpeg"
                    || ext == L".gif" || ext == L".webp" || ext == L".bmp") {
                    files.push_back(a->folder + L"\\" + name);
                }
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }

        std::wstring cache = cacheDir();
        int success = 0;
        std::vector<std::wstring> downloaded;
        for (const auto& path : files) {
            // 读文件
            HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                   nullptr, OPEN_EXISTING, 0, nullptr);
            if (f == INVALID_HANDLE_VALUE) continue;
            DWORD sz = GetFileSize(f, nullptr);
            if (sz == 0 || sz > 8 * 1024 * 1024) { CloseHandle(f); continue; }
            std::vector<BYTE> bytes(sz);
            DWORD rd = 0;
            ReadFile(f, bytes.data(), sz, &rd, nullptr);
            CloseHandle(f);

            // mime + filename
            std::string mime = "image/png";
            std::wstring ext = path.substr(path.find_last_of(L'.'));
            for (auto& c : ext) c = (wchar_t)towlower(c);
            if (ext == L".jpg" || ext == L".jpeg") mime = "image/jpeg";
            else if (ext == L".gif")  mime = "image/gif";
            else if (ext == L".webp") mime = "image/webp";
            else if (ext == L".bmp")  mime = "image/bmp";

            auto sl = path.find_last_of(L"\\/");
            std::wstring fn = (sl != std::wstring::npos) ? path.substr(sl + 1) : path;

            // 1. 上传 media
            auto mr = net::uploadMultipart(L"/api/media/upload",
                                            g_session_token, L"file",
                                            fn, mime, bytes);
            if (!mr.ok()) continue;
            std::string media_id = std::to_string(net::jsonInt(mr.body, "media_id"));
            if (media_id == "0") {
                std::string mid_str = net::jsonStr(mr.body, "media_id");
                if (!mid_str.empty()) media_id = mid_str;
            }
            std::string sha = net::jsonStr(mr.body, "sha256");

            // 2. 创建 sticker
            std::string body = "{\"session_token\":\"" + g_session_token
                             + "\",\"media_id\":" + media_id + "}";
            auto sr = net::postJson(L"/api/sticker", body);
            if (!sr.ok()) continue;
            // 3. 加到 pack — 后端 create_sticker 不接 pack_id；要再调一次 add_to_pack
            std::string sticker_id = net::jsonStr(sr.body, "id");
            if (!sticker_id.empty() && !a->pid.empty()) {
                std::string add_body = "{\"session_token\":\"" + g_session_token
                                     + "\",\"pack_id\":\"" + a->pid
                                     + "\",\"sticker_id\":\"" + sticker_id + "\"}";
                net::postJson(L"/api/sticker/pack/add", add_body);
            }

            // 3. 写本地缓存 (sha + ext)
            if (!sha.empty() && !cache.empty()) {
                std::wstring local = cache
                    + std::wstring(sha.begin(), sha.end())
                    + ext;
                if (GetFileAttributesW(local.c_str()) == INVALID_FILE_ATTRIBUTES) {
                    HANDLE wf = CreateFileW(local.c_str(), GENERIC_WRITE, 0,
                                             nullptr, CREATE_ALWAYS, 0, nullptr);
                    if (wf != INVALID_HANDLE_VALUE) {
                        DWORD wn = 0;
                        WriteFile(wf, bytes.data(), (DWORD)bytes.size(), &wn, nullptr);
                        CloseHandle(wf);
                    }
                }
                downloaded.push_back(local);
            }
            success++;
        }

        // 加进对应 pack
        {
            std::lock_guard<std::mutex> lk(g_mtx);
            for (auto& p : g_packs) {
                if (p.id == a->pid) {
                    for (auto& s : downloaded) {
                        bool dup = false;
                        for (auto& e : p.stickers) if (e == s) { dup = true; break; }
                        if (!dup) p.stickers.push_back(s);
                    }
                    break;
                }
            }
        }
        // 把 pack_id 透传回主线程，让 picker 切到这个 pack 显示新导入的图
        static std::string g_pending_imp_pid;
        g_pending_imp_pid = a->pid;
        PostMessageW(a->h, WM_APP + 29, (WPARAM)success, (LPARAM)&g_pending_imp_pid);
        return 0;
    }, a, 0, nullptr);
}

void importFromFolderUi(HWND notify, const std::string& pack_id) {
    BROWSEINFOW bi{};
    bi.hwndOwner = notify;
    bi.lpszTitle = L"选择含 .png/.jpg/.gif/.webp 的文件夹（批量导入到表情包）";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return;
    wchar_t path[MAX_PATH] = {0};
    if (SHGetPathFromIDListW(pidl, path)) {
        importFromFolder(notify, path, pack_id);
    }
    CoTaskMemFree(pidl);
}

int totalUserStickers() {
    std::lock_guard<std::mutex> lk(g_mtx);
    int n = 0;
    for (auto& p : g_packs) {
        if (p.is_system) continue;
        n += (int)p.stickers.size();
    }
    return n;
}

void ensureMyStickersPack(HWND notify) {
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        for (auto& p : g_packs) {
            if (!p.is_system && p.name == L"我的表情" && !p.id.empty()) return;
        }
    }
    // 没找到带 id 的"我的表情" → 后端创建一个
    struct A { HWND h; };
    auto* a = new A{ notify };
    CreateThread(nullptr, 0, [](LPVOID lp) -> DWORD {
        std::unique_ptr<A> a((A*)lp);
        if (g_session_token.empty()) return 0;
        std::string body = "{\"session_token\":\"" + g_session_token
                         + "\",\"name\":\"" + net::jsonEscape(L"我的表情") + "\"}";
        auto r = net::postJson(L"/api/sticker/pack", body);
        if (r.ok()) {
            std::string id = net::jsonStr(r.body, "id");
            if (!id.empty()) {
                std::lock_guard<std::mutex> lk(g_mtx);
                for (auto& p : g_packs) {
                    if (!p.is_system && p.name == L"我的表情") {
                        p.id = id;
                        break;
                    }
                }
            }
        }
        PostMessageW(a->h, WM_APP + 42, 0, 0);
        return 0;
    }, a, 0, nullptr);
}

void deleteSticker(HWND notify, const std::wstring& path) {
    // 1. 立即本地删除（乐观更新）
    std::string sha;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        for (auto& p : g_packs) {
            for (auto it = p.stickers.begin(); it != p.stickers.end(); ++it) {
                if (*it == path) {
                    // sha 从路径提取 (格式：cache_dir/<sha>.<ext>)
                    auto sl = path.find_last_of(L"\\/");
                    auto dot = path.find_last_of(L'.');
                    if (sl != std::wstring::npos && dot != std::wstring::npos && dot > sl) {
                        std::wstring s = path.substr(sl + 1, dot - sl - 1);
                        // sha256 是 ascii hex (0-9 a-f)，直接 wchar→char cast 安全
                        sha.reserve(s.size());
                        for (wchar_t c : s) sha.push_back((char)c);
                    }
                    p.stickers.erase(it);
                    goto local_done;
                }
            }
        }
    local_done:;
    }
    // 2. 后端异步同步（失败 silent）
    if (sha.empty()) return;
    struct A { std::string sha; HWND h; };
    auto* a = new A{ sha, notify };
    CreateThread(nullptr, 0, [](LPVOID lp) -> DWORD {
        std::unique_ptr<A> a((A*)lp);
        std::string body = "{\"session_token\":\"" + g_session_token
                         + "\",\"sha256\":\"" + a->sha + "\"}";
        net::postJson(L"/api/sticker/delete", body);
        PostMessageW(a->h, WM_APP + 39, 1, 0);
        return 0;
    }, a, 0, nullptr);
}

}  // namespace launcher::d2d::sticker
