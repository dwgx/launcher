// Modals — 简化版本：ChangePw / Confirm / CS2 / History。
// 完整 GDI+ 等价见 tools/preview/modals.inl 980 行。
//
// 简化范围：
//   * ChangePw：3 InputBox + 提交，接 POST /api/profile/password
//   * Confirm：title + msg + 双按钮
//   * CS2：游戏详情卡 + 启动按钮（占位 — 真启动 Steam 集成留下一轮）
//   * History：登录历史 modal（占位 — 拉 /api/profile/login-history 留下一轮）
#pragma once

#include "d2d_app.h"
#include "inputbox.h"
#include "anim.h"
#include <functional>
#include <string>
#include <vector>

namespace launcher::d2d::modal {

struct ChangePwState {
    InputBox old_pw, new_pw, repeat_pw;
    int  focus = 0;
    bool busy = false;
    std::wstring error_msg;
    bool open = false;
    Tween t;          // fade in tween
};
extern ChangePwState g_change_pw;
void openChangePw();
void paintChangePwModal(D2DApp& app, float W, float H);
// WM_APP+4 触发；后台线程结束后由主线程调
void onChangePwResult(bool success);

struct ConfirmState {
    bool open = false;
    Tween t;
    std::wstring title;
    std::wstring msg;
    std::wstring yes_label;
    std::wstring no_label;
    std::function<void()> on_yes;
    bool danger = false;
};
extern ConfirmState g_confirm;
void openConfirm(const std::wstring& title, const std::wstring& msg,
                 std::function<void()> on_yes,
                 const std::wstring& yes_label = L"确定",
                 const std::wstring& no_label = L"取消",
                 bool danger = false);
void paintConfirmModal(D2DApp& app, float W, float H);

struct CS2State {
    bool open = false;
    Tween t;
};
extern CS2State g_cs2;
void openCS2();
void paintCS2Modal(D2DApp& app, float W, float H);

struct HistoryState {
    bool open = false;
    Tween t;
    // 拉真 /api/profile/login-history 后填这个；每行 = "时间 | OK | IP | 地理"
    std::vector<std::wstring> rows;
    bool loaded = false;
    int  page = 0;     // 5 行/页
};
extern HistoryState g_history;
void openHistory();
void paintHistoryModal(D2DApp& app, float W, float H);
void onHistoryResult(const std::string& body);

// AddTag — 给 Home 个人标签 chip + 添加用
struct AddTagState {
    bool open = false;
    Tween t;
    InputBox input;
    bool busy = false;
    std::wstring error_msg;
};
extern AddTagState g_addtag;
void openAddTag();
void paintAddTagModal(D2DApp& app, float W, float H);
void onAddTagResult(bool success, int status);

// CreatePack — chat picker 里 + 新建表情包
struct CreatePackState {
    bool open = false;
    Tween t;
    InputBox input;
    bool busy = false;
    std::wstring error_msg;
};
extern CreatePackState g_createpack;
void openCreatePack();
void paintCreatePackModal(D2DApp& app, float W, float H);
void onCreatePackResult(bool success);

// RenamePack
struct RenamePackState {
    bool open = false;
    Tween t;
    std::string pack_id;
    std::wstring orig_name;
    InputBox input;
    bool busy = false;
    std::wstring error_msg;
};
extern RenamePackState g_renamepack;
void openRenamePack(const std::string& pack_id, const std::wstring& orig_name);
void paintRenamePackModal(D2DApp& app, float W, float H);
void onRenamePackResult(bool success);

// 看别人主页 (chat 头像右键)
struct UserProfileState {
    bool open = false;
    Tween t;
};
extern UserProfileState g_user_profile;
void openUserProfile(const std::wstring& uid_or_nickname);
void paintUserProfileModal(D2DApp& app, float W, float H);

// 编辑自己的状态消息（"在做什么..."）
struct EditStatusTextState {
    bool open = false;
    Tween t;
    InputBox input;
    bool busy = false;
};
extern EditStatusTextState g_edit_status;
void openEditStatusText();
void paintEditStatusTextModal(D2DApp& app, float W, float H);
void onEditStatusTextResult(bool success);

// 编辑自己的个人签名 bio
struct EditBioState {
    bool open = false;
    Tween t;
    InputBox input;
    bool busy = false;
};
extern EditBioState g_edit_bio;
void openEditBio();
void paintEditBioModal(D2DApp& app, float W, float H);
void onEditBioResult(bool success);

// 消息右键上下文菜单 — chat::onMouseRDown 命中消息时打开
struct MsgContextMenuState {
    bool open = false;
    Tween t;
    POINT anchor{};
    int   src_idx = -1;        // 在 streamFor(slug) 里的索引
    std::wstring slug;         // 频道 slug
    // 拷贝快照（避免 streamFor 容器变动后悬空）
    int  kind_int = 0;         // chat::MsgKind cast int
    std::wstring body;
    std::wstring author;
    std::wstring from;
};
extern MsgContextMenuState g_msg_menu;
void openMsgContextMenu(POINT anchor_dip, int src_idx);
void paintMsgContextMenu(D2DApp& app, float W, float H);

// launcher://pack/<short> 点击 → 这个 modal 显示分享的 pack（缩略图 + 名字 + 创建人 + 添加按钮）
struct PackPreviewState {
    bool open = false;
    Tween t;
    std::string short_name;
};
extern PackPreviewState g_pack_preview_modal;
void openPackPreviewModal(const std::string& short_name);
void paintPackPreviewModal(D2DApp& app, float W, float H);

// 通用 WebView 弹窗 — 视频 / 网页都用这个，全屏铺开 WebView2 子窗口
struct WebViewModalState {
    bool open = false;
    Tween t;
    std::wstring title;        // 标题栏文字（视频文件名 / URL host）
};
extern WebViewModalState g_webview_modal;

// 播本地视频 / URL（mp4/webm/m3u8 都行）
//   src 可以是 file:///path 或 https://... 或 本地路径（自动转 file:///）
void openVideoPlayer(const std::wstring& src);
// 弹任意 URL 到嵌入浏览器
void openWebPage(const std::wstring& url);
void paintWebViewModal(D2DApp& app, float W, float H);

// 主帧循环 tick
void tickAll(float dt);

// 任意 modal 在显示
bool anyOpen();

// 鼠标 / 键盘事件路由：return true = 已处理（modal 吃掉事件）
bool onMouseLDown(HWND hwnd, POINT dip);
bool onChar(HWND hwnd, wchar_t c, bool ctrl);
bool onKey(HWND hwnd, int vk, bool shift, bool ctrl);

}  // namespace launcher::d2d::modal
