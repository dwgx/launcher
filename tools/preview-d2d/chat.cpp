// Chat view 实现 — 见 chat.h。GDI+ 等价 tools/preview/chat_view.inl。

#include "chat.h"
#include "icons.h"
#include "palette.h"
#include "user_state.h"
#include "net.h"
#include "fetch.h"
#include "hit.h"
#include "stages.h"
#include "sticker.h"
#include "modals.h"
#include "render/primitives.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <limits>
#include <memory>
#include <mutex>
#include <objbase.h>
#include <unordered_map>
#include <unordered_set>

#pragma comment(lib, "ole32.lib")

namespace launcher::d2d::chat {

// fadeArgb 复用 — 把 ARGB 的 alpha 分量乘 op
namespace {
void normalizeMsgIdentity(Msg& m);
bool fillReplySnapshot(Msg& m);
bool sameMessageIdentityStrict(const Msg& a, const Msg& b);
void mergeServerIdentity(Msg& local, const Msg& server);
void rememberReplySnapshot(const std::wstring& slug, const Msg& m);
int64_t oldestServerId(const std::wstring& slug);
bool hasMessageServerId(const std::wstring& slug, int64_t server_id);
bool isFocusWaitingForSlug(const std::wstring& slug);
std::wstring displayAuthorFor(const Msg& m, HWND notify = nullptr);

inline uint32_t fadeArgb(uint32_t argb, float op) {
    uint32_t a = (argb >> 24) & 0xFFu;
    a = (uint32_t)(a * op + 0.5f);
    if (a > 255) a = 255;
    return (a << 24) | (argb & 0xFFFFFFu);
}
}

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
const wchar_t* kGroups[] = { L"IMPORTANT", L"GENERAL", L"GAMES", L"SHOP" };

std::wstring g_active = L"general";
InputBox     g_composer;
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

static std::unordered_map<std::wstring, std::vector<Msg>> g_streams;
static std::unordered_map<std::wstring, bool> g_group_collapsed;
static std::mutex g_streams_mtx;

struct ReplySnapshot {
    std::wstring slug;
    std::wstring author;
    std::wstring preview;
};
static std::unordered_map<int64_t, ReplySnapshot> g_reply_snapshots;
static std::mutex g_reply_snapshots_mtx;
static std::unordered_map<std::wstring, float> g_peer_profile_requested_at;
static std::mutex g_peer_profile_requested_mtx;

static std::wstring localTimeText(time_t tt = time(nullptr));

// 头像 hit 表 — paintChatPane 帧首清空，paintBubble 填充，WM_RBUTTONDOWN 命中
struct AvatarHit { LayoutRect rect; std::wstring peer_key; };
static std::vector<AvatarHit> g_avatar_hits;
// emoji 滚动偏移（picker 内部）
static float g_emoji_scroll_y = 0.0f;
static float g_emoji_grid_h_last = 0.0f;
static float g_emoji_total_h_last = 0.0f;
static std::vector<float> g_emoji_hover_t;
static float g_pack_scroll_y = 0.0f;
static float g_pack_grid_h_last = 0.0f;
static float g_pack_total_h_last = 0.0f;
// 消息体 hit 表 — paintChatPane 帧首清空，paintBubble 填充
struct MsgHit { LayoutRect rect; int idx; };
static std::vector<MsgHit> g_msg_hits;
static std::vector<MsgHit> g_msg_row_hits;
static LayoutRect g_chat_stream_rect{};
struct FocusTarget {
    std::wstring slug;
    int64_t server_id = 0;
    float highlight_until = 0.0f;
    bool scroll_applied = false;
    bool missing_reported = false;
};
static FocusTarget g_focus_target;
// 当前 picker 内 pack tab 实际像素位置（paintPicker 写，鼠标命中读用以拖拽）
struct PackTabRect { LayoutRect r; int idx; };
static std::vector<PackTabRect> g_pack_tab_rects;
// 鼠标在 picker 上的本地相对坐标（pack tabs 子区域）
static float g_picker_origin_x = 0, g_picker_origin_y = 0;
static LayoutRect g_picker_rect{};
static LayoutRect g_picker_content_rect{};
static Tween g_picker_content_t;
static int g_picker_content_tab = 0;
struct PickerScrollDrag {
    bool active = false;
    int mode = 0; // 0 = emoji grid, 1 = sticker grid
    float anchor_mouse_y = 0.0f;
    float anchor_scroll_y = 0.0f;
    float track_h = 0.0f;
    float thumb_h = 0.0f;
    float max_scroll = 0.0f;
};
static PickerScrollDrag g_picker_scroll_drag;
struct ComposerDrag {
    bool active = false;
    int anchor_cursor = 0;
};
static ComposerDrag g_composer_drag;
static std::vector<float> g_composer_caret_xs;
constexpr float kStreamTopPad = 12.0f;
constexpr float kStreamBottomPad = 18.0f;

static float clampf(float v, float lo, float hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static void setPickerOpen(bool open) {
    g_picker_open = open;
    if (open) {
        g_picker_t.start(g_picker_t.value(), 1.0f, 0.18f, 0, curve::easeOutCubic);
    } else {
        g_picker_t.start(g_picker_t.value(), 0.0f, 0.12f, 0, curve::easeOutCubic);
        g_picker_scroll_drag = PickerScrollDrag{};
        g_pack_drag = PackDrag{};
    }
}

std::vector<Msg>& streamFor(const std::wstring& slug) {
    auto it = g_streams.find(slug);
    if (it == g_streams.end()) {
        it = g_streams.emplace(slug, std::vector<Msg>{}).first;
    }
    return it->second;
}

enum class HistoryLoadState { NotStarted, Loading, Loaded, Exhausted, Failed };
static std::unordered_map<std::wstring, HistoryLoadState> g_history_state;

void switchChannel(const std::wstring& slug) {
    g_active = slug;
    g_focus_composer = false;
    if (g_pending_reply.active && g_pending_reply.slug != slug) {
        g_pending_reply = PendingReply{};
        g_pending_mentions.clear();
    }
    std::string chat_id;
    for (auto& c : g_channels) {
        if (c.slug == slug) { chat_id = c.id; break; }
    }
    auto& hist_state = g_history_state[slug];
    if (!chat_id.empty()
        && (hist_state == HistoryLoadState::NotStarted
            || hist_state == HistoryLoadState::Failed)) {
        fetchHistory(GetActiveWindow(), slug);
    }
    // 切到这个频道时不重置 scroll — 保留之前的位置（用户切到设置再切回来还在原位）
    // 但首次进入会通过 ChatScroll::initialized = false 自动 stick to bottom
}

namespace {
void normalizeMsgIdentity(Msg& m);
bool fillReplySnapshot(Msg& m);
bool sameMessageIdentityStrict(const Msg& a, const Msg& b);
void mergeServerIdentity(Msg& local, const Msg& server);
void rememberReplySnapshot(const std::wstring& slug, const Msg& m);
bool hasMessageServerId(const std::wstring& slug, int64_t server_id);
}

void onWheel(int delta) {
    if (g_picker_open && g_picker_rect.contains(g_mouse)) {
        if (g_picker_tab == 0) {
            float max_scroll = (std::max)(0.0f, g_emoji_total_h_last - g_emoji_grid_h_last);
            g_emoji_scroll_y = clampf(g_emoji_scroll_y - (float)delta * 0.5f, 0.0f, max_scroll);
        } else {
            float max_scroll = (std::max)(0.0f, g_pack_total_h_last - g_pack_grid_h_last);
            g_pack_scroll_y = clampf(g_pack_scroll_y - (float)delta * 0.5f, 0.0f, max_scroll);
        }
        return;
    }
    auto& sc = g_scroll[g_active];
    if (sc.total_height <= sc.viewport_h) return;
    // 写 target_offset，每帧 lerp 平滑到位（避免一格 60px 跳跃感）
    // 一次 wheel notch (delta=120) → 滚 90px，连续滚自动累积
    float dy = (float)delta * 0.75f;
    sc.target_offset += dy;
    float max_off = sc.total_height - sc.viewport_h;
    if (sc.target_offset > max_off) sc.target_offset = max_off;
    if (sc.target_offset < 0) sc.target_offset = 0;
}

void appendLocalMessage(Msg msg) {
    appendOrMergeMessage(g_active, std::move(msg));
    auto& sc = g_scroll[g_active];
    sc.offset_from_bottom = 0;
    sc.target_offset = 0;
}

void retargetTopSeg() {
    // 表情 / 表情包 顶部 seg pill — 0/1
    int idx = (g_picker_tab == 0) ? 0 : 1;
    float target_x = idx * (60 + 6);     // 第二个 tab 起点 (60 宽 + 6 间隔)
    float target_w = (idx == 0) ? 60.0f : 80.0f;     // 表情 60 / 表情包 80
    if (!g_top_seg_x.started) g_top_seg_x.start(target_x, target_x, 0.001f, 0, curve::easeOutQuint);
    else if (std::abs(g_top_seg_x.to - target_x) > 0.5f)
        g_top_seg_x.start(g_top_seg_x.value(), target_x, 0.10f, 0, curve::easeOutCubic);
    if (!g_top_seg_w.started) g_top_seg_w.start(target_w, target_w, 0.001f, 0, curve::easeOutQuint);
    else if (std::abs(g_top_seg_w.to - target_w) > 0.5f)
        g_top_seg_w.start(g_top_seg_w.value(), target_w, 0.10f, 0, curve::easeOutCubic);
}

void retargetPackTab() {
    int active = g_picker_tab - 1;     // pack idx (0-based 在 g_packs 里)
    if (active < 0) return;
    if (active >= (int)g_pack_tab_rects.size()) return;
    LayoutRect tr;
    bool found = false;
    for (auto& pt : g_pack_tab_rects) {
        if (pt.idx == active) { tr = pt.r; found = true; break; }
    }
    if (!found) return;
    if (!g_pack_tab_x.started) g_pack_tab_x.start(tr.x, tr.x, 0.001f, 0, curve::easeOutQuint);
    else if (std::abs(g_pack_tab_x.to - tr.x) > 0.5f)
        g_pack_tab_x.start(g_pack_tab_x.value(), tr.x, 0.10f, 0, curve::easeOutCubic);
    if (!g_pack_tab_w.started) g_pack_tab_w.start(tr.w, tr.w, 0.001f, 0, curve::easeOutQuint);
    else if (std::abs(g_pack_tab_w.to - tr.w) > 0.5f)
        g_pack_tab_w.start(g_pack_tab_w.value(), tr.w, 0.10f, 0, curve::easeOutCubic);
}

static void setPickerTabSmooth(int tab) {
    if (tab < 0) tab = 0;
    if (g_picker_tab == tab) return;
    int old_tab = g_picker_tab;
    g_picker_tab = tab;
    g_picker_content_tab = tab;
    if (tab > 0 && tab != old_tab) g_pack_scroll_y = 0.0f;
    g_picker_scroll_drag = PickerScrollDrag{};
    g_pack_drag = PackDrag{};
    g_picker_content_t.start(0.0f, 1.0f, 0.16f, 0, curve::easeOutCubic);
    retargetTopSeg();
    if (g_picker_tab > 0) retargetPackTab();
}

namespace {
struct HistArg { std::wstring slug; std::string chat_id; int64_t before_id = 0; HWND h; };
struct HistoryResult {
    bool ok = false;
    int64_t before_id = 0;
    std::vector<Msg> msgs;
};
std::mutex g_pending_hist_mtx;
std::unordered_map<std::wstring, HistoryResult> g_pending_history;
static constexpr int kHistoryPageLimit = 100;

std::wstring utf8wHist(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(n - 1, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

std::wstring mediaLocalPathFromUrl(const std::string& media_url) {
    if (media_url.empty()) return {};
    auto dl = launcher::d2d::fetch::downloadMediaToCache(media_url, L"chat");
    if (dl.ok) return dl.path;
    return utf8wHist(media_url);
}

std::wstring bodyFromPayloadObject(const std::string& payload_obj) {
    std::string sub_url = net::jsonStr(payload_obj, "media_url");
    if (sub_url.empty()) sub_url = net::jsonStr(payload_obj, "url");
    if (!sub_url.empty()) return mediaLocalPathFromUrl(sub_url);
    std::string sticker_id = net::jsonStr(payload_obj, "sticker_id");
    if (!sticker_id.empty()) return utf8wHist(sticker_id);
    return {};
}

std::wstring bodyFromMessageObject(const std::string& msg_obj) {
    std::string raw = net::jsonRaw(msg_obj, "payload");
    if (raw.empty() || raw == "null") return {};
    if (raw.front() == '"') {
        std::string decoded;
        if (net::parseJsonStringAt(raw, 0, decoded)) return utf8wHist(decoded);
    } else if (raw.front() == '{') {
        return bodyFromPayloadObject(raw);
    }
    return utf8wHist(raw);
}

std::wstring selfAuthorKey() {
    if (!g_user_id.empty()) return utf8wHist(g_user_id);
    if (!g_user.uid.empty()) return g_user.uid;
    if (!g_user.username.empty()) return g_user.username;
    return g_user.nickname;
}

std::wstring msgAuthorDisplay(const Msg& m) {
    if (m.from == L"me") {
        if (!m.author.empty()) return m.author;
        if (!g_user.nickname.empty()) return g_user.nickname;
        if (!g_user.username.empty()) return g_user.username;
        return L"me";
    }
    std::wstring display = displayAuthorFor(m);
    if (!display.empty()) return display;
    if (!m.author.empty()) return m.author;
    if (!m.peer_key.empty()) return m.peer_key;
    if (!m.from.empty()) return m.from;
    return L"unknown";
}

bool looksLikeUuidW(const std::wstring& s) {
    if (s.size() != 36) return false;
    for (size_t i = 0; i < s.size(); ++i) {
        wchar_t c = s[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != L'-') return false;
            continue;
        }
        bool hex = (c >= L'0' && c <= L'9')
            || (c >= L'a' && c <= L'f')
            || (c >= L'A' && c <= L'F');
        if (!hex) return false;
    }
    return true;
}

std::wstring shortPeerLabel(const std::wstring& key) {
    if (key.empty()) return L"unknown";
    if (looksLikeUuidW(key) && key.size() > 8) return key.substr(0, 8);
    return key;
}

void requestPeerProfileOnce(const std::wstring& key, HWND notify) {
    if (key.empty() || key == L"me") return;
    float now = (float)(GetTickCount64() / 1000.0);
    {
        std::lock_guard<std::mutex> lk(g_peer_profile_requested_mtx);
        auto it = g_peer_profile_requested_at.find(key);
        if (it != g_peer_profile_requested_at.end() && now - it->second < 15.0f) return;
        g_peer_profile_requested_at[key] = now;
    }
    fetch::peerProfile(notify ? notify : GetActiveWindow(), key);
}

std::wstring displayAuthorFor(const Msg& m, HWND notify) {
    if (m.from == L"me") {
        if (!m.author.empty()) return m.author;
        if (!g_user.nickname.empty()) return g_user.nickname;
        if (!g_user.username.empty()) return g_user.username;
        return L"me";
    }
    std::wstring key = !m.author_key.empty() ? m.author_key
                     : (!m.peer_key.empty() ? m.peer_key : m.from);
    if (!key.empty()) {
        fetch::PeerProfile peer = fetch::peerProfileCached(key);
        if (peer.loaded && peer.err.empty()) {
            if (!peer.nickname.empty()) return peer.nickname;
            if (!peer.username.empty()) return peer.username;
            if (!peer.uid.empty()) return peer.uid;
        }
        if (!peer.loading && peer.err.empty()) requestPeerProfileOnce(key, notify);
    }
    if (!m.author.empty() && !looksLikeUuidW(m.author)) return m.author;
    if (!m.from.empty() && !looksLikeUuidW(m.from)) return m.from;
    return shortPeerLabel(key);
}

std::wstring msgPreviewText(const Msg& m) {
    if (!m.body.empty()) {
        std::wstring preview = m.body;
        for (auto& c : preview) {
            if (c == L'\r' || c == L'\n' || c == L'\t') c = L' ';
        }
        while (preview.find(L"  ") != std::wstring::npos) {
            preview.erase(preview.find(L"  "), 1);
        }
        if (preview.size() > 96) preview = preview.substr(0, 96) + L"...";
        return preview;
    }
    switch (m.kind) {
        case MsgKind::Image: return L"[image]";
        case MsgKind::Sticker: return L"[sticker]";
        case MsgKind::Gif: return L"[gif]";
        case MsgKind::Video: return L"[video]";
        case MsgKind::System: return L"[system]";
        case MsgKind::DayDivider: return L"[day]";
        default: return L"[message]";
    }
}

void rememberReplySnapshot(const std::wstring& slug, const Msg& m) {
    if (m.server_id <= 0) return;
    ReplySnapshot snap;
    snap.slug = slug;
    snap.author = msgAuthorDisplay(m);
    snap.preview = msgPreviewText(m);
    std::lock_guard<std::mutex> lk(g_reply_snapshots_mtx);
    g_reply_snapshots[m.server_id] = std::move(snap);
}

bool fillReplySnapshot(Msg& m) {
    if (m.reply_to_id <= 0) return false;
    if (!m.reply_author.empty() || !m.reply_preview.empty()) return true;
    std::lock_guard<std::mutex> lk(g_reply_snapshots_mtx);
    auto it = g_reply_snapshots.find(m.reply_to_id);
    if (it == g_reply_snapshots.end()) return false;
    m.reply_author = it->second.author;
    m.reply_preview = it->second.preview;
    return true;
}

void normalizeMsgIdentity(Msg& m) {
    if (m.author_key.empty()) {
        if (m.from == L"me") m.author_key = selfAuthorKey();
        else if (!m.peer_key.empty()) m.author_key = m.peer_key;
        else m.author_key = m.from;
    }
    if (m.from != L"me" && m.peer_key.empty()) m.peer_key = m.author_key;
}

bool sameMessageIdentity(const Msg& a, const Msg& b) {
    if (a.server_id > 0 && b.server_id > 0 && a.server_id == b.server_id) return true;
    return !a.client_msg_id.empty()
        && !b.client_msg_id.empty()
        && a.client_msg_id == b.client_msg_id;
}

bool sameMessageIdentityStrict(const Msg& a, const Msg& b) {
    if (a.server_id > 0 && b.server_id > 0) return a.server_id == b.server_id;
    if (!a.client_msg_id.empty() && !b.client_msg_id.empty()) {
        return a.client_msg_id == b.client_msg_id;
    }
    return false;
}

void mergeServerIdentity(Msg& local, const Msg& server) {
    if (local.server_id == 0 && server.server_id > 0) local.server_id = server.server_id;
    if (local.client_msg_id.empty()) local.client_msg_id = server.client_msg_id;
    if (local.author_key.empty()) local.author_key = server.author_key;
    if (local.peer_key.empty()) local.peer_key = server.peer_key;
    if (local.author.empty()) local.author = server.author;
    if (local.time.empty()) local.time = server.time;
    if (local.reply_to_id == 0) local.reply_to_id = server.reply_to_id;
    if (local.reply_author.empty()) local.reply_author = server.reply_author;
    if (local.reply_preview.empty()) local.reply_preview = server.reply_preview;
    local.send_state = MsgSendState::Sent;
    local.error_text.clear();
}

int64_t oldestServerId(const std::wstring& slug) {
    int64_t oldest = std::numeric_limits<int64_t>::max();
    bool found = false;
    for (auto& m : streamFor(slug)) {
        if (m.server_id > 0 && m.server_id < oldest) {
            oldest = m.server_id;
            found = true;
        }
    }
    return found ? oldest : 0;
}

bool hasMessageServerId(const std::wstring& slug, int64_t server_id) {
    if (server_id <= 0) return false;
    for (auto& m : streamFor(slug)) {
        if (m.server_id == server_id) return true;
    }
    return false;
}

int64_t serverIdForClientMsgId(const std::wstring& slug, const std::string& client_msg_id) {
    if (client_msg_id.empty()) return 0;
    for (auto& m : streamFor(slug)) {
        if (m.client_msg_id == client_msg_id && m.server_id > 0) return m.server_id;
    }
    return 0;
}

void resolveLocalReplyTargets(const std::wstring& slug) {
    auto& msgs = streamFor(slug);
    for (auto& m : msgs) {
        if (m.reply_to_id > 0 || m.reply_client_msg_id.empty()) continue;
        int64_t resolved = serverIdForClientMsgId(slug, m.reply_client_msg_id);
        if (resolved > 0) {
            m.reply_to_id = resolved;
            m.waiting_reply_target = false;
        }
    }
}

bool isFocusWaitingForSlug(const std::wstring& slug) {
    return g_focus_target.server_id > 0
        && g_focus_target.slug == slug
        && !g_focus_target.scroll_applied;
}
}

static std::string makeClientMsgId();
static void sendChatMessage(HWND hwnd, const std::wstring& body, const char* kind,
                            std::string client_msg_id = {}, int64_t reply_to_id = 0);

void addMentionToComposer(const std::wstring& user_id, const std::wstring& label) {
    if (user_id.empty()) return;
    for (auto& m : g_pending_mentions) {
        if (m.user_id == user_id) return;
    }
    std::wstring clean = label.empty() ? user_id : label;
    for (auto& c : clean) {
        if (c == L'\r' || c == L'\n' || c == L'\t') c = L' ';
    }
    while (!clean.empty() && clean.front() == L' ') clean.erase(clean.begin());
    while (!clean.empty() && clean.back() == L' ') clean.pop_back();
    g_pending_mentions.push_back({ user_id, clean });
    std::wstring token = L"@" + clean;
    if (!g_composer.text.empty()) {
        wchar_t prev = g_composer.text.back();
        if (prev != L' ' && prev != L'\n' && prev != L'\t') token = L" " + token;
    }
    token += L" ";
    g_composer.replaceSelection(token);
    g_focus_composer = true;
}

void beginReplyToMessage(const std::wstring& slug, int64_t server_id,
                         const std::string& client_msg_id,
                         const std::wstring& author,
                         const std::wstring& author_key,
                         const std::wstring& preview) {
    g_pending_reply = PendingReply{};
    g_pending_reply.active = true;
    g_pending_reply.id = server_id;
    g_pending_reply.client_msg_id = client_msg_id;
    g_pending_reply.slug = slug.empty() ? g_active : slug;
    g_pending_reply.author = author.empty() ? L"message" : author;
    g_pending_reply.author_key = author_key;
    g_pending_reply.preview = preview.empty() ? L"[message]" : preview;
    g_focus_composer = true;
}

bool appendOrMergeMessage(const std::wstring& slug, Msg msg) {
    normalizeMsgIdentity(msg);
    fillReplySnapshot(msg);
    auto& s = streamFor(slug);
    auto& sc = g_scroll[slug];
    bool was_at_bottom = (sc.offset_from_bottom < 16.0f && sc.target_offset < 16.0f);
    for (auto& existing : s) {
        if (!sameMessageIdentityStrict(existing, msg)) continue;
        mergeServerIdentity(existing, msg);
        fillReplySnapshot(existing);
        rememberReplySnapshot(slug, existing);
        resolveLocalReplyTargets(slug);
        return false;
    }
    s.push_back(std::move(msg));
    rememberReplySnapshot(slug, s.back());
    resolveLocalReplyTargets(slug);
    if (was_at_bottom) {
        sc.offset_from_bottom = 0;
        sc.target_offset = 0;
    }
    return true;
}

void focusMessage(const std::wstring& slug, int64_t server_id) {
    if (server_id <= 0) return;
    if (!slug.empty()) switchChannel(slug);
    g_focus_target.slug = slug.empty() ? g_active : slug;
    g_focus_target.server_id = server_id;
    g_focus_target.highlight_until = stages::g_time_in_stage + 2.2f;
    g_focus_target.scroll_applied = false;
    g_focus_target.missing_reported = false;
    if (!hasMessageServerId(g_focus_target.slug, server_id)) {
        auto state = g_history_state[g_focus_target.slug];
        if (state == HistoryLoadState::Loaded || state == HistoryLoadState::Failed) {
            fetchHistory(GetActiveWindow(), g_focus_target.slug);
        } else if (state == HistoryLoadState::Exhausted) {
            g_focus_target.missing_reported = true;
        }
    }
}

void fetchHistory(HWND notify, const std::wstring& slug) {
    if (g_session_token.empty()) {
        g_history_state[slug] = HistoryLoadState::Failed;
        return;
    }
    std::string chat_id;
    for (auto& c : g_channels) if (c.slug == slug) { chat_id = c.id; break; }
    if (chat_id.empty()) {
        g_history_state[slug] = HistoryLoadState::NotStarted;
        return;
    }
    auto& hist_state = g_history_state[slug];
    if (hist_state == HistoryLoadState::Loading || hist_state == HistoryLoadState::Exhausted) {
        return;
    }
    int64_t before_id = oldestServerId(slug);
    hist_state = HistoryLoadState::Loading;

    auto* a = new HistArg{ slug, chat_id, before_id, notify };
    CreateThread(nullptr, 0, [](LPVOID lp) -> DWORD {
        std::unique_ptr<HistArg> a((HistArg*)lp);
        std::string url = "/api/chat/history?session_token=" + g_session_token
                        + "&chat_id=" + a->chat_id
                        + "&limit=" + std::to_string(kHistoryPageLimit);
        if (a->before_id > 0) {
            url += "&before_id=" + std::to_string(a->before_id);
        }
        std::wstring wurl(url.begin(), url.end());
        auto r = net::request(L"GET", wurl.c_str(), {}, L"");
        if (!r.ok()) {
            {
                std::lock_guard<std::mutex> lk(g_pending_hist_mtx);
                HistoryResult hr;
                hr.ok = false;
                hr.before_id = a->before_id;
                g_pending_history[a->slug] = std::move(hr);
            }
            auto* slug_p = new std::wstring(a->slug);
            PostMessageW(a->h, WM_APP + 45, 0, (LPARAM)slug_p);
            return 0;
        }
        // 后端 history 真实字段（chat.rs MessageOut）：
        //   id (i64) / sender_id (Option<String>) / msg_type / payload (JSON Value)
        //   / created_at (i64) / deleted (bool)
        std::vector<Msg> msgs;
        size_t pos = 0;
        while (true) {
            auto ob = r.body.find('{', pos);
            if (ob == std::string::npos) break;
            // 找匹配的 }（payload 可能是 nested object）— 简单 brace 计数
            int depth = 1;
            size_t scan = ob + 1;
            while (scan < r.body.size() && depth > 0) {
                char c = r.body[scan];
                if (c == '"') {
                    // 跳过字符串
                    ++scan;
                    while (scan < r.body.size() && r.body[scan] != '"') {
                        if (r.body[scan] == '\\' && scan + 1 < r.body.size()) ++scan;
                        ++scan;
                    }
                } else if (c == '{') depth++;
                else if (c == '}') depth--;
                if (depth == 0) break;
                ++scan;
            }
            if (depth != 0) break;
            size_t cb = scan;
            std::string obj = r.body.substr(ob, cb - ob + 1);
            // deleted 软删除消息跳过
            {
                auto pd = obj.find("\"deleted\":");
                if (pd != std::string::npos
                    && obj.compare(pd + 10, 4, "true") == 0) {
                    pos = cb + 1;
                    continue;
                }
            }
            Msg m;
            std::string kind = net::jsonStr(obj, "msg_type");
            if (kind == "sticker") m.kind = MsgKind::Sticker;
            else if (kind == "image") m.kind = MsgKind::Image;
            else if (kind == "gif") m.kind = MsgKind::Gif;
            else if (kind == "video") m.kind = MsgKind::Video;
            else if (kind == "system") m.kind = MsgKind::System;
            else m.kind = MsgKind::Text;
            m.server_id = net::jsonInt(obj, "id");
            m.client_msg_id = net::jsonStr(obj, "client_msg_id");
            // sender_id 是 UUID — 跟当前 user_id 比较决定 me / 别人
            std::string sender = net::jsonStr(obj, "sender_id");
            if (!g_user_id.empty() && sender == g_user_id) {
                m.from = L"me";
                m.author_key = utf8wHist(g_user_id);
                m.author = g_user.nickname;
            } else {
                m.peer_key = utf8wHist(sender);
                m.author_key = m.peer_key;
                m.from = shortPeerLabel(m.peer_key);
                m.author = utf8wHist(net::jsonStr(obj, "sender_nickname"));
                if (m.author.empty()) m.author = utf8wHist(net::jsonStr(obj, "sender_username"));
                if (m.author.empty()) m.author = utf8wHist(net::jsonStr(obj, "sender_uid"));
            }
            // payload 可能是 string 字面量 "abc" 或 JSON object {...}（image/sticker 含 url 等）
            // 简单做法：找 "payload":" 后第一个 unescaped " 之间的内容
            m.reply_to_id = net::jsonInt(obj, "reply_to_id");
            std::string reply = net::jsonObject(obj, "reply_snapshot");
            if (!reply.empty()) {
                m.reply_author = utf8wHist(net::jsonStr(reply, "sender_nickname"));
                if (m.reply_author.empty()) m.reply_author = utf8wHist(net::jsonStr(reply, "sender_username"));
                if (m.reply_author.empty()) m.reply_author = utf8wHist(net::jsonStr(reply, "sender_uid"));
                m.reply_preview = utf8wHist(net::jsonStr(reply, "preview"));
            }
            m.body = bodyFromMessageObject(obj);
            // created_at i64 → HH:MM 格式
            int64_t ts = net::jsonInt(obj, "created_at");
            if (ts > 0) {
                time_t tt = (time_t)ts;
                struct tm lt{};
                localtime_s(&lt, &tt);
                wchar_t tbuf[16];
                swprintf_s(tbuf, L"%02d:%02d", lt.tm_hour, lt.tm_min);
                m.time = tbuf;
            } else {
                m.time = L"";
            }
            m.status = L"online";
            normalizeMsgIdentity(m);
            fillReplySnapshot(m);
            msgs.push_back(std::move(m));
            pos = cb + 1;
        }
        std::reverse(msgs.begin(), msgs.end());
        {
            std::lock_guard<std::mutex> lk(g_pending_hist_mtx);
            HistoryResult hr;
            hr.ok = true;
            hr.before_id = a->before_id;
            hr.msgs = std::move(msgs);
            g_pending_history[a->slug] = std::move(hr);
        }
        // PostMessage 让主线程把 pending 替换到 streamFor
        auto* slug_p = new std::wstring(a->slug);
        PostMessageW(a->h, WM_APP + 45, 0, (LPARAM)slug_p);
        return 0;
    }, a, 0, nullptr);
}

// 主线程调（WM_APP+45）— merge 历史到 streamFor
void applyHistoryResult(const std::wstring& slug) {
    HistoryResult result;
    {
        std::lock_guard<std::mutex> lk(g_pending_hist_mtx);
        auto it = g_pending_history.find(slug);
        if (it == g_pending_history.end()) return;
        result = std::move(it->second);
        g_pending_history.erase(it);
    }
    if (!result.ok) {
        g_history_state[slug] = HistoryLoadState::Failed;
        return;
    }
    bool exhausted = result.msgs.empty() || result.msgs.size() < (size_t)kHistoryPageLimit;
    g_history_state[slug] = exhausted ? HistoryLoadState::Exhausted : HistoryLoadState::Loaded;
    auto& msgs = result.msgs;
    auto& s = streamFor(slug);
    std::unordered_set<int64_t> seen_server_ids;
    std::unordered_set<std::string> seen_client_ids;
    for (auto& existing : s) {
        normalizeMsgIdentity(existing);
        if (existing.server_id > 0) seen_server_ids.insert(existing.server_id);
        if (!existing.client_msg_id.empty()) seen_client_ids.insert(existing.client_msg_id);
    }
    auto exists = [&](Msg& candidate) -> bool {
        normalizeMsgIdentity(candidate);
        bool duplicate = false;
        if (candidate.server_id > 0 && seen_server_ids.count(candidate.server_id)) duplicate = true;
        if (!candidate.client_msg_id.empty() && seen_client_ids.count(candidate.client_msg_id)) duplicate = true;
        if (!duplicate) {
            if (candidate.server_id > 0) seen_server_ids.insert(candidate.server_id);
            if (!candidate.client_msg_id.empty()) seen_client_ids.insert(candidate.client_msg_id);
            return false;
        }
        for (auto& existing : s) {
            if (sameMessageIdentity(existing, candidate)) {
                mergeServerIdentity(existing, candidate);
                break;
            }
        }
        return true;
    };
    // 历史消息插到流的开头（之前实时收到的"me"放在后面）
    if (s.empty()) {
        for (auto& m : msgs) normalizeMsgIdentity(m);
        s = std::move(msgs);
    } else {
        // 简单 merge：历史在前，本地实时在后
        std::vector<Msg> merged;
        merged.reserve(msgs.size() + s.size());
        for (auto& m : msgs) {
            if (!exists(m)) merged.push_back(std::move(m));
        }
        for (auto& m : s) merged.push_back(std::move(m));
        s = std::move(merged);
    }
    for (auto& m : s) {
        normalizeMsgIdentity(m);
        fillReplySnapshot(m);
        rememberReplySnapshot(slug, m);
    }
    if (isFocusWaitingForSlug(slug) && !hasMessageServerId(slug, g_focus_target.server_id)) {
        if (!exhausted) {
            fetchHistory(GetActiveWindow(), slug);
        } else {
            g_focus_target.missing_reported = true;
        }
    }
}

static Channel* activeChannel() {
    for (auto& c : g_channels) if (c.slug == g_active) return &c;
    return &g_channels[2];   // general
}

std::string activeChatId() {
    if (g_channels.empty()) return {};
    if (auto* ch = activeChannel()) return ch->id;
    return {};
}

static float measureW(D2DApp& app, std::wstring_view s, IDWriteTextFormat* fmt) {
    if (s.empty() || !fmt) return 0.0f;
    DWRITE_TEXT_METRICS m{};
    if (!app.texts().measure(fmt, s, 8192.0f, 256.0f, &m)) return 0.0f;
    return m.width;
}

static float spaceW(D2DApp& app, IDWriteTextFormat* fmt) {
    float w = measureW(app, L"x x", fmt) - measureW(app, L"xx", fmt);
    return w > 0.5f ? w : 4.0f;
}

static float caretMeasureW(D2DApp& app, std::wstring_view s, IDWriteTextFormat* fmt) {
    if (s.empty() || !fmt) return 0.0f;
    size_t end = s.size();
    while (end > 0 && s[end - 1] == L' ') --end;
    float w = measureW(app, s.substr(0, end), fmt);
    if (end < s.size()) w += (float)(s.size() - end) * spaceW(app, fmt);
    return w;
}

static int cursorFromComposerPoint(float x) {
    if (g_composer_caret_xs.empty()) return 0;
    int best = 0;
    float best_dist = std::numeric_limits<float>::max();
    for (int i = 0; i < (int)g_composer_caret_xs.size(); ++i) {
        float d = std::abs(g_composer_caret_xs[i] - x);
        if (d < best_dist) {
            best_dist = d;
            best = i;
        }
    }
    return best;
}

struct WrappedText {
    std::wstring text;
    DWRITE_TEXT_METRICS metrics{};
    int line_count = 0;
    float text_h = 0.0f;
    float max_line_w = 0.0f;
};

static float dwriteMaxLineWidth(D2DApp& app, const std::wstring& text,
                                IDWriteTextFormat* fmt, float max_w, float max_h) {
    if (!fmt || text.empty()) return 0.0f;
    auto layout = app.texts().layout(fmt, text, max_w, max_h);
    if (!layout) return 0.0f;
    UINT32 hit_count = 0;
    HRESULT hr = layout->HitTestTextRange(0, (UINT32)text.size(), 0, 0,
                                          nullptr, 0, &hit_count);
    if (hr != E_NOT_SUFFICIENT_BUFFER || hit_count == 0) return 0.0f;
    std::vector<DWRITE_HIT_TEST_METRICS> hits(hit_count);
    if (FAILED(layout->HitTestTextRange(0, (UINT32)text.size(), 0, 0,
                                        hits.data(), hit_count, &hit_count))) {
        return 0.0f;
    }
    float widest = 0.0f;
    for (UINT32 i = 0; i < hit_count; ++i) {
        widest = (std::max)(widest, hits[i].left + hits[i].width);
    }
    return widest;
}

static WrappedText wrapTextForWidth(D2DApp& app, std::wstring_view src,
                                    IDWriteTextFormat* fmt, float max_w) {
    WrappedText out;
    if (!fmt || src.empty() || max_w <= 1.0f) return out;
    float line_w = 0.0f;
    size_t i = 0;
    while (i < src.size()) {
        wchar_t c = src[i];
        if (c == L'\r') { ++i; continue; }
        if (c == L'\n') {
            out.text.push_back(c);
            line_w = 0.0f;
            ++i;
            continue;
        }
        if (c == L' ' || c == L'\t') {
            float cw = spaceW(app, fmt);
            if (line_w <= 0.5f || line_w + cw > max_w) {
                if (line_w > 0.5f) {
                    out.text.push_back(L'\n');
                    line_w = 0.0f;
                }
                ++i;
                continue;
            }
            out.text.push_back(L' ');
            line_w += cw;
            ++i;
            continue;
        }

        size_t start = i;
        while (i < src.size()
               && src[i] != L'\r'
               && src[i] != L'\n'
               && src[i] != L' '
               && src[i] != L'\t') {
            ++i;
        }
        std::wstring_view run = src.substr(start, i - start);
        float run_w = measureW(app, run, fmt);
        if (run_w <= max_w) {
            if (line_w > 0.5f && line_w + run_w > max_w) {
                out.text.push_back(L'\n');
                line_w = 0.0f;
            }
            out.text.append(run.data(), run.size());
            line_w += run_w;
            continue;
        }

        if (line_w > 0.5f) {
            out.text.push_back(L'\n');
            line_w = 0.0f;
        }
        for (size_t j = 0; j < run.size(); ++j) {
            wchar_t rc = run[j];
            float cw = measureW(app, std::wstring_view(&rc, 1), fmt);
            if (line_w > 0.5f && line_w + cw > max_w) {
                out.text.push_back(L'\n');
                line_w = 0.0f;
            }
            out.text.push_back(rc);
            line_w += cw;
        }
    }
    app.texts().measure(fmt, out.text, max_w, 8192.0f, &out.metrics);
    out.line_count = out.text.empty() ? 0 : 1;
    for (wchar_t c : out.text) {
        if (c == L'\n') ++out.line_count;
    }
    out.text_h = (std::max)(out.metrics.height, out.line_count * 18.0f);
    float dwrite_w = dwriteMaxLineWidth(app, out.text, fmt, max_w, 8192.0f);
    size_t line_start = 0;
    while (line_start <= out.text.size()) {
        size_t line_end = out.text.find(L'\n', line_start);
        if (line_end == std::wstring::npos) line_end = out.text.size();
        std::wstring_view line(out.text.data() + line_start, line_end - line_start);
        out.max_line_w = (std::max)(out.max_line_w, caretMeasureW(app, line, fmt));
        if (line_end >= out.text.size()) break;
        line_start = line_end + 1;
    }
    out.max_line_w = (std::max)(out.max_line_w, dwrite_w);
    return out;
}

static std::wstring authorKeyFor(const Msg& m) {
    if (!m.author_key.empty()) return m.author_key;
    if (!m.peer_key.empty()) return m.peer_key;
    if (m.from == L"me") return L"me";
    return m.from;
}

static bool sameGroupedAuthor(const Msg& prev, const Msg& cur) {
    if (prev.kind != MsgKind::Text || cur.kind != MsgKind::Text) return false;
    if (prev.from == L"me" || cur.from == L"me") return false;
    return authorKeyFor(prev) == authorKeyFor(cur);
}

static bool messageHasReplyPreview(const Msg& m) {
    return m.reply_to_id > 0 || !m.reply_client_msg_id.empty()
        || !m.reply_author.empty() || !m.reply_preview.empty();
}

static std::wstring replyPreviewLine(const Msg& m) {
    std::wstring author = m.reply_author.empty() ? L"message" : m.reply_author;
    std::wstring preview = m.reply_preview.empty() ? L"[unavailable]" : m.reply_preview;
    std::wstring line = author + L": " + preview;
    if (line.size() > 96) line = line.substr(0, 96) + L"...";
    return line;
}

static float replyPreviewHeight(const Msg& m) {
    return messageHasReplyPreview(m) ? 28.0f : 0.0f;
}

void tick(float dt) {
    g_picker_t.tick(dt);
    g_picker_content_t.tick(dt);
    g_top_seg_x.tick(dt); g_top_seg_w.tick(dt);
    g_pack_tab_x.tick(dt); g_pack_tab_w.tick(dt);
}

void appendMedia(const std::wstring& path) {
    Msg m;
    std::wstring p = path;
    auto dot = p.find_last_of(L'.');
    std::wstring ext = (dot != std::wstring::npos) ? p.substr(dot) : L"";
    for (auto& c : ext) c = (wchar_t)towlower(c);
    const char* kind = "text";
    if (ext == L".png" || ext == L".jpg" || ext == L".jpeg" || ext == L".webp" || ext == L".bmp") {
        m.kind = MsgKind::Image;
        kind = "image";
    } else if (ext == L".gif") {
        m.kind = MsgKind::Gif;
        kind = "gif";
    } else if (ext == L".mp4" || ext == L".webm" || ext == L".mov" || ext == L".avi" || ext == L".mkv") {
        m.kind = MsgKind::Video;
        kind = "video";
    } else {
        m.kind = MsgKind::Text;
        m.body = L"[文件] " + path;
        m.from = L"me"; m.author = g_user.nickname; m.author_key = selfAuthorKey();
        m.status = L"online"; m.time = localTimeText();
        m.client_msg_id = makeClientMsgId();
        appendLocalMessage(std::move(m));
        return;
    }
    m.from = L"me";
    m.author = g_user.nickname;
    m.author_key = selfAuthorKey();
    m.status = L"online";
    m.body = path;
    m.time = localTimeText();
    m.client_msg_id = makeClientMsgId();
    m.send_state = MsgSendState::Pending;
    std::string client_msg_id = m.client_msg_id;
    appendLocalMessage(std::move(m));
    sendChatMessage(GetActiveWindow(), path, kind, client_msg_id);
}

// ============== 频道列表 ==============
static void paintChatList(D2DApp& app, float ax, float ay, float aw, float ah) {
    const Palette& pal = palette();
    auto* ctx = app.ctx();
    auto& br = app.brushes();

    prim::fillRect(ctx, ax, ay, aw, ah, br.solid(pal.bg));
    prim::drawLine(ctx, ax + aw, ay, ax + aw, ay + ah,
                   br.solid(pal.divider), 1.0f);

    auto* hdr_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(11.0f),
                                       DWRITE_FONT_WEIGHT_BOLD);
    auto* grp_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(8.0f),
                                       DWRITE_FONT_WEIGHT_BOLD);
    auto* ch_fmt  = app.texts().format(L"Microsoft YaHei UI", ptToDip(9.5f));
    auto* ch_active = app.texts().format(L"Microsoft YaHei UI", ptToDip(9.5f),
                                         DWRITE_FONT_WEIGHT_BOLD);

    // header
    float hy = ay + 12;
    prim::fillRR(ctx, ax + 14, hy - 2, 26, 26, 7.0f, br.solid(pal.primary));
    icons::drawIcon(app, icons::Name::Logo, ax + 14 + 5, hy - 2 + 5, 16, 0xFFFFFFFF);
    prim::drawText_(ctx, L"Launcher Server", hdr_fmt,
                    ax + 50, hy + 2, 200, 22, br.solid(pal.text));
    prim::drawLine(ctx, ax + 8, ay + 44, ax + aw - 8, ay + 44,
                   br.solid(pal.divider), 1.0f);

    float row_y = ay + 50;
    for (auto* gname : kGroups) {
        LayoutRect ghead{ ax + 6, row_y, aw - 12, 22 };
        bool ghov = ghead.contains(g_mouse);
        if (ghov) {
            prim::fillRR(ctx, ghead.x, ghead.y, ghead.w, ghead.h, 4.0f,
                         br.solidA(pal.text, 0.05f));
        }
        bool collapsed = g_group_collapsed[gname];
        prim::drawText_(ctx, collapsed ? L"▸" : L"▾", grp_fmt,
                        ax + 10, row_y + 4, 12, 14,
                        br.solid(pal.text_muted));
        prim::drawText_(ctx, gname, grp_fmt,
                        ax + 26, row_y + 4, 200, 14,
                        br.solid(pal.text_muted));
        const wchar_t* gn = gname;
        hit(ghead, [gn]() { g_group_collapsed[gn] = !g_group_collapsed[gn]; }, true);
        row_y += 24;

        if (collapsed) { row_y += 6; continue; }

        for (auto& c : g_channels) {
            if (wcscmp(c.group, gname) != 0) continue;
            bool active = (c.slug == g_active);
            LayoutRect cr{ ax + 6, row_y, aw - 12, 28 };
            bool hov = cr.contains(g_mouse);
            if (active) {
                prim::fillRR(ctx, cr.x, cr.y, cr.w, cr.h, 6.0f,
                             br.solidA(pal.primary, 0.14f));
            } else if (hov) {
                prim::fillRR(ctx, cr.x, cr.y, cr.w, cr.h, 6.0f,
                             br.solidA(pal.text, 0.04f));
            }
            uint32_t tc = active ? pal.primary : pal.text_muted;
            prim::drawText_(ctx, L"#", grp_fmt,
                            cr.x + 12, cr.y + 6, 14, 16,
                            br.solid(tc));
            prim::drawText_(ctx, c.name, active ? ch_active : ch_fmt,
                            cr.x + 26, cr.y + 7, cr.w - 60, 18,
                            br.solid(active ? pal.text : pal.text_muted));
            std::wstring tgt = c.slug;
            hit(cr, [tgt]() { switchChannel(tgt); }, true);
            row_y += 30;
        }
        row_y += 6;
    }
}

// 纯量高度 — 跟 paintBubble 完全镜像但不画任何 D2D / 不 push hit。
// 用来在真画之前一次过算 total，给 scroll offset 定位。
static float measureBubbleHeight(D2DApp& app, const Msg& m, float maxw, bool prev_same_author) {
    auto* body_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(9.5f));
    // 空 body 直接占 0 高度（与 paintBubble 行为一致）
    if (m.kind == MsgKind::Text && m.body.empty()) return 0;
    if (m.kind == MsgKind::DayDivider) return 30;
    if (m.kind == MsgKind::System)     return 32;
    if (m.kind == MsgKind::Image || m.kind == MsgKind::Gif) {
        float bub_w = 240, bub_h = 180;
        D2D1_SIZE_F sz{ 0, 0 };
        ID2D1Bitmap* bmp = nullptr;
        if (m.kind == MsgKind::Gif) {
            auto* a = app.gifs().fromFile(m.body);
            if (a) { sz.width = (float)a->width; sz.height = (float)a->height; }
        }
        if (sz.width <= 0) {
            bmp = app.images().fromFile(m.body);
            if (bmp) sz = bmp->GetSize();
        }
        if (sz.width > 0 && sz.height > 0) {
            float aspect = sz.height / sz.width;
            float max_w = (std::min)(maxw * 0.55f, 320.0f);
            bub_w = (std::min)(max_w, sz.width);
            bub_h = bub_w * aspect;
            if (bub_h > 240) { bub_h = 240; bub_w = bub_h / aspect; }
        }
        return (prev_same_author ? bub_h : bub_h + 22) + replyPreviewHeight(m) + 6;
    }
    if (m.kind == MsgKind::Video) {
        return (prev_same_author ? 140.0f : 162.0f) + replyPreviewHeight(m) + 6;
    }
    if (m.kind == MsgKind::Sticker) {
        return (prev_same_author ? 100.0f : 122.0f) + replyPreviewHeight(m) + 6;
    }
    // text — 处理 launcher://pack/ link 卡片
    auto find_url = [](const std::wstring& s) -> std::wstring {
        size_t p = s.find(L"launcher://");
        if (p == std::wstring::npos) {
            p = s.find(L"https://");
            if (p == std::wstring::npos) p = s.find(L"http://");
        }
        if (p == std::wstring::npos) return {};
        size_t e = p;
        while (e < s.size() && s[e] > 0x20 && s[e] != L' ') e++;
        return s.substr(p, e - p);
    };
    std::wstring url = find_url(m.body);
    if (!url.empty() && url.compare(0, 15, L"launcher://pack/") == 0) {
        return (prev_same_author ? 88.0f : 110.0f) + replyPreviewHeight(m) + 6;
    }
    if (m.body.empty()) {
        // 空消息 — 不算高度（实际 paint 也跳过）
        return prev_same_author ? 0.0f : 22.0f + 6.0f;
    }
    float bub_max_w = (std::min)(maxw * 0.65f, 480.0f);
    float text_w = bub_max_w - 28;
    WrappedText layout = wrapTextForWidth(app, m.body, body_fmt, text_w);
    float bub_h = (std::max)(layout.text_h + 18.0f, 30.0f);
    if (m.from == L"me" && m.send_state != MsgSendState::Sent) {
        auto* state_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(7.0f));
        std::wstring state_text = (m.send_state == MsgSendState::Pending)
            ? (m.error_text.empty() ? L"sending..." : m.error_text)
            : (m.error_text.empty() ? L"send failed" : m.error_text);
        DWRITE_TEXT_METRICS sm{};
        app.texts().measure(state_fmt, state_text, text_w, 64, &sm);
        bub_h += (std::max)(14.0f, sm.height + 4.0f);
    }
    if (!url.empty()) bub_h += 4;
    return (prev_same_author ? bub_h : bub_h + 22) + replyPreviewHeight(m) + 6;
}

