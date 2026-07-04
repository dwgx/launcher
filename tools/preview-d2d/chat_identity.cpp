// chat_identity.cpp — 消息身份归一 / 去重 / 作者显示名解析 / 回复快照。
//
// 从 chat.cpp 拆出。这是聊天数据模型的核心判定层：把后端消息对象解出
// 文本(bodyFrom*)、把 author_key/peer_key 归一(normalizeMsgIdentity)、
// 判定两条消息是否同一条(sameMessageIdentity[Strict])、合并本地乐观消息与
// 服务器回执(mergeServerIdentity)、维护回复预览快照(rememberReplySnapshot/
// fillReplySnapshot)。状态 g_reply_snapshots / g_peer_profile_requested_at
// 定义在 chat_state.cpp，声明见 chat_internal.h。
#include "chat.h"
#include "chat_internal.h"
#include "user_state.h"
#include "net.h"
#include "fetch.h"
#include "download_pool.h"
#include "ws_user.h"

#include <limits>
#include <mutex>
#include <string>

namespace launcher::d2d::chat {

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
    // Wave2: 不在 parse 线程同步下载。算出本地缓存路径立即返回；
    // 若文件已在磁盘(热路径)直接用,否则入队后台下载池(去重合并),
    // 下载完成 PostMessage 触发重绘,Wave1 的解码占位符会在期间顶着。
    std::wstring path = launcher::d2d::fetch::mediaCachePathForUrl(media_url, L"chat");
    if (path.empty()) return utf8wHist(media_url);
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        std::wstring wurl(media_url.begin(), media_url.end());
        launcher::d2d::DownloadPool::instance().enqueue(
            wurl, path, launcher::d2d::ws::mediaNotifyHwnd(), WM_APP + 10);
    }
    return path;
}

std::wstring bodyFromPayloadObject(const std::string& payload_obj) {
    std::string text = net::jsonStr(payload_obj, "text");
    if (!text.empty()) return utf8wHist(text);
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
    if (a.server_id > 0 && b.server_id > 0) return a.server_id == b.server_id;
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
    if (server.server_id > 0) {
        local.from = server.from;
        local.author_key = server.author_key;
        local.peer_key = server.peer_key;
        local.author = server.author;
        local.status = server.status;
        local.kind = server.kind;
        local.body = server.body;
        local.time = server.time;
        local.reply_to_id = server.reply_to_id;
        local.reply_author = server.reply_author;
        local.reply_preview = server.reply_preview;
        local.send_state = MsgSendState::Sent;
        local.error_text.clear();
        return;
    }
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

} // namespace launcher::d2d::chat
