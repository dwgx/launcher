// chat_internal.h — chat 模块内部共享契约（仅供 chat_*.cpp 包含，不对外）。
//
// 背景：chat.cpp 原本是 3692 行的“上帝文件”，把状态、消息身份、公告、网络收发、
// D2D 渲染、输入处理全塞在一个翻译单元里。本头文件把原先散落在 chat.cpp 内的
// 文件级 static 状态与内部 struct 抽出，统一声明为 extern，使其能被拆分后的
// 多个 chat_*.cpp 共享。状态的“定义”集中在 chat_state.cpp 一个翻译单元里。
//
// 对外契约仍然只看 chat.h —— 本头文件不改变任何 chat:: 公共符号。
#pragma once

#include "chat.h"
#include "render/primitives.h"
#include "sticker.h"

#include <cstdint>
#include <ctime>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace launcher::d2d::chat {

// ============================================================
//  内部 struct（原先定义在 chat.cpp 文件中段）
// ============================================================

struct ReplySnapshot {
    std::wstring slug;
    std::wstring author;
    std::wstring preview;
};

// 头像 hit 表 — paintChatPane 帧首清空，paintBubble 填充，WM_RBUTTONDOWN 命中
struct AvatarHit { LayoutRect rect; std::wstring peer_key; };

// 消息体 hit 表 — paintChatPane 帧首清空，paintBubble 填充
struct MsgHit { LayoutRect rect; int idx; };

struct FocusTarget {
    std::wstring slug;
    int64_t server_id = 0;
    float highlight_until = 0.0f;
    bool scroll_applied = false;
    bool missing_reported = false;
};

// 当前 picker 内 pack tab 实际像素位置（paintPicker 写，鼠标命中读用以拖拽）
struct PackTabRect { LayoutRect r; int idx; };

struct AnnouncementItem {
    std::string id;
    std::wstring title;
    std::wstring body;
    std::string severity;
    bool force_popup = false;
    bool red_dot = false;
    bool unread = false;
    bool acknowledged = false;
};

struct PickerScrollDrag {
    bool active = false;
    int mode = 0; // 0 = emoji grid, 1 = sticker grid
    float anchor_mouse_y = 0.0f;
    float anchor_scroll_y = 0.0f;
    float track_h = 0.0f;
    float thumb_h = 0.0f;
    float max_scroll = 0.0f;
};

struct ComposerDrag {
    bool active = false;
    int anchor_cursor = 0;
};

enum class HistoryLoadState { NotStarted, Loading, Loaded, Exhausted, Failed };

struct HistoryResult {
    bool ok = false;
    int64_t before_id = 0;
    std::vector<Msg> msgs;
};

struct WrappedText {
    std::wstring text;
    DWRITE_TEXT_METRICS metrics{};
    int line_count = 0;
    float text_h = 0.0f;
    float max_line_w = 0.0f;
};

// ============================================================
//  共享状态（定义在 chat_state.cpp）
// ============================================================

extern const wchar_t* kGroups[4];
extern std::vector<std::wstring> g_channel_string_pool;

extern std::unordered_map<std::wstring, std::vector<Msg>> g_streams;
extern std::unordered_map<std::wstring, bool> g_group_collapsed;
extern std::unordered_map<std::wstring, Tween> g_group_anim;
extern std::mutex g_streams_mtx;

extern std::unordered_map<int64_t, ReplySnapshot> g_reply_snapshots;
extern std::mutex g_reply_snapshots_mtx;
extern std::unordered_map<std::wstring, float> g_peer_profile_requested_at;
extern std::mutex g_peer_profile_requested_mtx;

extern std::vector<AvatarHit> g_avatar_hits;
extern float g_emoji_scroll_y;
extern float g_emoji_grid_h_last;
extern float g_emoji_total_h_last;
extern std::vector<float> g_emoji_hover_t;
extern float g_pack_scroll_y;
extern float g_pack_grid_h_last;
extern float g_pack_total_h_last;

extern std::vector<MsgHit> g_msg_hits;
extern std::vector<MsgHit> g_msg_row_hits;
extern LayoutRect g_chat_stream_rect;
extern FocusTarget g_focus_target;

extern std::vector<PackTabRect> g_pack_tab_rects;
extern float g_picker_origin_x, g_picker_origin_y;
extern LayoutRect g_picker_rect;
extern LayoutRect g_picker_content_rect;
extern LayoutRect g_emoji_button_rect;
extern Tween g_picker_content_t;
extern int g_picker_content_tab;

extern std::vector<AnnouncementItem> g_announcements;
extern std::mutex g_announcements_mtx;
extern std::wstring g_bootstrap_role;
extern bool g_bootstrap_is_admin;
extern AnnouncementItem g_popup_announcement;
extern bool g_popup_open;
extern Tween g_popup_t;
extern bool g_announcements_stream_dirty;

extern PickerScrollDrag g_picker_scroll_drag;
extern ComposerDrag g_composer_drag;
extern std::vector<float> g_composer_caret_xs;
// composer 光标的 DIP 坐标(paint 每帧更新);IME 用它把候选窗定位到光标处。
extern float g_composer_caret_dip_x;
extern float g_composer_caret_dip_y;   // 光标行底部 y(候选窗贴其下方)

extern std::unordered_map<std::wstring, HistoryLoadState> g_history_state;

extern std::mutex g_pending_hist_mtx;
extern std::unordered_map<std::wstring, HistoryResult> g_pending_history;

constexpr float kStreamTopPad = 12.0f;
constexpr float kStreamBottomPad = 18.0f;
constexpr int kHistoryPageLimit = 100;

