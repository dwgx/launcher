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

// 导入文件夹 — 扫描 *.png/*.jpg/*.jpeg/*.gif/*.webp/*.bmp 异步逐张上传：
//   POST /api/media/upload → media_id
//   POST /api/sticker      → sticker_id (含 pack_id)
//   写本地缓存 → 加进 g_packs[pack_id 对应] 的 stickers
// 完成后 PostMessage WM_APP+29 (wp = 上传成功数)
void importFromFolder(HWND notify, const std::wstring& folder_path,
                      const std::string& pack_id);

// 弹 SHBrowseForFolder + 调 importFromFolder
void importFromFolderUi(HWND notify, const std::string& pack_id);

// 用户名下所有 sticker（含全部自创 pack）总数 — 后端限 50/user
int totalUserStickers();

// 删单个 sticker — 本地从 g_packs 移除路径 + POST /api/sticker/delete
// (后端可能没此端点；客户端本地删除立即生效，后端失败 silent)
void deleteSticker(HWND notify, const std::wstring& path);

// sticker 缓存目录 = %LOCALAPPDATA%/Launcher/stickers/
std::wstring cacheDir();

}  // namespace launcher::d2d::sticker