// ============== 单条气泡 ==============
// 自己消息靠右 / 别人靠左。idx = 在 streamFor(g_active) 里的位置，用于消息 hit 注册（右键菜单）。
static float paintBubble(D2DApp& app, const Msg& m, int idx, float x, float y, float maxw,
                         bool prev_same_author) {
    const Palette& pal = palette();
    auto* ctx = app.ctx();
    auto& br = app.brushes();

    // Text 类型 + body 全空 — 整条直接跳过（避免画了头像/作者却没气泡的"幽灵行"）
    if (m.kind == MsgKind::Text && m.body.empty()) {
        return 0;
    }

    if (m.kind == MsgKind::DayDivider) {
        auto* pill_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(7.5f));
        float tw = measureW(app, m.body, pill_fmt) + 24;
        float bx = x + (maxw - tw) * 0.5f;
        prim::fillRR(ctx, bx, y + 6, tw, 18, 9.0f, br.solidA(pal.text, 0.05f));
        prim::drawText_(ctx, m.body, pill_fmt,
                        bx, y + 9, tw, 14,
                        br.solid(pal.text_muted),
                        DWRITE_TEXT_ALIGNMENT_CENTER);
        return 30;
    }
    if (m.kind == MsgKind::System) {
        auto* sys_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(8.5f));
        float tw = measureW(app, m.body, sys_fmt) + 24;
        float bx = x + (maxw - tw) * 0.5f;
        prim::fillRR(ctx, bx, y + 4, tw, 22, 11.0f, br.solidA(pal.text, 0.05f));
        prim::drawText_(ctx, m.body, sys_fmt,
                        bx, y + 8, tw, 16,
                        br.solid(pal.text_muted),
                        DWRITE_TEXT_ALIGNMENT_CENTER);
        return 32;
    }

    bool me = (m.from == L"me");

    auto* author_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(8.0f),
                                          DWRITE_FONT_WEIGHT_BOLD);
    auto* time_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(7.0f));
    auto* body_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(9.5f));

    // 头像 28×28 — me 在右边，别人在左边
    constexpr float ar = 14.0f;
    constexpr float gap = 10.0f;
    float avatar_x = me ? (x + maxw - ar * 2) : x;
    float ay = y + 4;
    if (!prev_same_author) {
        bool drew_real = false;
        // me 用真头像（g_avatar_path BitmapBrush 圆形裁剪）
        if (me && !g_avatar_path.empty()) {
            auto* abmp = app.images().fromFile(g_avatar_path);
            if (abmp) {
                D2D1_BITMAP_BRUSH_PROPERTIES bp = D2D1::BitmapBrushProperties(
                    D2D1_EXTEND_MODE_CLAMP, D2D1_EXTEND_MODE_CLAMP,
                    D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                ComPtr<ID2D1BitmapBrush> bb;
                if (SUCCEEDED(ctx->CreateBitmapBrush(abmp, bp, &bb))) {
                    D2D1_SIZE_F sz = abmp->GetSize();
                    if (sz.width > 0 && sz.height > 0) {
                        float sx = (ar * 2) / sz.width;
                        float sy = (ar * 2) / sz.height;
                        auto mt = D2D1::Matrix3x2F::Scale({sx, sy}, {0, 0})
                                * D2D1::Matrix3x2F::Translation(avatar_x, ay);
                        bb->SetTransform(mt);
                        ctx->FillEllipse(D2D1::Ellipse({avatar_x + ar, ay + ar}, ar, ar), bb.Get());
                        drew_real = true;
                    }
                }
            }
        }
        if (!drew_real) {
            prim::fillCircle(ctx, avatar_x + ar, ay + ar, ar, br.solid(pal.primary));
            // 自己用 nickname 首字，否则用 from 首字（对方 UUID 前 8 字的首字符）
            std::wstring avatar_label = displayAuthorFor(m, app.hwnd());
            wchar_t key = avatar_label.empty() ? L'?' : avatar_label[0];
            wchar_t initial[2] = { (wchar_t)towupper(key), 0 };
            auto* init_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(8.5f),
                                                DWRITE_FONT_WEIGHT_BOLD);
            prim::drawText_(ctx, initial, init_fmt,
                            avatar_x, ay, ar * 2, ar * 2,
                            br.solid(0xFFFFFFFF),
                            DWRITE_TEXT_ALIGNMENT_CENTER,
                            DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
        // 头像左键看主页（me 自己跳过）
        std::wstring profile_key = me ? selfAuthorKey() : (!m.peer_key.empty() ? m.peer_key : m.from);
        if (!profile_key.empty()) {
            g_avatar_hits.push_back({ { avatar_x, ay, ar * 2, ar * 2 }, profile_key });
            hit({ avatar_x, ay, ar * 2, ar * 2 }, [profile_key](){
                auto* p = new std::wstring(profile_key);
                PostMessageW(GetActiveWindow(), WM_APP + 37, 0, (LPARAM)p);
            }, true);
        }
    }
    // bub 起点：left/right
    float bub_inner_w_max = (std::min)(maxw - ar * 2 - gap, 480.0f);
    auto bub_x_for = [&](float bub_w) -> float {
        if (me) return avatar_x - gap - bub_w;
        return avatar_x + ar * 2 + gap;
    };
    float reply_h = replyPreviewHeight(m);
    float reply_y = y + (prev_same_author ? 0 : 22);
    float bub_y = reply_y + reply_h;
    if (reply_h > 0.0f) {
        float ref_w = (std::min)(maxw * 0.58f, 420.0f);
        float ref_x = me ? (avatar_x - gap - ref_w) : (avatar_x + ar * 2 + gap);
        bool clickable = m.reply_to_id > 0;
        prim::fillRR(ctx, ref_x, reply_y + 2, ref_w, 22, 7.0f,
                     br.solidA(me ? 0xFFFFFF : pal.primary, me ? 0.12f : 0.10f));
        prim::fillRR(ctx, ref_x + 8, reply_y + 6, 3, 14, 1.5f,
                     br.solidA(me ? 0xFFFFFF : pal.primary, clickable ? 0.80f : 0.42f));
        auto* ref_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(8.0f));
        prim::drawText_(ctx, replyPreviewLine(m), ref_fmt,
                        ref_x + 16, reply_y + 5, ref_w - 24, 14,
                        br.solidA(me ? 0xFFFFFF : pal.text_muted, clickable ? 0.92f : 0.68f));
        if (clickable) {
            int64_t target_id = m.reply_to_id;
            std::wstring target_slug = g_active;
            hit({ ref_x, reply_y + 2, ref_w, 22 }, [target_slug, target_id]() {
                focusMessage(target_slug, target_id);
            }, true);
        }
    }
    if (!prev_same_author) {
        // author + time 在气泡上方那一行
        // me：和气泡一样靠右；别人：和气泡靠左
        // 自己显示真昵称（不是 "me" 字面量）
        std::wstring author_disp;
        if (me) {
            author_disp = !m.author.empty() ? m.author
                        : (g_user.nickname.empty() ? std::wstring(L"我") : g_user.nickname);
        } else {
            author_disp = m.author.empty() ? m.from : m.author;
        }
        std::wstring author_display = displayAuthorFor(m, app.hwnd());
        float aw_ = measureW(app, author_display, author_fmt);
        float tw_ = m.time.empty() ? 0 : (measureW(app, m.time, time_fmt) + 8);
        float meta_w = aw_ + tw_;
        float meta_x = me ? (avatar_x - gap - meta_w) : (avatar_x + ar * 2 + gap);
        prim::drawText_(ctx, author_display, author_fmt,
                        meta_x, y + 2, aw_ + 4, 14,
                        br.solid(me ? pal.primary : pal.text));
        if (!m.time.empty()) {
            prim::drawText_(ctx, m.time, time_fmt,
                            meta_x + aw_ + 8, y + 4, tw_, 12,
                            br.solid(pal.text_muted));
        }
    }

    // ---------- 媒体气泡 (Image/Gif/Video) ----------
    if (m.kind == MsgKind::Image || m.kind == MsgKind::Gif) {
        float bub_w = 240, bub_h = 180;
        ID2D1Bitmap* draw_bmp = nullptr;
        D2D1_SIZE_F sz = { 0, 0 };

        if (m.kind == MsgKind::Gif) {
            // GIF 多帧 — IWICBitmapDecoder GetFrameCount + /grctlext/Delay
            auto* anim = app.gifs().fromFile(m.body);
            if (anim) {
                draw_bmp = app.gifs().frameAt(anim, stages::g_time_in_stage);
                sz.width = (float)anim->width;
                sz.height = (float)anim->height;
            }
        }
        if (!draw_bmp) {
            // Image 或 GIF 解码失败 → 退到单帧 ID2D1Bitmap
            draw_bmp = app.images().fromFile(m.body);
            if (draw_bmp) sz = draw_bmp->GetSize();
        }

        if (draw_bmp) {
            if (sz.width > 0 && sz.height > 0) {
                float aspect = sz.height / sz.width;
                float max_w = (std::min)(maxw * 0.55f, 320.0f);
                bub_w = (std::min)(max_w, sz.width);
                bub_h = bub_w * aspect;
                if (bub_h > 240) { bub_h = 240; bub_w = bub_h / aspect; }
            }
        }
        float bub_x = bub_x_for(bub_w);
        if (draw_bmp) {
            // 真圆角 mask（之前 PushAxisAlignedClip 只裁矩形 4 角是直的）
            prim::pushLayerRR(ctx, app.factory(), bub_x, bub_y, bub_w, bub_h, 12.0f);
            ctx->DrawBitmap(draw_bmp, D2D1::RectF(bub_x, bub_y, bub_x + bub_w, bub_y + bub_h),
                            1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            prim::popLayer(ctx);
            prim::strokeRR(ctx, bub_x, bub_y, bub_w, bub_h, 12.0f,
                           br.solidA(pal.divider, 0.5f), 1.0f);
        } else {
            prim::fillRR(ctx, bub_x, bub_y, bub_w, bub_h, 12.0f,
                         br.solid(pal.surface));
            prim::drawText_(ctx, m.kind == MsgKind::Gif ? L"[GIF]" : L"[Image]", body_fmt,
                            bub_x, bub_y + bub_h * 0.4f, bub_w, 22,
                            br.solid(pal.text_muted),
                            DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        if (m.kind == MsgKind::Gif) {
            prim::fillRR(ctx, bub_x + bub_w - 36, bub_y + 6, 30, 16, 4,
                         br.solidA(0x000000, 0.55f));
            auto* gif_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(7.0f),
                                               DWRITE_FONT_WEIGHT_BOLD);
            prim::drawText_(ctx, L"GIF", gif_fmt,
                            bub_x + bub_w - 36, bub_y + 7, 30, 14,
                            br.solid(0xFFFFFFFF),
                            DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        // 整行 hit — 让用户右键 row 任何位置（含 avatar / meta header）都弹菜单
        // 不只是 bubble 本体（sticker 100×100 太精确，用户难命中）
        {
            float _row_h = (prev_same_author ? bub_h : bub_h + 22) + reply_h;
            g_msg_hits.push_back({ { x, y, maxw, _row_h }, idx });
        }
        return (prev_same_author ? bub_h : bub_h + 22) + reply_h + 6;
    }

    if (m.kind == MsgKind::Video) {
        float bub_w = 240, bub_h = 140;
        float bub_x = bub_x_for(bub_w);
        prim::fillRR(ctx, bub_x, bub_y, bub_w, bub_h, 12.0f,
                     br.solid(pal.surface));
        prim::fillCircle(ctx, bub_x + bub_w * 0.5f, bub_y + bub_h * 0.5f, 28,
                         br.solidA(0x000000, 0.65f));
        prim::strokeCircle(ctx, bub_x + bub_w * 0.5f, bub_y + bub_h * 0.5f, 28,
                           br.solid(0xFFFFFFFF), 2.0f);
        icons::drawIcon(app, icons::Name::Play,
                        bub_x + bub_w * 0.5f - 12,
                        bub_y + bub_h * 0.5f - 12, 24,
                        0xFFFFFFFF);
        prim::drawText_(ctx, L"点击播放视频", body_fmt,
                        bub_x, bub_y + bub_h - 24, bub_w, 18,
                        br.solid(pal.text_muted),
                        DWRITE_TEXT_ALIGNMENT_CENTER);
        // 点击 → WebView2 内嵌播放器
        std::wstring src = m.body;
        hit({ bub_x, bub_y, bub_w, bub_h }, [src](){
            auto* payload = new std::wstring(src);
            PostMessageW(GetActiveWindow(), WM_APP + 46,
                         (WPARAM)payload, 0);
        }, true);
        // 整行 hit — 让用户右键 row 任何位置（含 avatar / meta header）都弹菜单
        // 不只是 bubble 本体（sticker 100×100 太精确，用户难命中）
        {
            float _row_h = (prev_same_author ? bub_h : bub_h + 22) + reply_h;
            g_msg_hits.push_back({ { x, y, maxw, _row_h }, idx });
        }
        return (prev_same_author ? bub_h : bub_h + 22) + reply_h + 6;
    }

    if (m.kind == MsgKind::Sticker) {
        float bub_w = 100, bub_h = 100;
        float bub_x = bub_x_for(bub_w);
        // sticker 也支持 GIF
        ID2D1Bitmap* sbmp = nullptr;
        auto sd = m.body.find_last_of(L'.');
        bool s_is_gif = (sd != std::wstring::npos
                         && (m.body.substr(sd) == L".gif"
                             || m.body.substr(sd) == L".GIF"));
        if (s_is_gif) {
            auto* sa = app.gifs().fromFile(m.body);
            if (sa) sbmp = app.gifs().frameAt(sa, stages::g_time_in_stage);
        }
        if (!sbmp) sbmp = app.images().fromFile(m.body);
        if (sbmp) {
            prim::pushLayerRR(ctx, app.factory(), bub_x, bub_y, bub_w, bub_h, 16.0f);
            ctx->DrawBitmap(sbmp, D2D1::RectF(bub_x, bub_y, bub_x + bub_w, bub_y + bub_h),
                            1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            prim::popLayer(ctx);
        } else {
            prim::fillRR(ctx, bub_x, bub_y, bub_w, bub_h, 16.0f,
                         br.solid(pal.surface));
        }
        // 整行 hit — 让用户右键 row 任何位置（含 avatar / meta header）都弹菜单
        // 不只是 bubble 本体（sticker 100×100 太精确，用户难命中）
        {
            float _row_h = (prev_same_author ? bub_h : bub_h + 22) + reply_h;
            g_msg_hits.push_back({ { x, y, maxw, _row_h }, idx });
        }
        return (prev_same_author ? bub_h : bub_h + 22) + reply_h + 6;
    }

    // ---------- Text bubble ----------
    // 空 body 直接跳过（避免后端 trim 后空白 + 错误 payload 显示成 28-px 小气泡）
    if (m.body.empty()) {
        return prev_same_author ? 0.0f : 22.0f + 6.0f;
    }
    // 如果文本里有 launcher://pack/<short>，特殊渲染为 pack 分享卡片（缩略图 + 标题 + 行动按钮）。
    auto find_url = [](const std::wstring& s) -> std::wstring {
        // launcher:// 优先
        size_t p = s.find(L"launcher://");
        if (p == std::wstring::npos) {
            p = s.find(L"https://");
            if (p == std::wstring::npos) p = s.find(L"http://");
        }
        if (p == std::wstring::npos) return {};
        size_t e = p;
        while (e < s.size() && s[e] > 0x20 && s[e] != L' ') e++;
        return s.substr(p, e - p);
    };
    std::wstring url = find_url(m.body);
    bool is_pack_link = !url.empty() && url.compare(0, 15, L"launcher://pack/") == 0;
    if (is_pack_link) {
        // 抽 short_name
        std::wstring short_w = url.substr(15);
        std::string short_a;
        for (wchar_t c : short_w) if (c) short_a.push_back((char)c);
        // pack-share 卡片：320×88，左边 64×64 缩略图（cache 后的 cover）+ 标题 + 提示
        float bub_w = 320, bub_h = 88;
        float bub_x = bub_x_for(bub_w);
        // 渐变 / 主色描边
        prim::fillRR(ctx, bub_x, bub_y, bub_w, bub_h, 12.0f, br.solid(pal.card));
        prim::strokeRR(ctx, bub_x, bub_y, bub_w, bub_h, 12.0f,
                       br.solidA(pal.primary, 0.4f), 1.5f);
        // 缩略图占位（如果 g_pack_preview 跟当前 short 匹配则用 cover；否则灰）
        ID2D1Bitmap* thumb = nullptr;
        {
            std::lock_guard<std::mutex> lk(sticker::g_pack_preview_mtx);
            if (sticker::g_pack_preview.short_name == short_a
                && !sticker::g_pack_preview.cover_path.empty()) {
                thumb = app.images().fromFile(sticker::g_pack_preview.cover_path);
            }
        }
        if (thumb) {
            prim::pushLayerRR(ctx, app.factory(), bub_x + 12, bub_y + 12, 64, 64, 8.0f);
            ctx->DrawBitmap(thumb, D2D1::RectF(bub_x + 12, bub_y + 12, bub_x + 76, bub_y + 76),
                            1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            prim::popLayer(ctx);
        } else {
            prim::fillRR(ctx, bub_x + 12, bub_y + 12, 64, 64, 8.0f,
                         br.solidA(pal.primary, 0.18f));
            auto* ic_fmt = app.texts().format(L"Segoe UI Emoji", ptToDip(20.0f));
            prim::drawText_(ctx, L"🎴", ic_fmt,
                            bub_x + 12, bub_y + 16, 64, 56,
                            br.solid(pal.primary),
                            DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        // 标题
        auto* tt_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(10.5f),
                                          DWRITE_FONT_WEIGHT_BOLD);
        auto* sub_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(8.5f));
        prim::drawText_(ctx, L"分享的表情包", tt_fmt,
                        bub_x + 88, bub_y + 14, bub_w - 100, 20,
                        br.solid(pal.text));
        std::wstring code_disp = L"launcher://pack/" + short_w;
        if (code_disp.size() > 32) code_disp = code_disp.substr(0, 32) + L"…";
        prim::drawText_(ctx, code_disp, sub_fmt,
                        bub_x + 88, bub_y + 36, bub_w - 100, 18,
                        br.solid(pal.text_muted));
        prim::drawText_(ctx, L"点击查看 / 添加分组 →", sub_fmt,
                        bub_x + 88, bub_y + 58, bub_w - 100, 18,
                        br.solid(pal.primary));

        std::string short_copy = short_a;
        // 第一次见到这个 short → 后台拉 cover 进缓存（不用阻塞 paint）
        static std::unordered_map<std::string, bool> g_pack_thumb_kicked;
        if (!g_pack_thumb_kicked[short_copy]) {
            g_pack_thumb_kicked[short_copy] = true;
            sticker::previewPackByShort(GetActiveWindow(), short_copy);
        }
        hit({ bub_x, bub_y, bub_w, bub_h }, [short_copy](){
            auto* payload = new std::string(short_copy);
            PostMessageW(GetActiveWindow(), WM_APP + 49,
                         (WPARAM)payload, 0);
        }, true);
        // 整行 hit — 让用户右键 row 任何位置（含 avatar / meta header）都弹菜单
        // 不只是 bubble 本体（sticker 100×100 太精确，用户难命中）
        {
            float _row_h = (prev_same_author ? bub_h : bub_h + 22) + reply_h;
            g_msg_hits.push_back({ { x, y, maxw, _row_h }, idx });
        }
        return (prev_same_author ? bub_h : bub_h + 22) + reply_h + 6;
    }

    float bub_max_w = (std::min)(maxw * 0.65f, 480.0f);
    float text_w = bub_max_w - 28.0f;
    WrappedText body_layout = wrapTextForWidth(app, m.body, body_fmt, text_w);
    auto* state_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(7.0f));
    bool show_state = me && m.send_state != MsgSendState::Sent;
    std::wstring state_text;
    if (show_state) {
        state_text = (m.send_state == MsgSendState::Pending)
            ? (m.error_text.empty() ? L"sending..." : m.error_text)
            : (m.error_text.empty() ? L"send failed" : m.error_text);
    }
    float state_h = 0.0f;
    if (show_state) {
        DWRITE_TEXT_METRICS sm{};
        app.texts().measure(state_fmt, state_text, text_w, 64, &sm);
        state_h = (std::max)(14.0f, sm.height + 4.0f);
    }
    float content_w = (std::max)((float)std::ceil(body_layout.max_line_w) + 8.0f, 16.0f);
    float bub_w = (std::max)(44.0f, (std::min)(content_w + 28.0f, bub_max_w));
    if (body_layout.max_line_w >= text_w - 1.0f) bub_w = bub_max_w;
    float bub_h = (std::max)(body_layout.text_h + 18.0f, 30.0f) + state_h;
    float bub_x = bub_x_for(bub_w);

    uint32_t bub_bg = me
        ? (m.send_state == MsgSendState::Failed ? 0xFFE34B4B : pal.primary)
        : pal.card;
    uint32_t bub_fg = me ? 0xFFFFFFFF : pal.text;
    prim::fillRR(ctx, bub_x, bub_y, bub_w, bub_h, 12.0f,
                 m.send_state == MsgSendState::Pending ? br.solidA(bub_bg, 0.72f) : br.solid(bub_bg));
    prim::drawText_(ctx, body_layout.text, body_fmt,
                    bub_x + 12, bub_y + 8, bub_w - 24, body_layout.text_h + 4.0f,
                    br.solid(bub_fg));
    if (show_state) {
        prim::drawText_(ctx, state_text, state_fmt,
                        bub_x + 12, bub_y + bub_h - state_h - 2, bub_w - 24, state_h,
                        br.solidA(0xFFFFFFFF, m.send_state == MsgSendState::Failed ? 0.95f : 0.72f),
                        DWRITE_TEXT_ALIGNMENT_TRAILING);
    }

    if (!url.empty()) {
        // 链接气泡下加一个小提示行 + hit 整个气泡 → WebView2 打开
        auto* link_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(7.5f),
                                            DWRITE_FONT_WEIGHT_BOLD);
        prim::drawText_(ctx, L"↗ 点击打开", link_fmt,
                        bub_x + 14, bub_y + bub_h - 14, bub_w - 28, 12,
                        br.solidA(me ? 0xFFFFFF : 0xC96442, 0.7f));
        bub_h += 4;
        std::wstring url_copy = url;
        hit({ bub_x, bub_y, bub_w, bub_h }, [url_copy](){
            auto* payload = new std::wstring(url_copy);
            PostMessageW(GetActiveWindow(), WM_APP + 47,
                         (WPARAM)payload, 0);
        }, true);
    }
    {
        float _row_h = (prev_same_author ? bub_h : bub_h + 22) + reply_h;
        g_msg_hits.push_back({ { x, y, maxw, _row_h }, idx });
    }

    return (prev_same_author ? bub_h : bub_h + 22) + reply_h + 6;
}

// ============== 异步发消息 ==============
namespace {
struct SendArg {
    std::string session_token;
    std::string chat_id;
    std::string kind;          // text / sticker / image / gif
    std::string client_msg_id;
    int64_t reply_to_id = 0;
    std::vector<std::wstring> mentions;
    std::wstring slug;
    std::wstring body;
    HWND hwnd;
};
struct LinkArg {
    std::wstring slug;
    std::string client_msg_id;
    std::wstring body;
    int64_t mid = 0;
    bool ok = false;
    std::wstring error_text;
};
std::mutex g_link_mtx;
std::vector<LinkArg> g_pending_links;
}

static std::string makeClientMsgId() {
    GUID g{};
    if (FAILED(CoCreateGuid(&g))) return {};
    char buf[40]{};
    sprintf_s(buf, "%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X",
        g.Data1, g.Data2, g.Data3,
        g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3],
        g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
    for (char& c : buf) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return buf;
}

static std::wstring localTimeText(time_t tt) {
    tm local_tm{};
    localtime_s(&local_tm, &tt);
    wchar_t tb[16]{};
    wcsftime(tb, 16, L"%H:%M", &local_tm);
    return tb;
}

// 主线程 WM_APP+52 调 — 找最近一条 me message 没绑定 server_id 的，写入 mid
void applySendResult() {
    std::vector<LinkArg> arr;
    {
        std::lock_guard<std::mutex> lk(g_link_mtx);
        arr.swap(g_pending_links);
    }
    for (auto& la : arr) {
        auto& msgs = streamFor(la.slug);
        for (auto it = msgs.rbegin(); it != msgs.rend(); ++it) {
            if (it->from == L"me" && it->server_id == 0
                && ((!la.client_msg_id.empty() && it->client_msg_id == la.client_msg_id)
                    || (la.client_msg_id.empty() && it->body == la.body))) {
                if (la.ok) {
                    it->server_id = la.mid;
                    it->send_state = MsgSendState::Sent;
                    it->error_text.clear();
                    rememberReplySnapshot(la.slug, *it);
                    resolveLocalReplyTargets(la.slug);
                } else {
                    it->send_state = MsgSendState::Failed;
                    it->error_text = la.error_text.empty() ? L"发送失败" : la.error_text;
                }
                break;
            }
        }
        for (auto& msg : msgs) {
            if (!msg.waiting_reply_target || msg.reply_to_id <= 0) continue;
            msg.waiting_reply_target = false;
            msg.error_text.clear();
            sendChatMessage(GetActiveWindow(), msg.body, "text",
                            msg.client_msg_id, msg.reply_to_id);
        }
    }
}

static void sendChatMessage(HWND hwnd, const std::wstring& body, const char* kind,
                            std::string client_msg_id, int64_t reply_to_id) {
    std::wstring slug = g_active;
    if (client_msg_id.empty()) client_msg_id = makeClientMsgId();
    std::vector<std::wstring> mentions;
    mentions.reserve(g_pending_mentions.size());
    for (auto& m : g_pending_mentions) mentions.push_back(m.user_id);
    auto fail = [&](const std::wstring& err) {
        {
            std::lock_guard<std::mutex> lk(g_link_mtx);
            g_pending_links.push_back({ slug, client_msg_id, body, 0, false, err });
        }
        PostMessageW(hwnd, WM_APP + 52, 0, 0);
    };
    if (g_session_token.empty()) { fail(L"未登录"); return; }
    auto* ch = activeChannel();
    if (ch->id.empty()) { fail(L"channel not ready"); return; }
    if (ch->id.empty()) return;     // 还没拿到 backend uuid
    auto& msgs = streamFor(slug);
    for (auto it = msgs.rbegin(); it != msgs.rend(); ++it) {
        if (it->from == L"me" && it->server_id == 0
            && it->body == body && it->client_msg_id.empty()) {
            it->client_msg_id = client_msg_id;
            break;
        }
    }
    auto* a = new SendArg{ g_session_token, ch->id, kind, client_msg_id, reply_to_id, mentions, slug, body, hwnd };
    CreateThread(nullptr, 0, [](LPVOID lp) -> DWORD {
        std::unique_ptr<SendArg> a((SendArg*)lp);
        auto push_result = [&](bool ok, int64_t mid, const std::wstring& err) {
            {
                std::lock_guard<std::mutex> lk(g_link_mtx);
                g_pending_links.push_back({ a->slug, a->client_msg_id, a->body, mid, ok, err });
            }
            PostMessageW(a->hwnd, WM_APP + 52, 0, 0);
        };
        std::string payload;
        if (a->kind == "image" || a->kind == "video" || a->kind == "gif") {
            if (a->kind == "gif") {
                payload = sticker::stickerPayloadJsonForPath(a->body);
                if (payload == "{}") payload.clear();
            }
            if (payload.empty()) {
                auto media = launcher::d2d::fetch::uploadMediaFile(a->body);
                if (!media.ok) {
                    push_result(false, 0, media.error.empty() ? L"media upload failed" : utf8wHist(media.error));
                    return 0;
                }
                payload = launcher::d2d::fetch::mediaPayloadJson(media);
            }
        } else if (a->kind == "sticker") {
            payload = sticker::stickerPayloadJsonForPath(a->body);
            if (payload == "{}") {
                auto media = launcher::d2d::fetch::uploadMediaFile(a->body);
                if (!media.ok) {
                    push_result(false, 0, media.error.empty() ? L"sticker upload failed" : utf8wHist(media.error));
                    return 0;
                }
                payload = launcher::d2d::fetch::mediaPayloadJson(media);
            }
        } else {
            payload = "\"" + net::jsonEscape(a->body) + "\"";
        }
        std::string b = "{\"session_token\":\"" + a->session_token
                      + "\",\"chat_id\":\"" + a->chat_id
                      + "\",\"msg_type\":\"" + a->kind
                      + "\",\"payload\":" + payload;
        if (!a->client_msg_id.empty()) {
            b += ",\"client_msg_id\":\"" + a->client_msg_id + "\"";
        }
        if (a->reply_to_id > 0) {
            b += ",\"reply_to_id\":" + std::to_string(a->reply_to_id);
        }
        b += ",\"mentions\":[";
        for (size_t i = 0; i < a->mentions.size(); ++i) {
            if (i) b += ",";
            b += "\"" + net::jsonEscape(a->mentions[i]) + "\"";
        }
        b += "]";
        b += "}";
        auto r = net::postJson(L"/api/chat/send", b);
        if (r.ok()) {
            int64_t mid = net::jsonInt(r.body, "id");
            if (mid > 0) {
                push_result(true, mid, {});
                return 0;
            }
        }
        push_result(false, 0, r.body.empty() ? L"send failed" : utf8wHist(r.body.substr(0, 80)));
        return 0;
    }, a, 0, nullptr);
}

// 异步删除消息 — 调后端 chat/delete (软删除)，本地立即移除
void deleteMessage(HWND hwnd, const std::wstring& slug, int64_t server_id) {
    {
        // 本地立即移除
        auto& msgs = streamFor(slug);
        for (auto it = msgs.begin(); it != msgs.end(); ++it) {
            if (it->server_id == server_id) { msgs.erase(it); break; }
        }
    }
    if (server_id == 0 || g_session_token.empty()) return;
    struct A { int64_t mid; HWND h; };
    auto* a = new A{ server_id, hwnd };
    CreateThread(nullptr, 0, [](LPVOID lp) -> DWORD {
        std::unique_ptr<A> a((A*)lp);
        char buf[64]; sprintf_s(buf, "%lld", (long long)a->mid);
        std::string body = "{\"session_token\":\"" + g_session_token
                         + "\",\"message_id\":" + buf + "}";
        auto r = net::postJson(L"/api/chat/delete", body);
        PostMessageW(a->h, WM_APP + 53, r.ok() ? 1 : 0, 0);
        return 0;
    }, a, 0, nullptr);
}

// WS 收到别人删除 — 在所有 stream 里找 server_id 摘掉
void onWsMessageDeleted(int64_t server_id) {
    std::lock_guard<std::mutex> lk(g_streams_mtx);
    for (auto& [slug, msgs] : g_streams) {
        for (auto it = msgs.begin(); it != msgs.end(); ++it) {
            if (it->server_id == server_id) { msgs.erase(it); return; }
        }
    }
}

// 兼容老调用名
static void sendTextMessage(HWND hwnd, const std::wstring& text) {
    int64_t reply_to_id = 0;
    std::string reply_client_msg_id;
    if (g_pending_reply.active && g_pending_reply.slug == g_active) {
        reply_to_id = g_pending_reply.id;
        reply_client_msg_id = g_pending_reply.client_msg_id;
        if (reply_to_id <= 0) {
            reply_to_id = serverIdForClientMsgId(g_active, reply_client_msg_id);
        }
    }
    Msg m;
    m.kind = MsgKind::Text;
    m.from = L"me";
    m.author = g_user.nickname;
    m.author_key = selfAuthorKey();
    m.status = L"online";
    m.body = text;
    m.time = localTimeText();
    m.client_msg_id = makeClientMsgId();
    m.reply_to_id = reply_to_id;
    m.reply_client_msg_id = reply_client_msg_id;
    if (g_pending_reply.active && g_pending_reply.slug == g_active) {
        m.reply_author = g_pending_reply.author;
        m.reply_preview = g_pending_reply.preview;
    }
    m.send_state = MsgSendState::Pending;
    std::string client_msg_id = m.client_msg_id;
    bool wait_for_reply_target = g_pending_reply.active
        && g_pending_reply.slug == g_active
        && reply_to_id <= 0
        && !reply_client_msg_id.empty();
    m.waiting_reply_target = wait_for_reply_target;
    if (wait_for_reply_target) {
        m.error_text = L"waiting reply target...";
    }
    appendLocalMessage(std::move(m));
    if (!wait_for_reply_target) {
        sendChatMessage(hwnd, text, "text", client_msg_id, reply_to_id);
    }
    g_pending_reply = PendingReply{};
    g_pending_mentions.clear();
}

// ============== Composer ==============
static void paintComposer(D2DApp& app, float ax, float ay, float aw, float ah) {
    const Palette& pal = palette();
    auto* ctx = app.ctx();
    auto& br = app.brushes();
    prim::fillRect(ctx, ax, ay, aw, ah, br.solid(pal.bg));
    prim::drawLine(ctx, ax, ay, ax + aw, ay,
                   br.solid(pal.divider), 1.0f);

    float reply_h = g_pending_reply.active ? 22.0f : 0.0f;
    if (g_pending_reply.active) {
        auto* reply_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(8.0f),
                                             DWRITE_FONT_WEIGHT_BOLD);
        auto* reply_body_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(8.0f));
        float rx = ax + 58.0f;
        float ry = ay + 5.0f;
        float rw = aw - 112.0f;
        prim::fillRR(ctx, rx, ry, rw, 18.0f, 6.0f, br.solidA(pal.primary, 0.10f));
        prim::drawText_(ctx, L"Reply", reply_fmt,
                        rx + 10, ry + 3, 42, 12, br.solid(pal.primary));
        std::wstring preview = g_pending_reply.author + L": " + g_pending_reply.preview;
        if (preview.size() > 90) preview = preview.substr(0, 90) + L"...";
        prim::drawText_(ctx, preview, reply_body_fmt,
                        rx + 54, ry + 3, rw - 82, 12, br.solid(pal.text_muted));
        LayoutRect cancel{ rx + rw - 22, ry, 18, 18 };
        bool ch = cancel.contains(g_mouse);
        if (ch) prim::fillCircle(ctx, cancel.x + 9, cancel.y + 9, 8, br.solidA(pal.text, 0.12f));
        icons::drawIcon(app, icons::Name::X, cancel.x + 4, cancel.y + 4, 10,
                        ch ? pal.text : pal.text_muted);
        hit(cancel, [](){ g_pending_reply = PendingReply{}; }, true);
    }

    const float ico_sz = 30.0f;
    float ix = ax + 14.0f;
    float iy = ay + reply_h + ((ah - reply_h) - ico_sz) * 0.5f;
    LayoutRect emoji_btn{ ix, iy, ico_sz, ico_sz };
    bool ehov = emoji_btn.contains(g_mouse);
    if (ehov) {
        prim::fillRR(ctx, ix, iy, ico_sz, ico_sz, 8.0f,
                     br.solid(pal.card));
    }
    icons::drawIcon(app, icons::Name::Smile, ix + 6, iy + 6, 18,
                    ehov ? pal.text : pal.text_muted);
    hit(emoji_btn, [](){
        setPickerOpen(!g_picker_open);
    }, true);

    // textarea
    float fx = ix + ico_sz + 10.0f;
    float send_w = 38.0f;
    float fw = aw - (fx - ax) - 14.0f - send_w - 10.0f;
    float fh = ico_sz;
    float fy = iy;
    g_composer.bounds = { fx, fy, fw, fh };

    prim::fillRR(ctx, fx, fy, fw, fh, fh * 0.5f, br.solid(pal.card));
    auto* border = g_focus_composer ? br.solid(pal.primary) : br.solid(pal.divider);
    prim::strokeRR(ctx, fx, fy, fw, fh, fh * 0.5f, border,
                   g_focus_composer ? 1.4f : 1.0f);
    if (g_focus_composer) {
        prim::strokeRR(ctx, fx - 2, fy - 2, fw + 4, fh + 4, fh * 0.5f + 2,
                       br.solidA(pal.primary, 0.10f), 3.0f);
    }

    auto* tx_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(9.5f));
    const float pad_l = 16.0f;
    const float text_y = fy + (fh - 14.0f) * 0.5f;
    const float text_w = fw - pad_l * 2;
    float caret_w = g_composer.text.empty()
        ? 0.0f
        : caretMeasureW(app, g_composer.displaySlice(0, g_composer.cursor), tx_fmt);
    float text_scroll_x = (std::max)(0.0f, caret_w - text_w + 6.0f);
    g_composer_caret_xs.clear();
    g_composer_caret_xs.reserve(g_composer.text.size() + 1);
    for (int i = 0; i <= (int)g_composer.text.size(); ++i) {
        g_composer_caret_xs.push_back(
            fx + pad_l + caretMeasureW(app, g_composer.displaySlice(0, i), tx_fmt) - text_scroll_x);
    }

    if (g_composer.text.empty()) {
        prim::drawText_(ctx, L"写点什么…", tx_fmt,
                        fx + pad_l, text_y, text_w, 18,
                        br.solid(pal.text_muted));
    } else {
        ctx->PushAxisAlignedClip(D2D1::RectF(fx + pad_l, fy + 4,
                                             fx + pad_l + text_w, fy + fh - 4),
                                 D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        // 选区
        if (g_focus_composer && g_composer.hasSelection()) {
            float pre_w = caretMeasureW(app,
                g_composer.displaySlice(0, g_composer.selStart()), tx_fmt);
            float in_w = caretMeasureW(app,
                g_composer.displaySlice(g_composer.selStart(), g_composer.selEnd()), tx_fmt);
            prim::fillRect(ctx,
                           fx + pad_l + pre_w - text_scroll_x, text_y - 1, in_w, 18,
                           br.solidA(pal.primary, 0.38f));
        }
        prim::drawText_(ctx, g_composer.text, tx_fmt,
                        fx + pad_l - text_scroll_x, text_y,
                        (std::max)(text_w, caretMeasureW(app, g_composer.text, tx_fmt) + 4.0f), 18,
                        br.solid(pal.text));
        ctx->PopAxisAlignedClip();
    }

    // caret
    if (g_focus_composer && !g_composer.hasSelection()) {
        float pre_w = caret_w - text_scroll_x;
        int phase = (int)(stages::g_time_in_stage * 1000) % 1000;
        if (phase < 500) {
            prim::drawLine(ctx,
                           fx + pad_l + pre_w, fy + 7,
                           fx + pad_l + pre_w, fy + fh - 7,
                           br.solid(pal.primary), 1.5f);
        }
    }

    hit(g_composer.bounds, [](){ g_focus_composer = true; }, true);

    // send btn
    float sx = ax + aw - 14 - send_w;
    float sy = iy + (fh - send_w) * 0.5f;
    bool can_send = !g_composer.text.empty();
    LayoutRect send_btn{ sx, sy, send_w, send_w };
    bool sh_ = send_btn.contains(g_mouse);
    uint32_t sbg = !can_send ? fadeArgb(pal.primary, 0.55f)
                            : (sh_ ? pal.primary_hover : pal.primary);
    prim::fillCircle(ctx, sx + send_w * 0.5f, sy + send_w * 0.5f, send_w * 0.5f,
                     br.solid(sbg));
    icons::drawIcon(app, icons::Name::ArrowUp, sx + 10, sy + 10, 18, 0xFFFFFFFF, 2.1f);
    if (can_send) {
        hit(send_btn, []() {
            sendTextMessage(GetActiveWindow(), g_composer.text);
            g_composer.text.clear();
            g_composer.cursor = 0;
            g_composer.clearSel();
            g_focus_composer = true;
        }, true);
    }
}

// ============== Pane (header + stream + composer) ==============
static void paintChatPane(D2DApp& app, float ax, float ay, float aw, float ah) {
    const Palette& pal = palette();
    auto* ctx = app.ctx();
    auto& br = app.brushes();
    prim::fillRect(ctx, ax, ay, aw, ah, br.solid(pal.bg));
    g_avatar_hits.clear();   // 帧首清，paintBubble 会填充
    g_msg_hits.clear();      // 帧首清，paintBubble 注册消息体 hit
    g_msg_row_hits.clear();

    // header
    float hdr_h = 56;
    prim::drawLine(ctx, ax, ay + hdr_h, ax + aw, ay + hdr_h,
                   br.solid(pal.divider), 1.0f);

    auto* ch = activeChannel();
    auto* hash_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(14.0f),
                                        DWRITE_FONT_WEIGHT_BOLD);
    auto* name_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(11.0f),
                                        DWRITE_FONT_WEIGHT_BOLD);
    auto* sub_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(8.5f));
    prim::drawText_(ctx, L"#", hash_fmt,
                    ax + 18, ay + 16, 16, 22, br.solid(pal.text_muted));
    prim::drawText_(ctx, ch->name, name_fmt,
                    ax + 36, ay + 14, 200, 22, br.solid(pal.text));
    prim::drawText_(ctx,
                    ch->is_market ? L"社区交易市场（出售 .cfg / 灵敏度配置）" : L"官方频道",
                    sub_fmt,
                    ax + 36, ay + 32, 300, 16, br.solid(pal.text_muted));

    // 右上 search / more 按钮
    float btn_x = ax + aw - 14 - 34 * 2 - 4;
    for (int i = 0; i < 2; ++i) {
        LayoutRect ar{ btn_x, ay + 11, 34, 34 };
        bool hov = ar.contains(g_mouse);
        if (hov) {
            prim::fillRR(ctx, ar.x, ar.y, ar.w, ar.h, 8.0f, br.solid(pal.card));
        }
        icons::Name n = (i == 0) ? icons::Name::Search : icons::Name::More;
        icons::drawIcon(app, n, ar.x + 8, ar.y + 8, 18,
                        hov ? pal.text : pal.text_muted);
        if (i == 0) {
            hit(ar, [](){ modal::openSearch(); }, true);
        }
        btn_x += 38;
    }

    // stream
    float comp_h = 64;
    float stream_y = ay + hdr_h;
    float stream_h = ah - hdr_h - comp_h;
    g_chat_stream_rect = { ax, stream_y, aw, stream_h };

    // 用 PushAxisAlignedClip 保证消息溢出不画到 composer 上
    ctx->PushAxisAlignedClip(D2D1::RectF(ax, stream_y, ax + aw, stream_y + stream_h),
                             D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    auto& msgs = streamFor(g_active);
    if (msgs.empty()) {
        auto* empty_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(10.0f));
        prim::drawText_(ctx,
            ch->is_market ? L"市场频道 — 切到 Market 标签查看商品" :
                            L"还没消息。说点什么吧～",
            empty_fmt,
            ax, stream_y + stream_h * 0.5f - 12, aw, 24,
            br.solid(pal.text_muted),
            DWRITE_TEXT_ALIGNMENT_CENTER);
        ctx->PopAxisAlignedClip();
        // 写一下 scroll 状态避免 wheel 事件来时 g_scroll[g_active] 不存在
        auto& sc = g_scroll[g_active];
        sc.total_height = 0;
        sc.viewport_h = stream_h;
        sc.offset_from_bottom = 0;
        sc.target_offset = 0;
        sc.rendered_count = 0;
        sc.tail_server_id = 0;
        sc.tail_client_msg_id.clear();
        sc.initialized = true;
        // composer
        paintComposer(app, ax, ay + ah - comp_h, aw, comp_h);
        return;
    }
    float maxw = aw - 32;
    // ----- Pass 1：dry-run 测每条 bubble 高度 + 算 total -----
    for (auto& m : msgs) {
        normalizeMsgIdentity(m);
        fillReplySnapshot(m);
        rememberReplySnapshot(g_active, m);
    }
    std::vector<float> heights(msgs.size(), 0);
    float total = kStreamTopPad + kStreamBottomPad;
    for (size_t i = 0; i < msgs.size(); ++i) {
        const Msg& m = msgs[i];
        const Msg* prev = (i > 0) ? &msgs[i - 1] : nullptr;
        bool prev_same = prev && sameGroupedAuthor(*prev, m);
        heights[i] = measureBubbleHeight(app, m, maxw, prev_same);
        total += heights[i];
    }
    // ----- 滚动状态 -----
    auto& sc = g_scroll[g_active];
    bool was_at_bottom = (sc.offset_from_bottom < 16.0f && sc.target_offset < 16.0f);
    bool tail_changed = false;
    int64_t tail_server_id = 0;
    std::string tail_client_msg_id;
    if (!msgs.empty()) {
        tail_server_id = msgs.back().server_id;
        tail_client_msg_id = msgs.back().client_msg_id;
    }
    if (sc.initialized) {
        tail_changed = sc.tail_server_id != tail_server_id
            || sc.tail_client_msg_id != tail_client_msg_id;
    }
    sc.total_height = total;
    sc.viewport_h = stream_h;
    if (!sc.initialized) {
        sc.offset_from_bottom = 0;
        sc.target_offset = 0;
        sc.initialized = true;
    } else if (tail_changed && was_at_bottom && !g_scroll_drag.active) {
        sc.offset_from_bottom = 0;
        sc.target_offset = 0;
    }
    sc.rendered_count = msgs.size();
    sc.tail_server_id = tail_server_id;
    sc.tail_client_msg_id = tail_client_msg_id;
    float max_off = (std::max)(0.0f, total - stream_h);
    if (sc.target_offset > max_off) sc.target_offset = max_off;
    if (sc.target_offset < 0) sc.target_offset = 0;
    if (g_focus_target.server_id > 0
        && !g_focus_target.scroll_applied
        && g_focus_target.slug == g_active) {
        float before = 0.0f;
        for (size_t i = 0; i < msgs.size(); ++i) {
            if (msgs[i].server_id == g_focus_target.server_id) {
                float target_center_from_top = before + heights[i] * 0.5f;
                float target = max_off - target_center_from_top + stream_h * 0.5f;
                if (target > max_off) target = max_off;
                if (target < 0) target = 0;
                sc.target_offset = target;
                sc.offset_from_bottom = target;
                g_focus_target.scroll_applied = true;
                break;
            }
            before += heights[i];
        }
        if (!g_focus_target.scroll_applied) {
            auto state = g_history_state[g_active];
            if (state == HistoryLoadState::Loaded || state == HistoryLoadState::Failed) {
                fetchHistory(GetActiveWindow(), g_active);
            } else if (state == HistoryLoadState::Exhausted) {
                g_focus_target.missing_reported = true;
            }
        }
    }
    // 拖动滚动条期间直接同步；否则平滑 lerp 到 target（每帧 18% 趋近 — 连贯但不软）
    if (g_scroll_drag.active && g_mouse_pressed) {
        // 鼠标 y 增加 = 滚动条下移 = offset 减少（更接近底部）
        float dy = (float)g_mouse.y - g_scroll_drag.anchor_mouse_y;
        float track_h = (std::max)(1.0f, g_scroll_drag.bar_track_h);
        float content_per_track = (g_scroll_drag.total_height - g_scroll_drag.viewport_h) / track_h;
        // bar 下移（dy>0）→ offset 减少（向更新消息靠近）
        float new_off = g_scroll_drag.anchor_offset - dy * content_per_track;
        if (new_off > max_off) new_off = max_off;
        if (new_off < 0) new_off = 0;
        sc.target_offset = new_off;
        sc.offset_from_bottom = new_off;
    } else {
        float diff = sc.target_offset - sc.offset_from_bottom;
        if (std::abs(diff) < 0.5f) sc.offset_from_bottom = sc.target_offset;
        else                       sc.offset_from_bottom += diff * 0.22f;
    }
    if (sc.offset_from_bottom > max_off) sc.offset_from_bottom = max_off;
    if (sc.offset_from_bottom < 0) sc.offset_from_bottom = 0;

    float my_top;
    if (total <= stream_h) {
        // 消息没把 viewport 填满 — 顶端开始，不滚
        my_top = stream_y + kStreamTopPad;
    } else {
        // total > viewport：offset_from_bottom 表示从底部往上滚了多少 px
        // offset = 0 → 锁底（最新消息在底部）→ my_top = stream_bottom - total
        // offset = max_off → 顶部（最早消息在顶部）→ my_top = stream_y
        my_top = stream_y + stream_h - total + sc.offset_from_bottom + kStreamTopPad;
    }

    // ----- Pass 2：实际画 + 注册 hit -----
    float my = my_top;
    for (size_t i = 0; i < msgs.size(); ++i) {
        const Msg& m = msgs[i];
        const Msg* prev = (i > 0) ? &msgs[i - 1] : nullptr;
        bool prev_same = prev && sameGroupedAuthor(*prev, m);
        // 跳过完全在 viewport 之外的 bubble — 既省 D2D 也避免 hit 冲突
        if (my + heights[i] < stream_y || my > stream_y + stream_h) {
            my += heights[i];
            continue;
        }
        if (g_focus_target.server_id > 0
            && g_focus_target.slug == g_active
            && msgs[i].server_id == g_focus_target.server_id
            && stages::g_time_in_stage < g_focus_target.highlight_until) {
            float remain = g_focus_target.highlight_until - stages::g_time_in_stage;
            float alpha = (std::min)(0.18f, 0.08f + remain * 0.05f);
            prim::fillRR(ctx, ax + 10, my - 2, aw - 20, heights[i], 8.0f,
                         br.solidA(pal.primary, alpha));
        }
        g_msg_row_hits.push_back({ { ax, my, aw, heights[i] }, (int)i });
        paintBubble(app, m, (int)i, ax + 16, my, maxw, prev_same);
        my += heights[i];
    }
    ctx->PopAxisAlignedClip();

    // 右侧滚动条 — 可拖动
    if (total > stream_h) {
        float bar_x = ax + aw - 8;
        float bar_w = 6;     // 加宽 4→6 让拖动更好命中
        float bar_track_y = stream_y + 4;
        float bar_track_h = stream_h - 8;
        float bar_h = (stream_h / total) * bar_track_h;
        if (bar_h < 28) bar_h = 28;
        float t_pos = (max_off > 0) ? (sc.offset_from_bottom / max_off) : 0;
        float bar_y = bar_track_y + (bar_track_h - bar_h) * (1.0f - t_pos);
        // track
        prim::fillRR(ctx, bar_x, bar_track_y, bar_w, bar_track_h, 3.0f,
                     br.solidA(pal.text, 0.05f));
        // thumb — hover/拖动时颜色加深
        LayoutRect bar_rect{ bar_x - 2, bar_y, bar_w + 4, bar_h };
        bool bar_hov = bar_rect.contains(g_mouse) || g_scroll_drag.active;
        prim::fillRR(ctx, bar_x, bar_y, bar_w, bar_h, 3.0f,
                     br.solidA(pal.text, bar_hov ? 0.50f : 0.30f));
        // 注册 thumb 拖动 hit
        float anchor_y = (float)g_mouse.y;
        float anchor_off = sc.offset_from_bottom;
        float track_h_capt = bar_track_h - bar_h;
        float total_capt = total;
        float vp_capt = stream_h;
        hit(bar_rect, [anchor_y, anchor_off, track_h_capt, total_capt, vp_capt](){
            g_scroll_drag.active = true;
            g_scroll_drag.anchor_mouse_y = anchor_y;
            g_scroll_drag.anchor_offset = anchor_off;
            g_scroll_drag.bar_track_h = track_h_capt;
            g_scroll_drag.total_height = total_capt;
            g_scroll_drag.viewport_h = vp_capt;
        }, true);
    }

    // composer
    paintComposer(app, ax, ay + ah - comp_h, aw, comp_h);
}

