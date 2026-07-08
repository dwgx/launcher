// chat_state.cpp — chat 模块全部文件级共享状态的“唯一定义点”。
//
// 原先这些 static 状态散在 chat.cpp 顶部 200 行里，跟逻辑/渲染搅在一起。
// 拆分后统一在此定义（对应 chat_internal.h 的 extern 声明），让 chat_*.cpp
// 各翻译单元共享同一份状态。公共符号（chat.h 里 extern 的）也在此定义。
#include "chat.h"
#include "chat_internal.h"
#include "render/primitives.h"

#include <ctime>

namespace launcher::d2d::chat {

// ---- 公共状态（chat.h extern）----
// 8 官方频道（不要 touhou/vrchat — 跟 GDI+ 那边一致）
std::vector<Channel> g_channels = {
    { L"announcements", L"announcements", L"IMPORTANT", false, "", 1 },
    { L"rules",         L"rules",         L"IMPORTANT", false, "", 1 },
    { L"general",       L"general",       L"GENERAL",   false, "", 0 },
    { L"random",        L"random",        L"GENERAL",   false, "", 0 },
    { L"helpdesk",      L"helpdesk",      L"GENERAL",   false, "", 1 },
    { L"cs2",           L"cs2",           L"GAMES",     false, "", 1 },
    { L"market",        L"market",        L"SHOP",      true,  "", 1 },
    { L"trades",        L"trades",        L"SHOP",      false, "", 1 },
};
std::wstring g_active = L"general";
MultilineEdit g_composer;
bool         g_focus_composer = false;
PendingReply g_pending_reply;
std::vector<PendingMention> g_pending_mentions;
bool         g_picker_open = false;
Tween        g_picker_t;
int          g_picker_tab = 0;
Tween g_top_seg_x, g_top_seg_w;
Tween g_pack_tab_x, g_pack_tab_w;
PackDrag g_pack_drag;
std::unordered_map<std::wstring, ChatScroll> g_scroll;
ScrollBarDrag g_scroll_drag;
ReactTarget   g_react_target;

// ---- 内部共享状态（chat_internal.h extern）----
const wchar_t* kGroups[] = { L"IMPORTANT", L"GENERAL", L"GAMES", L"SHOP" };
std::vector<std::wstring> g_channel_string_pool;

std::unordered_map<std::wstring, std::vector<Msg>> g_streams;
std::unordered_map<std::wstring, bool> g_group_collapsed;
std::unordered_map<std::wstring, Tween> g_group_anim;
std::mutex g_streams_mtx;

std::unordered_map<int64_t, ReplySnapshot> g_reply_snapshots;
std::mutex g_reply_snapshots_mtx;
std::unordered_map<std::wstring, float> g_peer_profile_requested_at;
std::mutex g_peer_profile_requested_mtx;

std::vector<AvatarHit> g_avatar_hits;
float g_emoji_scroll_y = 0.0f;
float g_emoji_grid_h_last = 0.0f;
float g_emoji_total_h_last = 0.0f;
std::vector<float> g_emoji_hover_t;
float g_pack_scroll_y = 0.0f;
float g_pack_grid_h_last = 0.0f;
float g_pack_total_h_last = 0.0f;

std::vector<MsgHit> g_msg_hits;
std::vector<MsgHit> g_msg_row_hits;
LayoutRect g_chat_stream_rect{};
FocusTarget g_focus_target;

std::vector<PackTabRect> g_pack_tab_rects;
float g_picker_origin_x = 0, g_picker_origin_y = 0;
LayoutRect g_picker_rect{};
LayoutRect g_picker_content_rect{};
LayoutRect g_emoji_button_rect{};
Tween g_picker_content_t;
int g_picker_content_tab = 0;

std::vector<AnnouncementItem> g_announcements;
std::mutex g_announcements_mtx;
std::wstring g_bootstrap_role;
bool g_bootstrap_is_admin = false;
AnnouncementItem g_popup_announcement;
bool g_popup_open = false;
Tween g_popup_t;
bool g_announcements_stream_dirty = true;

PickerScrollDrag g_picker_scroll_drag;
ComposerDrag g_composer_drag;
std::vector<float> g_composer_caret_xs;

std::unordered_map<std::wstring, HistoryLoadState> g_history_state;

std::mutex g_pending_hist_mtx;
std::unordered_map<std::wstring, HistoryResult> g_pending_history;

// ---- 共享小辅助 ----
float clampf(float v, float lo, float hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

void setPickerOpen(bool open) {
    g_picker_open = open;
    if (open) {
        g_picker_t.start(g_picker_t.value(), 1.0f, 0.18f, 0, curve::easeOutCubic);
    } else {
        g_picker_t.start(g_picker_t.value(), 0.0f, 0.12f, 0, curve::easeOutCubic);
        g_picker_scroll_drag = PickerScrollDrag{};
        g_pack_drag = PackDrag{};
        g_react_target.active = false;   // 关闭 picker 即退出 react 选择模式
    }
}

// 分组折叠动画进度：0=完全折叠，1=完全展开。
// 关键：未被点击过的分组在 g_group_anim 里没有条目（started==false），
// 其值直接由 bool 派生 —— 默认 bool=false → 值=1（完全展开），
// 保证从未交互的应用（含 visual-smoke 屏 05）逐像素等同于今天的展开布局。
float groupAnimValue(const std::wstring& name) {
    auto it = g_group_anim.find(name);
    if (it != g_group_anim.end() && it->second.started) return it->second.value();
    return g_group_collapsed[name] ? 0.0f : 1.0f;   // 静止态：由 bool 派生
}

// 切换分组折叠：翻转 bool 目标态，并从当前可见进度起 tween 到目标（0.22s easeOutCubic）。
// from=cur 让动画途中再次点击能从当前进度平滑续接，不跳变。
void toggleGroupCollapsed(const std::wstring& name) {
    bool now_collapsed = !g_group_collapsed[name];
    g_group_collapsed[name] = now_collapsed;
    float cur = groupAnimValue(name);
    g_group_anim[name].start(cur, now_collapsed ? 0.0f : 1.0f,
                             0.22f, 0.0f, curve::easeOutCubic);
}

std::vector<Msg>& streamFor(const std::wstring& slug) {
    auto it = g_streams.find(slug);
    if (it == g_streams.end()) {
        it = g_streams.emplace(slug, std::vector<Msg>{}).first;
    }
    return it->second;
}

} // namespace launcher::d2d::chat
