// Chat view — 1:1 复刻 tools/preview/chat_view.inl 关键骨架。
// 简化掉：picker / sticker pack / GIF/video bubble / 头像下载 / WS（留下一轮接通）。
// 保留：8 官方频道写死 / channel list 分组折叠 / text bubble + day/system / composer
//       (含选区 + 光标 + Ctrl+ACVX) + 真后端发消息（POST /api/chat/send）。
#pragma once

#include "d2d_app.h"
#include "inputbox.h"
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace launcher::d2d::chat {

struct Channel {
    const wchar_t* slug;       // backend slug（也是 g_active key）
    const wchar_t* name;
    const wchar_t* group;      // IMPORTANT / GENERAL / GAMES / SHOP
    bool is_market;
    std::string   id;          // backend uuid (启动后 GET /api/chat/official 填)
    int           write_role;  // 0 = user, 1 = admin_only
};

extern std::vector<Channel> g_channels;
extern std::wstring g_active;        // 当前频道 slug
extern InputBox     g_composer;
extern bool         g_focus_composer;

struct PendingReply {
    bool active = false;
    int64_t id = 0;
    std::string client_msg_id;
    std::wstring slug;
    std::wstring author;
    std::wstring author_key;
    std::wstring preview;
};
extern PendingReply g_pending_reply;

struct PendingMention {
    std::wstring user_id;
    std::wstring label;
};
extern std::vector<PendingMention> g_pending_mentions;
void addMentionToComposer(const std::wstring& user_id, const std::wstring& label);

// Emoji / sticker picker
extern bool  g_picker_open;
extern Tween g_picker_t;
extern int   g_picker_tab;     // 0 = emoji, 1+ = sticker pack idx (1+ = 索引到 sticker::g_packs[idx-1])

// 顶部 tab 滑块动画（[表情] [表情包] pill）
extern Tween g_top_seg_x, g_top_seg_w;
// pack tab 滑块动画（pack list 里 active pill）
extern Tween g_pack_tab_x, g_pack_tab_w;

// pack tab 拖拽排序状态 — paintPicker 写、 onMouse* 读
struct PackDrag {
    int   from      = -1;     // 在 g_packs 里的索引（拖动源）
    int   over      = -1;     // 当前鼠标悬停的目标索引
    float anchor_dx = 0;      // 抓取时鼠标距 tab 左边距 — 让拖动跟手
    float start_x   = 0;      // 鼠标按下时的 x — 用来判断是否真拖动了
    bool  moved     = false;  // 鼠标已经离开起点（区分 click vs drag）
};
extern PackDrag g_pack_drag;

struct PackActionPayload {
    std::string id;
    std::wstring name;
};

struct MsgContextPayload {
    POINT pt{};
    int idx = -1;
};

// per-channel 滚动偏移（从底部往上的像素数；0 = 锁底，新消息自动跟）
struct ChatScroll {
    float offset_from_bottom = 0;     // 当前实际渲染用值（每帧 lerp 朝 target 平滑）
    float target_offset      = 0;     // 滚轮 / 拖动写这个，offset 跟随
    float total_height       = 0;
    float viewport_h         = 0;
    size_t rendered_count    = 0;
    int64_t tail_server_id   = 0;
    std::string tail_client_msg_id;
    bool  initialized        = false; // false = 首次进入这个频道，会自动 stick to bottom
};
extern std::unordered_map<std::wstring, ChatScroll> g_scroll;

// 滚动条拖动状态
struct ScrollBarDrag {
    bool  active = false;
    float anchor_mouse_y = 0;
    float anchor_offset  = 0;
    float bar_track_y    = 0;
    float bar_track_h    = 0;
    float total_height   = 0;
    float viewport_h     = 0;
};
extern ScrollBarDrag g_scroll_drag;

enum class MsgKind { Text, System, DayDivider, Image, Sticker, Gif, Video };
enum class MsgSendState { Sent, Pending, Failed };

struct Msg {
    MsgKind kind = MsgKind::Text;
    std::wstring from;        // "me" 表示自己
    std::wstring peer_key;    // full sender id / profile lookup key for non-me messages
    std::wstring author_key;  // stable sender key for de-dupe/profile lookup
    std::wstring author;
    std::wstring status;
    std::wstring body;
    std::wstring time;
    MsgSendState send_state = MsgSendState::Sent;
    std::wstring error_text;
    std::string  client_msg_id;
    int64_t      server_id = 0;     // 后端 messages.id (软删除时 POST /chat/delete 用)
    int64_t      reply_to_id = 0;
    std::string  reply_client_msg_id;
    std::wstring reply_author;
    std::wstring reply_preview;
    bool         waiting_reply_target = false;
};

std::vector<Msg>& streamFor(const std::wstring& slug);
void switchChannel(const std::wstring& slug);
std::string activeChatId();
void beginReplyToMessage(const std::wstring& slug, int64_t server_id,
                         const std::string& client_msg_id,
                         const std::wstring& author,
                         const std::wstring& author_key,
                         const std::wstring& preview);

// 顶层 paint — 分两半：list 240 / pane 1fr
void paintChatView(D2DApp& app, float ax, float ay, float aw, float ah);

void tick(float dt);

bool onMouseLDown(HWND hwnd, POINT dip);
bool onMouseMove(HWND hwnd, POINT dip);
bool onMouseLUp(HWND hwnd, POINT dip);
// 右键命中消息 → 弹消息菜单；命中头像 → 看主页
bool onMouseRDown(HWND hwnd, POINT dip);
// 滚轮：delta = WHEEL_DELTA 的倍数（120 = 一格）
void onWheel(int delta);
void onChar(HWND hwnd, wchar_t c, bool ctrl);
void onKey(HWND hwnd, int vk, bool shift, bool ctrl);

// 把 pack tab 动画 reset 到当前 active idx —— 切 picker tab、重排序后调
void retargetPackTab();
// 把顶部 [表情]/[表情包] 滑块 retarget 到当前 g_picker_tab
void retargetTopSeg();

// 启动后异步拉 GET /api/chat/official 把 slug → id 填入 g_channels
void fetchOfficialChannels(HWND notify);

// main thread WM_APP+5 调
void applyOfficialResult();

// 拖拽文件进 chat → 添加 image bubble + 上传后端
void appendMedia(const std::wstring& path);

// 公共：把消息追加到当前频道，并且如果此频道用户在底部就自动跟随到底
void appendLocalMessage(Msg msg);

// 公共：按 server_id/client_msg_id 合并；没有重复时追加到指定频道
bool appendOrMergeMessage(const std::wstring& slug, Msg msg);

// 搜索/外部导航：按后端 message id 定位已加载消息。
// 如果历史还没合并，目标会保留到下一帧绘制时再应用。
void focusMessage(const std::wstring& slug, int64_t server_id);

// 拉某频道历史消息 (GET /api/chat/history?session_token=&chat_id=) → WM_APP+45
void appendLocalMessage(Msg msg);
bool appendOrMergeMessage(const std::wstring& slug, Msg msg);
void focusMessage(const std::wstring& slug, int64_t server_id);
void fetchHistory(HWND notify, const std::wstring& slug);

// 主线程 WM_APP+45 调 — 把后台拉到的历史 merge 到 streamFor(slug)
void applyHistoryResult(const std::wstring& slug);

// 主线程 WM_APP+52 调 — 把刚发的 me 消息绑定上后端 server_id
void applySendResult();

// 删除消息 — 本地立即移 + 后端软删除（仅自己消息有效）
void deleteMessage(HWND hwnd, const std::wstring& slug, int64_t server_id);

// WS 收到 type=delete 事件 — 在所有 stream 里找匹配 server_id 摘掉
void onWsMessageDeleted(int64_t server_id);

}  // namespace launcher::d2d::chat
