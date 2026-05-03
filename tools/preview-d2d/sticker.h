// Sticker pack 全套管理 — 1:1 复刻 GDI+ Preview chatv::Pack。
//   * 启动后 fetchMyPacks → GET /api/sticker/packs/mine 拉用户表情包
//   * fetchMyStickers → GET /api/sticker/mine 拉单个 sticker（同账号多设备同步）
//   * fetchPackContents(pack_id) → GET /api/sticker/pack/:id 拉 pack 里的 stickers
//   * sharePack / deletePack / installPack 操作
#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <string>
#include <vector>

namespace launcher::d2d::sticker {

struct Pack {
    std::string  id;
    std::wstring name;
    std::vector<std::wstring> stickers;     // 本地路径
    std::vector<std::string>  sticker_ids;
    bool is_system = false;
    bool is_public = false;
    std::string short_name;
    int  install_count = 0;
};

extern std::vector<Pack> g_packs;

// 启动后异步拉
void fetchMyPacks(HWND notify);
void fetchMyStickers(HWND notify);
void fetchPackContents(HWND notify, const std::string& pack_id);

// 操作（异步线程 + WM_APP 通知）
void sharePack(HWND notify, const std::string& pack_id, bool is_public);
void deletePack(HWND notify, const std::string& pack_id);
void installPack(HWND notify, const std::string& short_name);

// sticker 缓存目录 = %LOCALAPPDATA%/Launcher/stickers/
std::wstring cacheDir();

}  // namespace launcher::d2d::sticker
