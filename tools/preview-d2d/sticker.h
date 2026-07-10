// Sticker pack 全套管理 — 1:1 复刻 GDI+ Preview chatv::Pack。
//   * 启动后 fetchMyPacks → GET /api/sticker/packs/mine 拉用户表情包
//   * fetchMyStickers → GET /api/sticker/mine 拉单个 sticker（同账号多设备同步）
//   * fetchPackContents(pack_id) → GET /api/sticker/pack/:id 拉 pack 里的 stickers
//   * sharePack / deletePack / installPack 操作
#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace launcher::d2d::sticker {

struct StickerItem {
    std::wstring path;       // local cache path for preview/rendering
    std::string sticker_id;  // backend stickers.id
    int64_t media_id = 0;
    std::string media_url;   // /api/media/<sha>/file.<ext>, remote payload source
    std::string sha256;
    std::string mime;
};

struct Pack {
    std::string  id;
    std::wstring name;
    std::vector<std::wstring> stickers;     // 本地路径，保留给现有 picker/render 兼容
    std::vector<std::string>  sticker_ids;  // 与 stickers 同步，保留给旧调用
    std::vector<StickerItem>  items;        // 新主数据：本地路径 + 远端 id/url
    bool is_system = false;
    bool is_public = false;
    bool is_owner  = false;        // 当前用户是否是 pack 创建人
    std::wstring creator_name;     // by xxx
    std::string  short_name;
    int  install_count = 0;
};

extern std::vector<Pack> g_packs;
extern std::mutex        g_packs_mtx;

// 内置分组的 canonical 名 —— 这是与后端约定的【数据标识】，不是 UI 文案，
// 永不本地化（后端按此 name 存"我的表情"pack）。显示层用 tr("pack.*") 另取译文，
// 逻辑判定一律走 is_system 标志或 isMyStickersPack()，禁止把这些字面量当 UI 文本。
constexpr const wchar_t* kSysEmojiName   = L"系统 emoji";
constexpr const wchar_t* kMyStickersName = L"我的表情";

// 是否"我的表情"分组：本地 placeholder（is_owner 且无 backend id）或后端同名 pack。
inline bool isMyStickersPack(const Pack& p) {
    return !p.is_system && p.name == kMyStickersName;
}

// UI 显示名 —— 内置分组（系统 emoji / 我的表情）的 name 是 canonical 数据标识，
// 不能直接画到界面（英文/日文用户会看到中文）。显示层一律走此函数取译文；
// 普通用户自建 pack 用其真实 name。
std::wstring packDisplayName(const Pack& p);


// 启动后异步拉
void fetchMyPacks(HWND notify);
void fetchMyStickers(HWND notify);
void fetchPackContents(HWND notify, const std::string& pack_id);

// 操作（异步线程 + WM_APP 通知）
void sharePack(HWND notify, const std::string& pack_id, bool is_public);
void deletePack(HWND notify, const std::string& pack_id);

// 拖拽排序后调 — 把 backend pack id 顺序提交到云端
// (跳过系统 emoji 的本地伪 pack，那个 client 端 persist::saveSystemPackOrder 单独存)
void reorderPacks(HWND notify, const std::vector<std::string>& pack_ids);

// 导出 pack 到指定文件夹 — 把 stickers 本地路径下的文件全部 CopyFile 过去
// 文件名沿用 sha 哈希。返回成功导出张数（异步执行 + WM_APP+43 通知）
void exportPackToFolder(HWND notify, const std::string& pack_id);

// 看分享链接对应的 pack（GET /api/sticker/pack/by-short/:short）
// 完成后写入 g_pack_preview 并 PostMessage WM_APP+44
void previewPackByShort(HWND notify, const std::string& short_name);

// 一键安装：previewPackByShort + 自动 install + fetchMyPacks
void installPackByShort(HWND notify, const std::string& short_name);

// pack 预览（PackPreview modal 用）
struct PackPreview {
    bool         loaded = false;
    std::string  err;
    std::string  id;
    std::string  short_name;
    std::wstring name;
    std::wstring creator_name;
    int          install_count = 0;
    bool         already_installed = false;
    std::vector<std::wstring> sticker_paths;   // 本地 cache 路径
    std::vector<StickerItem>  items;           // same stickers with remote ids/urls
    std::wstring cover_path;                   // 第一张图本地路径
};
extern PackPreview g_pack_preview;
extern std::mutex  g_pack_preview_mtx;

// 导入文件夹 — 扫描 *.png/*.jpg/*.jpeg/*.gif/*.webp/*.bmp 异步逐张上传：
//   POST /api/media/upload → media_id
//   POST /api/sticker      → sticker_id (含 pack_id)
//   写本地缓存 → 加进 g_packs[pack_id 对应] 的 stickers
// 完成后 PostMessage WM_APP+29 (wp = 上传成功数)
void importFromFolder(HWND notify, const std::wstring& folder_path,
                      const std::string& pack_id);

// 弹 SHBrowseForFolder + 调 importFromFolder
void importFromFolderUi(HWND notify, const std::string& pack_id);

// 弹多选文件对话框（PNG/JPEG/GIF/WEBP）+ 逐张 importOneFile 上传/入包
// 完成后 PostMessage WM_APP+29 (wp = 上传成功数)
void importFilesUi(HWND notify, const std::string& pack_id);

// 用户名下所有 sticker（含全部自创 pack）总数 — 后端限 50/user
int totalUserStickers();

// 删单个 sticker — 本地从 g_packs 移除路径 + POST /api/sticker/delete
// (后端可能没此端点；客户端本地删除立即生效，后端失败 silent)
void deleteSticker(HWND notify, const std::wstring& path);

// 给 chat 接入用：不要把 local path 作为远端 payload。
// stickerPayloadJsonForPath 返回 {"sticker_id","media_id","media_url","url","sha256","mime"}。
bool stickerForPath(const std::wstring& path, StickerItem* out);
std::string stickerPayloadJsonForPath(const std::wstring& path);

// 启动后调 — 如果 g_packs 里"我的表情"(name=='我的表情') 还没真 backend pack id，
// 自动 POST /api/sticker/pack 创建一个名为"我的表情"的 pack，把 id 写回。
// 这样"我的表情" tab 也能直接走 importFromFolder。
void ensureMyStickersPack(HWND notify);


}  // namespace launcher::d2d::sticker