// ============== Picker ==============
// 200+ 常用 emoji — 不分类，按 group 排（Segoe UI Emoji 都能渲染）。picker 区域加滚动。
const wchar_t* kEmoji[] = {
    // 笑脸
    L"😀",L"😃",L"😄",L"😁",L"😆",L"😅",L"🤣",L"😂",L"🙂",L"🙃",
    L"😉",L"😊",L"😇",L"🥰",L"😍",L"🤩",L"😘",L"😗",L"😚",L"😙",
    L"😋",L"😛",L"😜",L"🤪",L"😝",L"🤑",L"🤗",L"🤭",L"🤫",L"🤔",
    L"🤐",L"🤨",L"😐",L"😑",L"😶",L"😏",L"😒",L"🙄",L"😬",L"🤥",
    L"😌",L"😔",L"😪",L"🤤",L"😴",L"😷",L"🤒",L"🤕",L"🤢",L"🤮",
    // 情绪
    L"🥳",L"😎",L"🤓",L"🧐",L"😕",L"😟",L"🙁",L"😮",L"😯",L"😲",
    L"😳",L"🥺",L"😦",L"😧",L"😨",L"😰",L"😥",L"😢",L"😭",L"😱",
    L"😖",L"😣",L"😞",L"😓",L"😩",L"😫",L"🥱",L"😤",L"😡",L"😠",
    L"🤬",L"😈",L"👿",L"💀",L"💩",L"🤡",L"👹",L"👺",L"👻",L"👽",
    L"👾",L"🤖",
    // 手势
    L"👍",L"👎",L"👊",L"✊",L"🤛",L"🤜",L"👏",L"🙌",L"👐",L"🤲",
    L"🤝",L"🙏",L"✌",L"🤞",L"🤟",L"🤘",L"🤙",L"👌",L"👈",L"👉",
    L"👆",L"👇",L"☝",L"✋",L"🤚",L"🖐",L"🖖",L"👋",L"💪",L"🦾",
    // 心
    L"❤",L"🧡",L"💛",L"💚",L"💙",L"💜",L"🖤",L"🤍",L"🤎",L"💔",
    L"❣",L"💕",L"💞",L"💓",L"💗",L"💖",L"💘",L"💝",L"💟",
    // 动作 / 标记
    L"💯",L"💢",L"💥",L"💫",L"💦",L"💨",L"💣",L"💬",L"💭",L"💤",
    L"🔥",L"🌟",L"⭐",L"✨",L"⚡",L"🌈",L"☀",L"🌙",L"☁",L"❄",
    // 物品 / 食物
    L"🎉",L"🎊",L"🎁",L"🎂",L"🍰",L"🍕",L"🍔",L"🍟",L"🌭",L"🍿",
    L"🍣",L"🍱",L"🍜",L"🍙",L"🍩",L"🍪",L"🍫",L"🍬",L"🍭",L"🍮",
    L"🥤",L"🍻",L"🍺",L"🍷",L"🍸",L"☕",L"🍵",L"🥛",
    // 动物
    L"🐶",L"🐱",L"🐭",L"🐹",L"🐰",L"🦊",L"🐻",L"🐼",L"🐨",L"🐯",
    L"🦁",L"🐮",L"🐷",L"🐸",L"🐵",L"🙈",L"🙉",L"🙊",L"🐒",L"🐔",
    L"🐧",L"🐤",L"🦆",L"🦅",L"🦉",L"🐺",L"🐗",
    // 游戏 / 运动
    L"🎮",L"🕹",L"🎯",L"🎲",L"🎴",L"♟",L"🎳",L"🎱",L"⚽",L"🏀",
    L"🏈",L"⚾",L"🎾",L"🏐",L"🏉",L"🚀",L"💎",L"🎵",L"🎶",L"🌸",
    L"🌹",L"🌺",L"🌻",L"🌷",L"🌴",L"🍀",
};

