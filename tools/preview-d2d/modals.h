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

// 商品详情 — market 卡片点击打开，拉 GET /api/market/listings/:id，含购买 + 评价。
struct MarketDetailState {
    bool open = false;
    Tween t;
    std::string listing_id;
    bool buying = false;        // 购买请求进行中，禁用 Buy 按钮（购买非幂等）
    bool reviewing = false;     // 评价请求进行中
    int  rating = 5;            // 1-5 星，默认 5
    InputBox review_input;      // 可选评价文字
    std::wstring error_msg;
};
extern MarketDetailState g_market_detail_modal;
void openMarketDetail(const std::string& id);
void paintMarketDetailModal(D2DApp& app, float W, float H);
// WM_APP+63 / +64 触发：购买 / 评价结果
void onMarketPurchaseResult(bool success);
void onMarketReviewResult(bool success);

struct HistoryState {
    bool open = false;
    Tween t;
    // 拉真 /api/profile/login-history 后填这个；每行 = "时间 · IP · 地理"
    struct Row { std::wstring text; bool success = true; };
    std::vector<Row> rows;
    bool loaded = false;
    int  page = 0;     // 5 行/页
    // 翻页横向滑动动画 —— < > 换页时旧页滑出、新页滑入（0.22s easeOutCubic）。
    Tween page_anim;   // 0→1 进度；只在 started && !done 时双页并绘
    int   prev_page = 0;
    int   slide_dir = 0;   // +1 = 下一页（新页从右侧滑入）；-1 = 上一页（从左侧滑入）
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
    std::wstring cur_key;   // 当前展示的 profile key —— 连点同一头像去重,避免重复重启弹出动画
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

// 编辑自己的昵称 nickname（走 /api/profile/nickname，有冷却限流）
struct EditNicknameState {
    bool open = false;
    Tween t;
    InputBox input;
    bool busy = false;
};
extern EditNicknameState g_edit_nickname;
void openEditNickname();
void paintEditNicknameModal(D2DApp& app, float W, float H);
// status = HTTP 状态码（204 成功 / 429 冷却 / 400 非法 / 其他失败）
void onEditNicknameResult(unsigned int status);

// 消息右键上下文菜单 — chat::onMouseRDown 命中消息时打开
struct MsgContextMenuState {
    bool open = false;
    Tween t;
    POINT anchor{};
    LayoutRect menu_rect{};
    int   src_idx = -1;        // 在 streamFor(slug) 里的索引
    std::wstring slug;         // 频道 slug
    // 拷贝快照（避免 streamFor 容器变动后悬空）
    int  kind_int = 0;         // chat::MsgKind cast int
    std::wstring body;
    std::wstring author;
    std::wstring from;
    std::wstring profile_key;
    int64_t server_id = 0;
    std::string client_msg_id;
    std::wstring reply_author;
    std::wstring reply_preview;
};
extern MsgContextMenuState g_msg_menu;
void openMsgContextMenu(POINT anchor_dip, int src_idx);
void paintMsgContextMenu(D2DApp& app, float W, float H);

struct UserContextMenuState {
    bool open = false;
    Tween t;
    POINT anchor{};
    LayoutRect menu_rect{};
    std::wstring profile_key;
    std::wstring label;
};
extern UserContextMenuState g_user_menu;
void openUserContextMenu(POINT anchor_dip, const std::wstring& profile_key, const std::wstring& label);
void paintUserContextMenu(D2DApp& app, float W, float H);

// 聊天头部「更多」弹出菜单：搜索 / 成员 / 公告。
// 复用 UserContextMenuState 的几何/交互模式；多加 view/scroll 让同一个弹层能在
// 3 项主菜单(view==0)与成员子列表(view==1)之间就地切换，避免再开第二个 modal 结构。
struct ChatMoreMenuState {
    bool open = false;
    Tween t;
    POINT anchor{};          // header 设置（chat_paint.cpp -> More 按钮下方）
    LayoutRect menu_rect{};  // 每帧计算；供 outside-click / RDown 命中判定
    int  view = 0;           // 0 = 主菜单(3 项)  1 = 成员子列表
    int  scroll = 0;         // 成员子列表滚动偏移（行）
};
extern ChatMoreMenuState g_chat_more;
void openChatMoreMenu(POINT anchor_dip);
void paintChatMoreMenu(D2DApp& app, float W, float H);
void closeChatMoreMenu();

struct MuteUserState {
    bool open = false;
    Tween t;
    InputBox duration;
    InputBox reason;
    int unit = 1; // 0 seconds, 1 minutes, 2 hours, 3 days
    int focus = 1; // 0 duration, 1 reason
    bool busy = false;
    std::wstring target_user_id;
    std::wstring target_label;
    std::string chat_id;
    std::wstring error_msg;
};
extern MuteUserState g_mute_user;
void openMuteUser(const std::wstring& target_user_id, const std::wstring& target_label);
void paintMuteUserModal(D2DApp& app, float W, float H);
void onMuteUserResult(bool success);
void onUnmuteUserResult(bool success);

// launcher://pack/<short> 点击 → 这个 modal 显示分享的 pack（缩略图 + 名字 + 创建人 + 添加按钮）
struct PackPreviewState {
    bool open = false;
    Tween t;
    std::string short_name;
};
extern PackPreviewState g_pack_preview_modal;
void openPackPreviewModal(const std::string& short_name);
void paintPackPreviewModal(D2DApp& app, float W, float H);

// 全局消息搜索 — Ctrl+F 触发
struct SearchHit {
    int64_t      msg_id = 0;
    std::wstring slug;
    std::wstring chat_id;
    std::wstring sender_id;
    std::wstring kind;
    std::wstring payload;
    std::wstring time;
};
struct SearchState {
    bool open = false;
    Tween t;
    InputBox input;
    bool busy = false;
    int  scroll = 0;          // 列表滚动偏移
    std::vector<SearchHit> results;
    std::wstring last_query;  // 防抖
};
extern SearchState g_search;
void openSearch();
void paintSearchModal(D2DApp& app, float W, float H);
void onSearchResult(const std::string& body);
// main thread drain — 把后台 thread 拉来的 body 解析并填进 g_search.results
void drainSearchResult();

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
bool hasBlockingModalOpen();
void closeContextMenus();

// 鼠标 / 键盘事件路由：return true = 已处理（modal 吃掉事件）
bool onMouseLDown(HWND hwnd, POINT dip);
bool onMouseRDown(HWND hwnd, POINT dip);
bool onChar(HWND hwnd, wchar_t c, bool ctrl);
bool onKey(HWND hwnd, int vk, bool shift, bool ctrl);

}  // namespace launcher::d2d::modal