// ============================================================
//  共享内部辅助（小函数定义在 chat_state.cpp）
// ============================================================

inline uint32_t fadeArgb(uint32_t argb, float op) {
    uint32_t a = (argb >> 24) & 0xFFu;
    a = (uint32_t)(a * op + 0.5f);
    if (a > 255) a = 255;
    return (a << 24) | (argb & 0xFFFFFFu);
}

// 反应 chip 行高度（有反应时占一行固定高度）。measureBubbleHeight/paintBubble 共用，保持镜像。
constexpr float kReactionRowH = 26.0f;
inline float reactionRowHeight(const Msg& m) {
    return m.reactions.empty() ? 0.0f : kReactionRowH;
}

float clampf(float v, float lo, float hi);
float groupAnimValue(const std::wstring& name);
void toggleGroupCollapsed(const std::wstring& name);
void setPickerOpen(bool open);
void setPickerTabSmooth(int tab);
std::wstring localTimeText(time_t tt = time(nullptr));

// ---- chat_identity.cpp ----
void normalizeMsgIdentity(Msg& m);
bool fillReplySnapshot(Msg& m);
bool sameMessageIdentity(const Msg& a, const Msg& b);
bool sameMessageIdentityStrict(const Msg& a, const Msg& b);
void mergeServerIdentity(Msg& local, const Msg& server);
void rememberReplySnapshot(const std::wstring& slug, const Msg& m);
int64_t oldestServerId(const std::wstring& slug);
bool hasMessageServerId(const std::wstring& slug, int64_t server_id);
int64_t serverIdForClientMsgId(const std::wstring& slug, const std::string& client_msg_id);
void resolveLocalReplyTargets(const std::wstring& slug);
bool isFocusWaitingForSlug(const std::wstring& slug);
std::wstring displayAuthorFor(const Msg& m, HWND notify = nullptr);
std::wstring msgAuthorDisplay(const Msg& m);
std::wstring selfAuthorKey();
std::wstring shortPeerLabel(const std::wstring& key);
bool looksLikeUuidW(const std::wstring& s);
void requestPeerProfileOnce(const std::wstring& key, HWND notify);
std::wstring msgPreviewText(const Msg& m);
std::wstring utf8wHist(const std::string& s);
std::wstring mediaLocalPathFromUrl(const std::string& media_url);
std::wstring bodyFromPayloadObject(const std::string& payload_obj);
std::wstring bodyFromMessageObject(const std::string& msg_obj);

// ---- chat_announcements.cpp ----
void syncAnnouncementNoticeOnChannels();
std::wstring announcementSeverityLabel(const std::string& severity);
void rebuildAnnouncementStream();
void postAnnouncementMark(const std::string& id, bool ack);
void markAnnouncementsReadLocal(bool ack_popup, const std::string& only_id = {});

// ---- chat_net.cpp ----
std::string makeClientMsgId();
void sendChatMessage(HWND hwnd, const std::wstring& body, const char* kind,
                     std::string client_msg_id = {}, int64_t reply_to_id = 0);
bool sendTextMessage(HWND hwnd, const std::wstring& text);

// ---- chat_channel.cpp（频道写权限判定）----
Channel* activeChannel();
bool currentUserCanWriteRestrictedChannel();
bool canWriteChannel(const Channel* ch);
bool canWriteActiveChannel();
std::wstring activeWriteBlockedMessage();
bool requireActiveChannelWrite();
std::wstring normalizeSendErrorText(std::wstring err);

// ---- chat_render.cpp（文本测量 + 绘制）----
float measureW(D2DApp& app, std::wstring_view s, IDWriteTextFormat* fmt);
float spaceW(D2DApp& app, IDWriteTextFormat* fmt);
float caretMeasureW(D2DApp& app, std::wstring_view s, IDWriteTextFormat* fmt);
std::wstring fitTextOneLine(D2DApp& app, const std::wstring& s, IDWriteTextFormat* fmt, float max_w);
void drawTextOneLine(ID2D1DeviceContext* ctx, std::wstring_view text,
                     IDWriteTextFormat* fmt, float x, float y, float w, float h,
                     ID2D1Brush* b,
                     DWRITE_TEXT_ALIGNMENT halign = DWRITE_TEXT_ALIGNMENT_LEADING,
                     DWRITE_PARAGRAPH_ALIGNMENT valign = DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
int cursorFromComposerPoint(float x);
float dwriteMaxLineWidth(D2DApp& app, const std::wstring& text,
                         IDWriteTextFormat* fmt, float max_w, float max_h);
WrappedText wrapTextForWidth(D2DApp& app, std::wstring_view src, IDWriteTextFormat* fmt, float max_w);
std::wstring authorKeyFor(const Msg& m);
bool sameGroupedAuthor(const Msg& prev, const Msg& cur);
bool messageHasReplyPreview(const Msg& m);
std::wstring replyPreviewLine(const Msg& m);
float replyPreviewHeight(const Msg& m);
float measureBubbleHeight(D2DApp& app, const Msg& m, float maxw, bool prev_same_author);
float paintBubble(D2DApp& app, const Msg& m, int idx, float x, float y, float maxw,
                  bool prev_same_author, float op);
void paintChatList(D2DApp& app, float ax, float ay, float aw, float ah);
void paintChatPane(D2DApp& app, float ax, float ay, float aw, float ah);
void paintComposer(D2DApp& app, float ax, float ay, float aw, float ah);
void paintPicker(D2DApp& app, float anchor_x, float anchor_y);

// ---- chat.cpp（核心：频道切换 / 流管理 / tick）----
void retargetTopSeg();
void retargetPackTab();

} // namespace launcher::d2d::chat
