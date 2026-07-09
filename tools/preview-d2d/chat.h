// Chat view — 1:1 复刻 tools/preview/chat_view.inl 关键骨架。
// 简化掉：picker / sticker pack / GIF/video bubble / 头像下载 / WS（留下一轮接通）。
// 保留：8 官方频道写死 / channel list 分组折叠 / text bubble + day/system / composer
//       (含选区 + 光标 + Ctrl+ACVX) + 真后端发消息（POST /api/chat/send）。
#pragma once

#include "d2d_app.h"
#include "inputbox.h"
#include "textedit.h"
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
    bool          notice = false; // bootstrap announcement red dot
    std::string   write_policy;
    std::string   allowed_role;
    int           min_level = 1;
    int           slowmode_seconds = 0;
    bool          requires_subscription = false;
    bool          is_readonly = false;
    bool          is_locked = false;
};

extern std::vector<Channel> g_channels;
extern std::wstring g_active;        // 当前频道 slug
extern MultilineEdit g_composer;
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
void setPickerOpen(bool open);   // 公开:统一 overlay 派发(modals.cpp)关闭 picker 用
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

// 一条消息上的某个 emoji 聚合（count = 该 emoji 的总反应数，mine = 当前用户是否也点了）。
// 只由 WS "reaction" 事件 + 本地乐观切换维护 —— 后端 history/MessageOut 不带 reactions，
// 冷加载看不到旧消息的反应（见 feature 设计 risks）。
struct Reaction {
    std::wstring emoji;
    int          count = 0;
    bool         mine  = false;
};

struct Msg {
    MsgKind kind = MsgKind::Text;
    std::wstring from;        // "me" 表示自己
    std::wstring peer_key;    // full sender id / profile lookup key for non-me messages
    std::wstring author_key;  // stable sender key for de-dupe/profile lookup
    std::wstring author;
    std::wstring status;
    std::wstring body;
    std::wstring time;
    // BLURHASH SEAM (Wave3)：backend 下发的 blurhash 占位串。现阶段仅布线，
    // chat_net 解析处留空；paint 把它透传给 ImageCache::fromFile 供未来占位解码。
    std::string  blurhash;
    MsgSendState send_state = MsgSendState::Sent;
    std::wstring error_text;
    std::string  client_msg_id;
    int64_t      server_id = 0;     // 后端 messages.id (软删除时 POST /chat/delete 用)
    int64_t      reply_to_id = 0;
    std::string  reply_client_msg_id;
    std::wstring reply_author;
    std::wstring reply_preview;
    bool         waiting_reply_target = false;
    bool         recalled = false;     // 已撤回:保留行但渲染「X 撤回了一条消息」墓碑
    std::vector<Reaction> reactions;   // emoji 反应聚合（WS 事件驱动，见 Reaction 注释）
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
// 点是否落在消息流区域(NCHITTEST 用:流区应为 HTCLIENT,否则纯文本消息行
// 会被判成 HTCAPTION 拖窗 → 右键收不到 WM_RBUTTONDOWN → 菜单弹不出)。
bool pointInStream(POINT dip);
// composer 光标 DIP 坐标(IME 候选窗定位用;paint 每帧更新)。
extern float g_composer_caret_dip_x;
extern float g_composer_caret_dip_y;
// IME 自绘内联:当前组合串(拼音),WM_IME_COMPOSITION 更新,composer paint 在光标处画出。
extern std::wstring g_ime_composition;
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

bool hasUnreadAnnouncements();
void paintAnnouncementModal(D2DApp& app, float W, float H);

// 拖拽文件进 chat → 添加 image bubble + 上传后端
void appendMedia(const std::wstring& path);

// 公共：把消息追加到当前频道，并且如果此频道用户在底部就自动跟随到底
void appendLocalMessage(Msg msg);

// 切换账号（登录成功）时清空所有频道消息缓存，避免上一个账号的
// "me" 消息在新账号下仍然右对齐（render-time self 判定靠当前 user_id）。
void resetForAccount();

// 判定一条消息是否属于"当前登录用户"。不再只看 m.from==L"me" 字面量，
// 而是把 author_key 与当前 g_user_id/uid/username 比对，避免跨账号污染。
bool isSelfMessage(const Msg& m);

// 公共：按 server_id/client_msg_id 合并；没有重复时追加到指定频道
bool appendOrMergeMessage(const std::wstring& slug, Msg msg);

// 搜索/外部导航：按后端 message id 定位已加载消息。
// 如果历史还没合并，目标会保留到下一帧绘制时再应用。
void focusMessage(const std::wstring& slug, int64_t server_id);

// 拉某频道历史消息 (GET /api/chat/history?session_token=&chat_id=) → WM_APP+45
void fetchHistory(HWND notify, const std::wstring& slug);

// 主线程 WM_APP+45 调 — 把后台拉到的历史 merge 到 streamFor(slug)
void applyHistoryResult(const std::wstring& slug);

// 主线程 WM_APP+52 调 — 把刚发的 me 消息绑定上后端 server_id
void applySendResult();

// 删除消息 — 本地立即移 + 后端软删除（仅自己消息有效）
void deleteMessage(HWND hwnd, const std::wstring& slug, int64_t server_id);
// 撤回消息 — 30秒时窗内(全局配置),留「已撤回」墓碑(仅自己消息)
void recallMessage(HWND hwnd, const std::wstring& slug, int64_t server_id);
void onWsMessageRecalled(int64_t server_id);

// WS 收到 type=delete 事件 — 在所有 stream 里找匹配 server_id 摘掉
void onWsMessageDeleted(int64_t server_id);

// ============== 已读回执 + emoji 反应 ==============
// 标记已读：解析 slug→chat_id，POST /api/chat/read（fire-and-forget，后端 GREATEST 容重复）。
// up_to_id 必须是后端 message id（server_id>0），pending 消息无 id 不发。
void markRead(HWND hwnd, const std::wstring& slug, int64_t up_to_id);

// 对某条消息加/去 emoji 反应：本地乐观切换 + POST /api/chat/react（失败回 WM_APP+60 回滚）。
void reactToMessage(HWND hwnd, const std::wstring& slug, int64_t server_id,
                    const std::wstring& emoji, bool remove);

// WM_APP+60 主线程回调 — react 失败时按 (slug, server_id, emoji, was_remove) 回滚本地聚合。
void applyReactFailure();

// picker 复用 —— 右键菜单点 "React" 时置位，picker cell 命中读它决定是"插入 composer"还是"发反应"。
struct ReactTarget {
    std::wstring slug;
    int64_t      server_id = 0;
    bool         active    = false;
};
extern ReactTarget g_react_target;

// 从消息右键菜单进入"选 emoji 反应"模式：置位 g_react_target 并打开 picker。
// picker cell 命中时读 g_react_target.active 决定是发反应还是插入 composer。
void beginReactPick(const std::wstring& slug, int64_t server_id);

// WS "reaction" 事件 — 在所有 stream 里按 server_id 定位消息，增减该 emoji 计数。
void onWsReaction(int64_t message_id, const std::wstring& actor_id,
                  const std::wstring& emoji, bool remove);
// WS "read" 事件 — 记录某频道内某 peer 的 last_read message id（渲染 "已读" 标记用）。
void onWsRead(const std::wstring& chat_id, const std::wstring& actor_id, int64_t up_to_message_id);
// 查询：自己在 slug 频道的某条消息 server_id 是否已被任一 peer 读过。
bool messageReadByPeer(const std::wstring& slug, int64_t server_id);

}  // namespace launcher::d2d::chat