static void paintPicker(D2DApp& app, float anchor_x, float anchor_y) {
    if (!g_picker_open && g_picker_t.value() < 0.001f) return;
    float t = g_picker_t.value();
    if (t < 0.001f) return;

    const Palette& pal = palette();
    auto* ctx = app.ctx();
    auto& br = app.brushes();

    float pw = 330, ph = 340;
    float px = anchor_x;
    float py = anchor_y - ph - 8 + (1.0f - t) * 14.0f;
    g_picker_origin_x = px; g_picker_origin_y = py;
    g_picker_rect = { px, py, pw, ph };
    g_picker_content_rect = { px + 14, py + 50, pw - 28, ph - 62 };
    hit(g_picker_rect, [](){}, false);

    prim::drawShadow(ctx, br, px, py, pw, ph, 12.0f,
                     pal.shadow_card_hover, 0.28f * t, 2.0f, 2);
    prim::fillRR(ctx, px, py, pw, ph, 12.0f, br.solidA(pal.card, t));
    prim::strokeRR(ctx, px, py, pw, ph, 12.0f, br.solidA(pal.divider, t));

    auto* tab_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(9.5f),
                                       DWRITE_FONT_WEIGHT_BOLD);
    auto* hint_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(8.0f));

    // ===== 顶部 seg：[表情] [表情包]，右边 [↥导入] [⇣导出] [+新建] =====
    float seg_y = py + 12;
    LayoutRect tab_em{ px + 14, seg_y, 60, 26 };
    LayoutRect tab_pk{ px + 14 + 60 + 6, seg_y, 80, 26 };
    bool em_act = (g_picker_tab == 0);
    // 滑块 — 跟随 active 动画
    float pill_x = px + 14 + g_top_seg_x.value();
    float pill_w = g_top_seg_w.started ? g_top_seg_w.value() : (em_act ? 60.0f : 80.0f);
    if (pill_w > 0)
        prim::fillRR(ctx, pill_x, seg_y, pill_w, 26, 6.0f,
                     br.solidA(pal.primary, 0.18f * t));
    prim::drawText_(ctx, L"表情", tab_fmt,
                    tab_em.x, tab_em.y + 5, tab_em.w, 18,
                    br.solidA(em_act ? pal.primary : pal.text_muted, t),
                    DWRITE_TEXT_ALIGNMENT_CENTER);
    hit(tab_em, [](){
        setPickerTabSmooth(0);
    }, true);
    prim::drawText_(ctx, L"表情包", tab_fmt,
                    tab_pk.x, tab_pk.y + 5, tab_pk.w, 18,
                    br.solidA(g_picker_tab > 0 ? pal.primary : pal.text_muted, t),
                    DWRITE_TEXT_ALIGNMENT_CENTER);
    hit(tab_pk, [](){
        setPickerTabSmooth(g_picker_tab == 0 ? 1 : g_picker_tab);
    }, true);

    // 决定当前 pack 状态（用于按钮 enable / 操作目标）
    auto& packs = sticker::g_packs;
    int active_pack = -1;     // -1 = 在 emoji tab 或没 pack
    if (g_picker_tab > 0) {
        active_pack = g_picker_tab - 1;
        if (active_pack >= (int)packs.size()) active_pack = 0;
    }
    std::string cur_pid;
    if (active_pack >= 0 && active_pack < (int)packs.size()) {
        cur_pid = packs[active_pack].id;
    }

    // ===== 右上 3 按钮 =====
    auto draw_btn = [&](float bx, float by, float bw, float bh,
                        const wchar_t* label, uint32_t color, bool primary,
                        std::function<void()> on_click) {
        LayoutRect r{ bx, by, bw, bh };
        bool hov = r.contains(g_mouse);
        if (primary) {
            prim::fillRR(ctx, bx, by, bw, bh, 6.0f,
                         br.solidA(color, t * (hov ? 1.0f : 0.85f)));
        } else {
            prim::fillRR(ctx, bx, by, bw, bh, 6.0f,
                         br.solidA(color, t * (hov ? 0.18f : 0.08f)));
        }
        prim::drawText_(ctx, label, hint_fmt,
                        bx, by + 5, bw, 16,
                        br.solidA(primary ? 0xFFFFFF : color, t),
                        DWRITE_TEXT_ALIGNMENT_CENTER);
        hit(r, std::move(on_click), true);
    };
    float bw_new = 48, bw_imp = 48, bw_exp = 48;
    float bgap = 5;
    float right_btn_y = seg_y;
    float bx_new = px + pw - 14 - bw_new;
    float bx_exp = bx_new - bgap - bw_exp;
    float bx_imp = bx_exp - bgap - bw_imp;
    // [↥ 导入]
    draw_btn(bx_imp, right_btn_y, bw_imp, 26, L"导入", pal.text, false, [cur_pid](){
        if (cur_pid.empty()) {
            PostMessageW(GetActiveWindow(), WM_APP + 41, 0, 0);
        } else {
            auto* payload = new std::string(cur_pid);
            PostMessageW(GetActiveWindow(), WM_APP + 34,
                         (WPARAM)payload, 0);
        }
    });
    // [⇣ 导出]
    draw_btn(bx_exp, right_btn_y, bw_exp, 26, L"导出", pal.text, false, [cur_pid](){
        if (cur_pid.empty()) {
            PostMessageW(GetActiveWindow(), WM_APP + 41, 0, 0);
        } else {
            sticker::exportPackToFolder(GetActiveWindow(), cur_pid);
        }
    });
    // [+ 新建]
    draw_btn(bx_new, right_btn_y, bw_new, 26, L"新建", pal.primary, true, [](){
        setPickerOpen(false);
        PostMessageW(GetActiveWindow(), WM_APP + 21, 0, 0);
    });

    float content_t = g_picker_content_t.started ? g_picker_content_t.value() : 1.0f;
    float open_content_t = clampf((t - 0.22f) / 0.78f, 0.0f, 1.0f);
    float ct = t * content_t * open_content_t;
    float content_y = (1.0f - content_t) * 8.0f;

    if (g_picker_tab == 0) {
        // ===== 8 列 emoji grid + 垂直滚动 =====
        int cols = 8;
        int total_n = (int)(sizeof(kEmoji) / sizeof(kEmoji[0]));
        if ((int)g_emoji_hover_t.size() != total_n) {
            g_emoji_hover_t.assign(total_n, 0.0f);
        }
        float cell = 37.0f;
        float grid_x = px + 14, grid_y = py + 50 + content_y;
        // viewport：emoji 区高度 = picker 底部 - grid_y - 12 边距
        float view_h = (py + ph - 12) - grid_y;
        int rows_total = (total_n + cols - 1) / cols;
        float total_h = rows_total * cell;
        g_emoji_grid_h_last = view_h;
        g_emoji_total_h_last = total_h;
        // 钳 scroll
        float max_scroll = (std::max)(0.0f, total_h - view_h);
        if (g_picker_scroll_drag.active && g_picker_scroll_drag.mode == 0 && g_mouse_pressed) {
            float track_move = (std::max)(1.0f, g_picker_scroll_drag.track_h - g_picker_scroll_drag.thumb_h);
            float dy = (float)g_mouse.y - g_picker_scroll_drag.anchor_mouse_y;
            g_emoji_scroll_y = g_picker_scroll_drag.anchor_scroll_y
                + (dy / track_move) * g_picker_scroll_drag.max_scroll;
        } else if (!g_mouse_pressed) {
            g_picker_scroll_drag.active = false;
        }
        g_emoji_scroll_y = clampf(g_emoji_scroll_y, 0.0f, max_scroll);

        // clip 到 emoji 区
        ctx->PushAxisAlignedClip(D2D1::RectF(grid_x, grid_y, grid_x + cols * cell, grid_y + view_h),
                                 D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        auto* em_fmt = app.texts().format(L"Segoe UI Emoji", ptToDip(16.0f));
        int first_row = (std::max)(0, (int)(g_emoji_scroll_y / cell) - 1);
        int last_row = (std::min)(rows_total - 1, (int)((g_emoji_scroll_y + view_h) / cell) + 1);
        for (int i = first_row * cols; i < total_n && i < (last_row + 1) * cols; ++i) {
            int row = i / cols, col = i % cols;
            float ex = grid_x + col * cell;
            float ey = grid_y + row * cell - g_emoji_scroll_y;
            // 完全不可见的跳过
            if (ey + cell < grid_y) continue;
            if (ey > grid_y + view_h) break;
            LayoutRect cell_r{ ex, ey, cell, cell };
            bool hov = cell_r.contains(g_mouse);
            float& ht = g_emoji_hover_t[i];
            ht += ((hov ? 1.0f : 0.0f) - ht) * 0.24f;
            if (ht > 0.01f) {
                float inset = 3.0f - ht;
                prim::fillRR(ctx, ex + inset, ey + inset,
                             cell - inset * 2.0f, cell - inset * 2.0f,
                             7.0f, br.solidA(pal.primary, ct * (0.06f + 0.10f * ht)));
                prim::strokeRR(ctx, ex + inset, ey + inset,
                               cell - inset * 2.0f, cell - inset * 2.0f,
                               7.0f, br.solidA(pal.primary, ct * 0.20f * ht), 1.0f);
            }
            float lift = ht * 2.0f;
            float grow = ht * 2.0f;
            prim::drawText_(ctx, kEmoji[i], em_fmt,
                            ex - grow * 0.5f, ey + 4 - lift, cell + grow, cell - 4 + grow,
                            br.solidA(pal.text, ct),
                            DWRITE_TEXT_ALIGNMENT_CENTER);
            const wchar_t* e = kEmoji[i];
            hit(cell_r, [e](){
                std::wstring s = e;
                g_composer.replaceSelection(s);
                g_focus_composer = true;
                // 不自动关 picker — 用户可能要连续选
            }, true);
        }
        ctx->PopAxisAlignedClip();
        // 滚动条
        if (max_scroll > 0) {
            float bar_x = grid_x + cols * cell + 2;
            float bar_w = 6;
            float bar_h_p = (std::max)(28.0f, (view_h / total_h) * view_h);
            float bar_top = grid_y + (view_h - bar_h_p) * (g_emoji_scroll_y / max_scroll);
            LayoutRect thumb{ bar_x - 4, bar_top, bar_w + 8, bar_h_p };
            bool bar_hov = thumb.contains(g_mouse) || g_picker_scroll_drag.active;
            prim::fillRR(ctx, bar_x, grid_y, bar_w, view_h, 2.0f,
                         br.solidA(pal.text, ct * 0.05f));
            prim::fillRR(ctx, bar_x, bar_top, bar_w, bar_h_p, 2.0f,
                         br.solidA(pal.text, ct * (bar_hov ? 0.50f : 0.30f)));
            float anchor_y = (float)g_mouse.y;
            float anchor_scroll = g_emoji_scroll_y;
            hit(thumb, [anchor_y, anchor_scroll, view_h, bar_h_p, max_scroll](){
                g_picker_scroll_drag.active = true;
                g_picker_scroll_drag.mode = 0;
                g_picker_scroll_drag.anchor_mouse_y = anchor_y;
                g_picker_scroll_drag.anchor_scroll_y = anchor_scroll;
                g_picker_scroll_drag.track_h = view_h;
                g_picker_scroll_drag.thumb_h = bar_h_p;
                g_picker_scroll_drag.max_scroll = max_scroll;
            }, true);
        }
    } else {
        // ===== 表情包面板 =====
        // pack 顶部 tab 行 — 横向滑动 + 拖拽排序
        float tab_y = py + 50 + content_y;
        g_pack_tab_rects.clear();
        // pre-layout: 算每个 tab 的宽度
        std::vector<float> ws(packs.size(), 0);
        for (size_t i = 0; i < packs.size(); ++i) {
            const auto& p = packs[i];
            wchar_t buf[40]; swprintf_s(buf, L"%.10ls", p.name.c_str());
            float bw = measureW(app, buf, hint_fmt) + 16.0f;
            if (bw > 100.0f) bw = 100.0f;
            if (bw < 40.0f) bw = 40.0f;
            ws[i] = bw;
        }
        float gap_tab = 4.0f;
        std::vector<float> xs(packs.size(), 0);
        float bx = px + 14;
        for (size_t i = 0; i < packs.size(); ++i) {
            xs[i] = bx;
            bx += ws[i] + gap_tab;
        }
        // 在 g_pack_tab_rects 缓存所有 tab 位置（拖拽 / 命中）
        for (size_t i = 0; i < packs.size(); ++i) {
            g_pack_tab_rects.push_back({ {xs[i], tab_y, ws[i], 24}, (int)i });
        }
        retargetPackTab();
        // ---- 拖拽检测 + 实时重排 ----
        if (g_pack_drag.from >= 0 && g_mouse_pressed) {
            float dx = (float)g_mouse.x - g_pack_drag.start_x;
            if (!g_pack_drag.moved && std::abs(dx) > 6.0f) g_pack_drag.moved = true;
            if (g_pack_drag.moved) {
                // 找到鼠标所处的目标位置
                int target = g_pack_drag.from;
                for (size_t i = 0; i < packs.size(); ++i) {
                    if (g_mouse.x >= xs[i] && g_mouse.x <= xs[i] + ws[i]) {
                        target = (int)i;
                        break;
                    }
                }
                // 如果目标变了 → 立即在 g_packs 里 swap (本地立即响应；松手后云端保存)
                if (target != g_pack_drag.from
                    && target >= 0 && target < (int)packs.size()) {
                    std::lock_guard<std::mutex> lk(sticker::g_packs_mtx);
                    auto& v = sticker::g_packs;
                    if (g_pack_drag.from < (int)v.size() && target < (int)v.size()) {
                        sticker::Pack moving = std::move(v[g_pack_drag.from]);
                        v.erase(v.begin() + g_pack_drag.from);
                        v.insert(v.begin() + target, std::move(moving));
                        g_pack_drag.from = target;
                        g_picker_tab = 1 + target;
                        retargetPackTab();
                    }
                }
            }
        }
        // 滑块（active pill）— 用 tween 平滑
        if (!g_pack_tab_x.started && active_pack >= 0 && active_pack < (int)packs.size()) {
            g_pack_tab_x.start(xs[active_pack], xs[active_pack], 0.001f, 0, curve::easeOutQuint);
            g_pack_tab_w.start(ws[active_pack], ws[active_pack], 0.001f, 0, curve::easeOutQuint);
        }
        if (g_pack_tab_w.value() > 0.5f) {
            prim::fillRR(ctx, g_pack_tab_x.value(), tab_y,
                         g_pack_tab_w.value(), 24, 4.0f,
                         br.solidA(pal.primary, ct * 0.18f));
        }
        // 画每个 tab
        for (size_t i = 0; i < packs.size(); ++i) {
            // 拖拽中：源 tab 跟随鼠标
            float draw_x = xs[i];
            if (g_pack_drag.from == (int)i && g_pack_drag.moved) {
                draw_x = g_mouse.x - g_pack_drag.anchor_dx;
                // 不画背景 — 用纯文字 + 半透明高亮
                prim::fillRR(ctx, draw_x, tab_y, ws[i], 24, 4.0f,
                             br.solidA(pal.primary, ct * 0.30f));
            }
            wchar_t buf[40]; swprintf_s(buf, L"%.10ls", packs[i].name.c_str());
            bool pa = ((int)i == active_pack);
            uint32_t tcol = pa ? pal.primary : pal.text_muted;
            prim::drawText_(ctx, buf, hint_fmt,
                            draw_x + 4, tab_y + 5, ws[i] - 8, 16,
                            br.solidA(tcol, ct),
                            DWRITE_TEXT_ALIGNMENT_CENTER);
            int idx = (int)i;
            // 注意 hit 的是 tab 实际位置（拖动时这个 hit 跟着移）— 让点击到拖到位置上
            LayoutRect r{ draw_x, tab_y, ws[i], 24 };
            float anchor_dx = g_mouse.x - xs[i];
            hit(r, [idx, anchor_dx](){
                // 如果不是拖动结束的 click（左键单击）就切 active
                if (g_pack_drag.from == idx && g_pack_drag.moved) return;
                setPickerTabSmooth(1 + idx);
            }, true);
        }

        // ===== 当前 pack 内容 =====
        bool has_active_pack = active_pack >= 0 && active_pack < (int)packs.size();
        if (!has_active_pack) {
            g_pack_grid_h_last = 1.0f;
            g_pack_total_h_last = 0.0f;
            prim::drawText_(ctx, L"还没有表情包",
                            hint_fmt, px + 14, py + 130, pw - 28, 18,
                            br.solidA(pal.text_muted, ct),
                            DWRITE_TEXT_ALIGNMENT_CENTER);
        } else {
            const auto& cur_pack = packs[active_pack];
            // 创建人小标
            if (!cur_pack.creator_name.empty() || !cur_pack.is_owner) {
                std::wstring tip;
                if (cur_pack.is_owner) tip = L"我创建的";
                else if (!cur_pack.creator_name.empty()) tip = L"by " + cur_pack.creator_name;
                else tip = L"已安装";
                prim::drawText_(ctx, tip, hint_fmt,
                                px + 14, py + 78, pw - 28, 14,
                                br.solidA(pal.text_faint, ct));
            }
            if (cur_pack.stickers.empty()) {
                g_pack_grid_h_last = 1.0f;
                g_pack_total_h_last = 0.0f;
                prim::drawText_(ctx,
                    cur_pack.name == L"系统 emoji"
                        ? L"切到 表情 标签" : L"还没贴纸 — 拖文件 / 上传 / 安装",
                    hint_fmt,
                    px + 14, py + 130, pw - 28, 18,
                    br.solidA(pal.text_muted, ct),
                    DWRITE_TEXT_ALIGNMENT_CENTER);
            } else {
                int cols = 5;
                float cell = 56.0f;
                float gap = 5.0f;
                float gx = px + 14, gy = py + 96 + content_y;
                float view_h = (py + ph - 44.0f) - gy;
                int rows_total = ((int)cur_pack.stickers.size() + cols - 1) / cols;
                float row_h = cell + gap;
                float total_h = rows_total * row_h;
                g_pack_grid_h_last = view_h;
                g_pack_total_h_last = total_h;
                float max_scroll = (std::max)(0.0f, total_h - view_h);
                if (g_picker_scroll_drag.active && g_picker_scroll_drag.mode == 1 && g_mouse_pressed) {
                    float track_move = (std::max)(1.0f, g_picker_scroll_drag.track_h - g_picker_scroll_drag.thumb_h);
                    float dy = (float)g_mouse.y - g_picker_scroll_drag.anchor_mouse_y;
                    g_pack_scroll_y = g_picker_scroll_drag.anchor_scroll_y
                        + (dy / track_move) * g_picker_scroll_drag.max_scroll;
                } else if (!g_mouse_pressed) {
                    g_picker_scroll_drag.active = false;
                }
                g_pack_scroll_y = clampf(g_pack_scroll_y, 0.0f, max_scroll);

                ctx->PushAxisAlignedClip(D2D1::RectF(gx, gy, gx + cols * (cell + gap), gy + view_h),
                                         D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
                int first_row = (std::max)(0, (int)(g_pack_scroll_y / row_h) - 1);
                int last_row = (std::min)(rows_total - 1, (int)((g_pack_scroll_y + view_h) / row_h) + 1);
                bool decode_bitmaps = ct > 0.45f;
                for (size_t i = (size_t)(first_row * cols);
                     i < cur_pack.stickers.size() && i < (size_t)((last_row + 1) * cols); ++i) {
                    int row = (int)(i / cols), col = (int)(i % cols);
                    float ex = gx + col * (cell + gap), ey = gy + row * row_h - g_pack_scroll_y;
                    if (ey + cell < gy) continue;
                    if (ey > gy + view_h) break;
                    LayoutRect sr{ ex, ey, cell, cell };
                    bool sh_ = sr.contains(g_mouse);
                    prim::fillRR(ctx, ex, ey, cell, cell, 7.0f,
                                 br.solidA(pal.primary, ct * (sh_ ? 0.12f : 0.035f)));
                    ID2D1Bitmap* sticker_bmp = nullptr;
                    std::wstring sp = cur_pack.stickers[i];
                    auto sd = sp.find_last_of(L'.');
                    bool is_gif = (sd != std::wstring::npos
                                   && (sp.substr(sd) == L".gif"
                                       || sp.substr(sd) == L".GIF"));
                    if (decode_bitmaps) {
                        if (is_gif) {
                            auto* sa = app.gifs().fromFile(sp);
                            if (sa) sticker_bmp = app.gifs().frameAt(sa, stages::g_time_in_stage);
                        }
                        if (!sticker_bmp) sticker_bmp = app.images().fromFile(sp);
                    }
                    if (sticker_bmp) {
                        prim::pushLayerRR(ctx, app.factory(),
                                          ex + 4, ey + 4, cell - 8, cell - 8, 8.0f);
                        ctx->DrawBitmap(sticker_bmp,
                            D2D1::RectF(ex + 4, ey + 4, ex + cell - 4, ey + cell - 4),
                            ct, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                        prim::popLayer(ctx);
                    }
                    std::wstring path = cur_pack.stickers[i];
                    bool can_delete = cur_pack.is_owner;
                    auto send_sticker = [path]() {
                        Msg m;
                        auto sd2 = path.find_last_of(L'.');
                        bool is_g = (sd2 != std::wstring::npos
                                     && (path.substr(sd2) == L".gif"
                                         || path.substr(sd2) == L".GIF"));
                        m.kind = is_g ? MsgKind::Gif : MsgKind::Sticker;
                        m.from = L"me";
                        m.author = g_user.nickname;
                        m.author_key = selfAuthorKey();
                        m.status = L"online";
                        m.body = path;
                        m.time = localTimeText();
                        m.client_msg_id = makeClientMsgId();
                        m.send_state = MsgSendState::Pending;
                        std::string client_msg_id = m.client_msg_id;
                        appendLocalMessage(std::move(m));
                        sendChatMessage(GetActiveWindow(), path,
                                        is_g ? "gif" : "sticker", client_msg_id);
                        setPickerOpen(false);
                    };
                    hit(sr, send_sticker, true);
                    if (sh_ && can_delete) {
                        LayoutRect xb{ ex + cell - 18, ey + 2, 16, 16 };
                        bool xh = xb.contains(g_mouse);
                        prim::fillCircle(ctx, xb.x + 8, xb.y + 8, 8,
                                         br.solidA(0x000000, ct * (xh ? 0.85f : 0.65f)));
                        icons::drawIcon(app, icons::Name::X, xb.x + 2, xb.y + 2, 12,
                                        fadeArgb(0xFFFFFFFF, ct));
                        hit(xb, [path](){
                            auto* payload = new std::wstring(path);
                            PostMessageW(GetActiveWindow(), WM_APP + 40,
                                         (WPARAM)payload, 0);
                        }, true);
                    }
                }
                ctx->PopAxisAlignedClip();

                if (max_scroll > 0) {
                    float bar_x = px + pw - 18.0f;
                    float bar_w = 6.0f;
                    float bar_h_p = (std::max)(28.0f, (view_h / total_h) * view_h);
                    float bar_top = gy + (view_h - bar_h_p) * (g_pack_scroll_y / max_scroll);
                    LayoutRect thumb{ bar_x - 4, bar_top, bar_w + 8, bar_h_p };
                    bool bar_hov = thumb.contains(g_mouse) || (g_picker_scroll_drag.active && g_picker_scroll_drag.mode == 1);
                    prim::fillRR(ctx, bar_x, gy, bar_w, view_h, 2.0f,
                                 br.solidA(pal.text, ct * 0.05f));
                    prim::fillRR(ctx, bar_x, bar_top, bar_w, bar_h_p, 2.0f,
                                 br.solidA(pal.text, ct * (bar_hov ? 0.50f : 0.30f)));
                    float anchor_y = (float)g_mouse.y;
                    float anchor_scroll = g_pack_scroll_y;
                    hit(thumb, [anchor_y, anchor_scroll, view_h, bar_h_p, max_scroll](){
                        g_picker_scroll_drag.active = true;
                        g_picker_scroll_drag.mode = 1;
                        g_picker_scroll_drag.anchor_mouse_y = anchor_y;
                        g_picker_scroll_drag.anchor_scroll_y = anchor_scroll;
                        g_picker_scroll_drag.track_h = view_h;
                        g_picker_scroll_drag.thumb_h = bar_h_p;
                        g_picker_scroll_drag.max_scroll = max_scroll;
                    }, true);
                }
            }

        // ===== 操作行：[复制分享链接] [重命名(仅 owner)] [删除(仅 owner)] =====
        if (!cur_pack.is_system && !cur_pack.id.empty()) {
            float oy = py + ph - 36;
            std::string pid = cur_pack.id;
            std::wstring pname = cur_pack.name;
            bool is_owner = cur_pack.is_owner;

            // 分享：永远是「复制分享链接」按钮，点击 → sharePack(pid, true) → 自动复制
            LayoutRect sb{ px + 14, oy, 110, 24 };
            bool s_h = sb.contains(g_mouse);
            prim::fillRR(ctx, sb.x, sb.y, sb.w, sb.h, 4,
                         br.solidA(pal.primary, ct * (s_h ? 0.30f : 0.15f)));
            prim::drawText_(ctx, L"⧉ 复制分享链接", hint_fmt,
                            sb.x, sb.y + 5, sb.w, 16,
                            br.solidA(pal.primary, ct),
                            DWRITE_TEXT_ALIGNMENT_CENTER);
            hit(sb, [pid](){
                sticker::sharePack(GetActiveWindow(), pid, true);
            }, true);

            float bx2 = px + 14 + 110 + 6;
            if (is_owner) {
                LayoutRect rb{ bx2, oy, 64, 24 };
                bool rh = rb.contains(g_mouse);
                prim::fillRR(ctx, rb.x, rb.y, rb.w, rb.h, 4,
                             br.solidA(pal.text, ct * (rh ? 0.10f : 0.05f)));
                prim::drawText_(ctx, L"重命名", hint_fmt,
                                rb.x, rb.y + 5, rb.w, 16,
                                br.solidA(pal.text, ct),
                                DWRITE_TEXT_ALIGNMENT_CENTER);
                hit(rb, [pid, pname](){
                    setPickerOpen(false);
                    auto* payload = new PackActionPayload{ pid, pname };
                    PostMessageW(GetActiveWindow(), WM_APP + 31,
                                 (WPARAM)payload, 0);
                }, true);
                bx2 += 64 + 6;
            }
            // 删除：owner = 删自己创建的；非 owner = 卸载（uninstall）
            const wchar_t* del_lbl = is_owner ? L"删除" : L"卸载";
            LayoutRect db{ bx2, oy, 64, 24 };
            bool dh = db.contains(g_mouse);
            prim::fillRR(ctx, db.x, db.y, db.w, db.h, 4,
                         br.solidA(0xE34B4B, ct * (dh ? 0.18f : 0.08f)));
            prim::drawText_(ctx, del_lbl, hint_fmt,
                            db.x, db.y + 5, db.w, 16,
                            br.solidA(0xE34B4B, ct),
                            DWRITE_TEXT_ALIGNMENT_CENTER);
            hit(db, [pid, pname, is_owner](){
                auto* payload = new PackActionPayload{ pid, pname };
                if (is_owner) {
                    PostMessageW(GetActiveWindow(), WM_APP + 32,
                                 (WPARAM)payload, 0);
                } else {
                    PostMessageW(GetActiveWindow(), WM_APP + 51,
                                 (WPARAM)payload, 0);
                }
            }, true);
        }
        }
    }

    // 顶部固定命中层最后注册，避免滚动内容或贴纸格子吞掉 tab/按钮点击。
    hit(tab_em, [](){
        setPickerTabSmooth(0);
    }, true);
    hit(tab_pk, [](){
        setPickerTabSmooth(g_picker_tab == 0 ? 1 : g_picker_tab);
    }, true);
    hit({ bx_imp, right_btn_y, bw_imp, 26 }, [cur_pid](){
        if (cur_pid.empty()) {
            PostMessageW(GetActiveWindow(), WM_APP + 41, 0, 0);
        } else {
            auto* payload = new std::string(cur_pid);
            PostMessageW(GetActiveWindow(), WM_APP + 34,
                         (WPARAM)payload, 0);
        }
    }, true);
    hit({ bx_exp, right_btn_y, bw_exp, 26 }, [cur_pid](){
        if (cur_pid.empty()) {
            PostMessageW(GetActiveWindow(), WM_APP + 41, 0, 0);
        } else {
            sticker::exportPackToFolder(GetActiveWindow(), cur_pid);
        }
    }, true);
    hit({ bx_new, right_btn_y, bw_new, 26 }, [](){
        setPickerOpen(false);
        PostMessageW(GetActiveWindow(), WM_APP + 21, 0, 0);
    }, true);
}

void paintChatView(D2DApp& app, float ax, float ay, float aw, float ah) {
    float lw = 240.0f;
    paintChatList(app, ax, ay, lw, ah);
    paintChatPane(app, ax + lw + 1, ay, aw - lw - 1, ah);

    // picker 在 composer 上面浮起
    float comp_h = 64;
    paintPicker(app, ax + lw + 1 + 14, ay + ah - comp_h);
}

// ============== 事件 ==============
bool onMouseLDown(HWND /*hwnd*/, POINT dip) {
    if (g_composer.bounds.contains(dip)) {
        g_focus_composer = true;
        int pos = cursorFromComposerPoint((float)dip.x);
        g_composer.cursor = pos;
        g_composer.sel_anchor = pos;
        g_composer_drag.active = true;
        g_composer_drag.anchor_cursor = pos;
    } else {
        g_composer_drag.active = false;
    }
    // pack tab 拖拽起点 — 在 picker 打开 + 表情包 tab + 命中某个 tab 时初始化拖动状态
    g_pack_drag = PackDrag{};
    if (g_picker_open && g_picker_tab > 0) {
        for (auto& pt : g_pack_tab_rects) {
            if (pt.r.contains(dip)) {
                g_pack_drag.from = pt.idx;
                g_pack_drag.over = pt.idx;
                g_pack_drag.anchor_dx = (float)(dip.x - pt.r.x);
                g_pack_drag.start_x = (float)dip.x;
                g_pack_drag.moved = false;
                break;
            }
        }
    }
    bool consumed = dispatchClick(dip);
    if (g_focus_composer && !g_composer.bounds.contains(dip)) {
        g_focus_composer = false;
    }
    if (g_composer_drag.active && !g_composer.hasSelection()) {
        g_composer.clearSel();
    }
    if (g_picker_open && !consumed) {
        setPickerOpen(false);
    }
    return consumed;
}

bool onMouseMove(HWND /*hwnd*/, POINT dip) {
    if (!g_composer_drag.active || !g_mouse_pressed) return false;
    g_focus_composer = true;
    if (g_composer.sel_anchor < 0) {
        g_composer.sel_anchor = g_composer_drag.anchor_cursor;
    }
    g_composer.cursor = cursorFromComposerPoint((float)dip.x);
    return true;
}

bool onMouseRDown(HWND hwnd, POINT dip) {
    if (modal::hasBlockingModalOpen()) return true;
    // 优先：头像右键 → 用户菜单（主页 / @ / 管理）
    for (auto it = g_avatar_hits.rbegin(); it != g_avatar_hits.rend(); ++it) {
        if (it->rect.contains(dip)) {
            std::wstring label = it->peer_key;
            fetch::PeerProfile peer = fetch::peerProfileCached(it->peer_key);
            if (peer.loaded && peer.err.empty()) {
                if (!peer.nickname.empty()) label = peer.nickname;
                else if (!peer.username.empty()) label = peer.username;
                else if (!peer.uid.empty()) label = peer.uid;
            } else if (it->peer_key == selfAuthorKey()) {
                label = g_user.nickname.empty() ? g_user.username : g_user.nickname;
            }
            if (g_picker_open) setPickerOpen(false);
            modal::openUserContextMenu(dip, it->peer_key, label);
            InvalidateRect(hwnd, nullptr, FALSE);
            return true;
        }
    }
    auto& msgs = streamFor(g_active);
    auto open_msg_menu = [&](const std::vector<MsgHit>& hits) -> bool {
        for (auto it = hits.rbegin(); it != hits.rend(); ++it) {
            if (!it->rect.contains(dip)) continue;
            int idx = it->idx;
            if (idx < 0 || idx >= (int)msgs.size()) return true;
            const Msg& m = msgs[idx];
            if (m.kind == MsgKind::System || m.kind == MsgKind::DayDivider) return true;
            if (g_picker_open) setPickerOpen(false);
            modal::openMsgContextMenu(dip, idx);
            InvalidateRect(hwnd, nullptr, FALSE);
            return true;
        }
        return false;
    };
    if (open_msg_menu(g_msg_row_hits)) return true;
    if (open_msg_menu(g_msg_hits)) return true;
    modal::closeContextMenus();
    return true;
}

bool onMouseLUp(HWND /*hwnd*/, POINT /*dip*/) {
    // pack 拖拽抬起 — 真正提交顺序由 paintPicker 在拖动时即时本地排过；这里只发后端
    if (g_pack_drag.from >= 0 && g_pack_drag.moved) {
        std::vector<std::string> ids;
        {
            std::lock_guard<std::mutex> lk(sticker::g_packs_mtx);
            for (auto& p : sticker::g_packs) {
                if (!p.id.empty()) ids.push_back(p.id);
            }
        }
        sticker::reorderPacks(GetActiveWindow(), ids);
    }
    g_pack_drag = PackDrag{};
    g_composer_drag.active = false;
    g_scroll_drag.active = false;
    g_picker_scroll_drag.active = false;
    return false;
}

void onChar(HWND hwnd, wchar_t c, bool ctrl) {
    if (!g_focus_composer) return;
    g_composer.onChar(c, ctrl, hwnd);
}

void onKey(HWND hwnd, int vk, bool shift, bool ctrl) {
    if (!g_focus_composer) return;
    if (vk == VK_RETURN) {
        if (!g_composer.text.empty()) {
            sendTextMessage(hwnd, g_composer.text);
            g_composer.text.clear();
            g_composer.cursor = 0;
            g_composer.clearSel();
        }
        return;
    }
    if (vk == VK_ESCAPE) { g_focus_composer = false; return; }
    g_composer.onKey(vk, shift, ctrl);
}

// ============== 启动后异步 fetch official channels ==============
namespace {
struct OfficialArg { HWND h; };
std::mutex g_official_mtx;
std::vector<std::pair<std::wstring, std::string>> g_pending_official;  // slug → uuid
}

void fetchOfficialChannels(HWND notify) {
    if (g_session_token.empty()) return;
    auto* a = new OfficialArg{ notify };
    CreateThread(nullptr, 0, [](LPVOID lp) -> DWORD {
        std::unique_ptr<OfficialArg> a((OfficialArg*)lp);
        std::string path = "/api/chat/official?session_token=" + g_session_token;
        std::wstring wpath(path.begin(), path.end());
        auto resp = net::request(L"GET", wpath.c_str(), {}, L"");
        if (!resp.ok()) return 0;
        // 简易解析 [{"id":"...","slug":"...","title":"...","group_label":"...","write_role":"..."}]
        std::vector<std::pair<std::wstring, std::string>> tmp;
        size_t p = 0;
        while (true) {
            auto open_brace = resp.body.find('{', p);
            if (open_brace == std::string::npos) break;
            auto close_brace = net::findJsonObjectEnd(resp.body, open_brace);
            if (close_brace == std::string::npos) break;
            std::string obj = resp.body.substr(open_brace, close_brace - open_brace + 1);
            std::string id = net::jsonStr(obj, "id");
            std::string slug = net::jsonStr(obj, "slug");
            if (!id.empty() && !slug.empty()) {
                int n = MultiByteToWideChar(CP_UTF8, 0, slug.c_str(), -1, nullptr, 0);
                std::wstring wslug(n > 0 ? n - 1 : 0, 0);
                if (n > 0) MultiByteToWideChar(CP_UTF8, 0, slug.c_str(), -1, wslug.data(), n);
                tmp.emplace_back(std::move(wslug), id);
            }
            p = close_brace + 1;
        }
        {
            std::lock_guard<std::mutex> lk(g_official_mtx);
            g_pending_official = std::move(tmp);
        }
        PostMessageW(a->h, WM_APP + 5, 0, 0);
        return 0;
    }, a, 0, nullptr);
}

// 由 main thread WM_APP+5 调用
void applyOfficialResult() {
    std::lock_guard<std::mutex> lk(g_official_mtx);
    for (auto& [slug, id] : g_pending_official) {
        for (auto& c : g_channels) {
            if (slug == c.slug) { c.id = id; break; }
        }
    }
    g_pending_official.clear();
}

}  // namespace launcher::d2d::chat
