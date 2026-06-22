// Sticker pack 实现 — 1:1 复刻 GDI+ Preview。

#include "sticker.h"
#include "fetch.h"
#include "net.h"
#include "user_state.h"

#include <ShlObj.h>
#include <memory>
#include <mutex>

#pragma comment(lib, "shell32.lib")

namespace launcher::d2d::sticker {

std::vector<Pack> g_packs = {
    { "", L"系统 emoji", {}, {}, {}, true,  false, false, L"", "", 0 },
    { "", L"我的表情",   {}, {}, {}, false, false, true,  L"", "", 0 },
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

std::string jsonStringFieldFrom(const std::string& body, const char* field, size_t start) {
    return net::jsonStr(body.substr(start), field);
}

std::string shaFromMediaUrl(const std::string& media_url) {
    const std::string marker = "/api/media/";
    auto p = media_url.find(marker);
    if (p == std::string::npos) return {};
    p += marker.size();
    auto e = media_url.find('/', p);
    if (e == std::string::npos || e <= p) return {};
    return media_url.substr(p, e - p);
}

void syncLegacyVectors(Pack& p) {
    p.stickers.clear();
    p.sticker_ids.clear();
    p.stickers.reserve(p.items.size());
    p.sticker_ids.reserve(p.items.size());
    for (const auto& item : p.items) {
        p.stickers.push_back(item.path);
        p.sticker_ids.push_back(item.sticker_id);
    }
}

bool hasItem(const Pack& p, const StickerItem& item) {
    for (const auto& e : p.items) {
        if (!item.sticker_id.empty() && e.sticker_id == item.sticker_id) return true;
        if (!item.media_url.empty() && e.media_url == item.media_url) return true;
        if (!item.path.empty() && e.path == item.path) return true;
    }
    return false;
}

void addItemToPack(Pack& p, StickerItem item) {
    if (item.path.empty() || hasItem(p, item)) return;
    p.items.push_back(std::move(item));
    syncLegacyVectors(p);
}

StickerItem stickerItemFromBodyAt(const std::string& body, size_t obj_start) {
    StickerItem item;
    item.sticker_id = jsonStringFieldFrom(body, "id", obj_start);
    item.media_id = net::jsonInt(body.substr(obj_start), "media_id");
    item.media_url = jsonStringFieldFrom(body, "media_url", obj_start);
    item.mime = jsonStringFieldFrom(body, "mime", obj_start);
    item.sha256 = shaFromMediaUrl(item.media_url);
    if (item.media_url.empty()) return item;
    auto dl = fetch::downloadMediaToCache(item.media_url, L"stickers");
    if (dl.ok) {
        item.path = dl.path;
        if (item.sha256.empty()) item.sha256 = dl.sha256;
    }
    return item;
}

std::vector<StickerItem> parseAndDownloadStickerItems(const std::string& body) {
    std::vector<StickerItem> out;
    size_t pos = 0;
    while (true) {
        pos = body.find("\"media_url\":\"", pos);
        if (pos == std::string::npos) break;
        size_t obj = body.rfind('{', pos);
        if (obj == std::string::npos) obj = pos;
        StickerItem item = stickerItemFromBodyAt(body, obj);
        if (!item.path.empty()) out.push_back(std::move(item));
        pos += 13;
    }
    return out;
}
}

std::wstring cacheDir() {
    std::wstring dir = fetch::mediaCacheDir(L"stickers");
    if (!dir.empty() && dir.back() != L'\\') dir += L"\\";
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
                    np.items = std::move(it->second.items);
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
        // 找每个 media_url 下载到本地，并加进"我的表情"分组；item 保留远端 id/url。
        std::vector<StickerItem> downloaded = parseAndDownloadStickerItems(r.body);
        {
            std::lock_guard<std::mutex> lk(g_mtx);
            for (auto& p : g_packs) {
                if (p.name == L"我的表情") {
                    for (auto& item : downloaded) {
                        addItemToPack(p, std::move(item));
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
        std::vector<StickerItem> downloaded = parseAndDownloadStickerItems(r.body);
        {
            std::lock_guard<std::mutex> lk(g_mtx);
            for (auto& p : g_packs) {
                if (p.id == a->id) {
                    for (auto& item : downloaded) {
                        addItemToPack(p, std::move(item));
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
        std::string g_pending_sn;
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
        auto* payload = g_pending_sn.empty() ? nullptr : new std::string(g_pending_sn);
        PostMessageW(a->h, WM_APP + 26, r.ok() ? 1 : 0,
                     (LPARAM)payload);
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
            bool apply = false;
            {
            std::lock_guard<std::mutex> lk(g_pack_preview_mtx);
            if (g_pack_preview.short_name == a->sn) {
            g_pack_preview.err = r.body.empty() ? "无法连接" : r.body.substr(0, 80);
            g_pack_preview.loaded = true;
                apply = true;
            }
            }
            if (apply) PostMessageW(a->h, WM_APP + 44, 0, 0);
            return 0;
        }

        std::vector<StickerItem> items = parseAndDownloadStickerItems(r.body);
        std::vector<std::wstring> downloaded;
        downloaded.reserve(items.size());
        for (const auto& item : items) downloaded.push_back(item.path);
        std::string id = net::jsonStr(r.body, "id");
        std::wstring name = utf8ToW(net::jsonStr(r.body, "name"));
        std::wstring creator = utf8ToW(net::jsonStr(r.body, "creator_name"));
        long long install_count = net::jsonInt(r.body, "install_count");
        bool already = false;
        {
            std::lock_guard<std::mutex> lk(g_mtx);
            for (auto& p : g_packs) if (p.id == id) { already = true; break; }
        }
        bool apply = false;
        {
            std::lock_guard<std::mutex> lk(g_pack_preview_mtx);
            if (g_pack_preview.short_name == a->sn) {
            g_pack_preview.id = id;
            g_pack_preview.name = name;
            g_pack_preview.creator_name = creator;
            g_pack_preview.install_count = (int)install_count;
            g_pack_preview.sticker_paths = downloaded;
            g_pack_preview.items = std::move(items);
            g_pack_preview.cover_path = downloaded.empty() ? L"" : downloaded.front();
            g_pack_preview.already_installed = already;
            g_pack_preview.loaded = true;
                apply = true;
            }
        }
        if (apply) PostMessageW(a->h, WM_APP + 44, 1, 0);
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

        int success = 0;
        std::vector<StickerItem> imported;
        for (const auto& path : files) {
            // 1. 上传 media。结果保留 media_id/sha256/mime/url，后续 chat payload 不再依赖本地路径。
            auto media = fetch::uploadMediaFile(path);
            if (!media.ok) continue;

            // 2. 创建 sticker
            std::string body = "{\"session_token\":\"" + g_session_token
                             + "\",\"media_id\":" + std::to_string(media.media_id) + "}";
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

            // 4. 写/复用本地缓存，并保存远端元数据。
            StickerItem item;
            item.path = path;
            item.sticker_id = sticker_id;
            item.media_id = media.media_id;
            item.media_url = media.url;
            item.sha256 = media.sha256;
            item.mime = media.mime;
            auto dl = fetch::downloadMediaToCache(media.url, L"stickers");
            if (dl.ok) item.path = dl.path;
            imported.push_back(std::move(item));
            success++;
        }

        // 加进对应 pack
        {
            std::lock_guard<std::mutex> lk(g_mtx);
            for (auto& p : g_packs) {
                if (p.id == a->pid) {
                    for (auto& item : imported) {
                        addItemToPack(p, std::move(item));
                    }
                    break;
                }
            }
        }
        // 把 pack_id 透传回主线程，让 picker 切到这个 pack 显示新导入的图
        auto* payload = new std::string(a->pid);
        PostMessageW(a->h, WM_APP + 29, (WPARAM)success, (LPARAM)payload);
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
        n += (int)(p.items.empty() ? p.stickers.size() : p.items.size());
    }
    return n;
}

bool stickerForPath(const std::wstring& path, StickerItem* out) {
    if (!out) return false;
    std::lock_guard<std::mutex> lk(g_mtx);
    for (const auto& p : g_packs) {
        for (const auto& item : p.items) {
            if (item.path == path) {
                *out = item;
                return true;
            }
        }
        for (size_t i = 0; i < p.stickers.size(); ++i) {
            if (p.stickers[i] != path) continue;
            *out = StickerItem{};
            out->path = p.stickers[i];
            if (i < p.sticker_ids.size()) out->sticker_id = p.sticker_ids[i];
            return true;
        }
    }
    return false;
}

std::string stickerPayloadJsonForPath(const std::wstring& path) {
    StickerItem item;
    if (!stickerForPath(path, &item)) return "{}";
    std::string json = "{";
    bool first = true;
    auto add_str = [&](const char* key, const std::string& value) {
        if (value.empty()) return;
        if (!first) json += ",";
        json += "\"";
        json += key;
        json += "\":\"";
        json += value;
        json += "\"";
        first = false;
    };
    auto add_i64 = [&](const char* key, int64_t value) {
        if (value <= 0) return;
        if (!first) json += ",";
        json += "\"";
        json += key;
        json += "\":";
        json += std::to_string(value);
        first = false;
    };
    add_str("sticker_id", item.sticker_id);
    add_i64("media_id", item.media_id);
    add_str("media_url", item.media_url);
    add_str("url", item.media_url);
    add_str("sha256", item.sha256);
    add_str("mime", item.mime);
    json += "}";
    return json;
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
    std::string sticker_id;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        for (auto& p : g_packs) {
            for (auto it = p.items.begin(); it != p.items.end(); ++it) {
                if (it->path == path) {
                    sticker_id = it->sticker_id;
                    sha = it->sha256;
                    p.items.erase(it);
                    syncLegacyVectors(p);
                    goto local_done;
                }
            }
            for (size_t i = 0; i < p.stickers.size(); ++i) {
                if (p.stickers[i] != path) continue;
                if (i < p.sticker_ids.size()) sticker_id = p.sticker_ids[i];
                // sha 从路径提取 (格式：cache_dir/<sha>.<ext>)
                auto sl = path.find_last_of(L"\\/");
                auto dot = path.find_last_of(L'.');
                if (sl != std::wstring::npos && dot != std::wstring::npos && dot > sl) {
                    std::wstring s = path.substr(sl + 1, dot - sl - 1);
                    // sha256 是 ascii hex (0-9 a-f)，直接 wchar→char cast 安全
                    sha.reserve(s.size());
                    for (wchar_t c : s) sha.push_back((char)c);
                }
                p.stickers.erase(p.stickers.begin() + i);
                if (i < p.sticker_ids.size()) p.sticker_ids.erase(p.sticker_ids.begin() + i);
                goto local_done;
            }
        }
    local_done:;
    }
    // 2. 后端异步同步（失败 silent）
    if (sha.empty() && sticker_id.empty()) return;
    struct A { std::string sha; std::string sid; HWND h; };
    auto* a = new A{ sha, sticker_id, notify };
    CreateThread(nullptr, 0, [](LPVOID lp) -> DWORD {
        std::unique_ptr<A> a((A*)lp);
        std::string body = "{\"session_token\":\"" + g_session_token
                         + "\"";
        if (!a->sid.empty()) {
            body += ",\"sticker_id\":\"" + a->sid + "\"";
        } else {
            body += ",\"sha256\":\"" + a->sha + "\"";
        }
        body += "}";
        net::postJson(L"/api/sticker/delete", body);
        PostMessageW(a->h, WM_APP + 39, 1, 0);
        return 0;
    }, a, 0, nullptr);
}

}  // namespace launcher::d2d::sticker
