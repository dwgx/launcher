// WS 瀹㈡埛绔?鈥?net::WsClient 鏀?frame锛孭ostMessage 閫氱煡 UI 闃熷垪銆?
//
// 绠€鍖栫殑 message 澶勭悊锛?
//   * {"type":"chat","chat_id":"...","kind":"text","body":"...","from":"..."} 鈫?push 鍒?chat::streamFor
//   * {"type":"status","user_id":"...","status":"..."} 鈫?鏇存柊瀵瑰簲 user
//   * 鍏朵粬绫诲瀷鍏堟敹涓嬩笉澶勭悊

#include "ws_user.h"
#include "net.h"
#include "user_state.h"
#include "chat.h"
#include "fetch.h"
#include "i18n.h"
#include "download_pool.h"

#include <memory>
#include <deque>
#include <mutex>

namespace launcher::d2d::ws {

namespace {
net::WsClient g_ws;
std::mutex g_inbox_mtx;
std::deque<std::string> g_inbox;
HWND g_notify = nullptr;

struct PendingMediaResolve {
    std::wstring slug;
    int64_t server_id = 0;
    std::string url;
    std::wstring path;
    bool ok = false;
};

std::mutex g_media_mtx;
std::deque<PendingMediaResolve> g_media_resolved;

std::wstring utf8ToW(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(n - 1, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

bool isMediaKind(chat::MsgKind kind) {
    return kind == chat::MsgKind::Image
        || kind == chat::MsgKind::Gif
        || kind == chat::MsgKind::Video
        || kind == chat::MsgKind::Sticker;
}

std::string bodyFromPayloadObject(const std::string& payload_obj) {
    std::string text = net::jsonStr(payload_obj, "text");
    if (!text.empty()) return text;
    std::string media_url = net::jsonStr(payload_obj, "url");
    if (media_url.empty()) media_url = net::jsonStr(payload_obj, "media_url");
    if (!media_url.empty()) return media_url;
    return net::jsonStr(payload_obj, "sticker_id");
}

std::string bodyFromMessageObject(const std::string& msg_obj, std::string& media_url) {
    std::string raw = net::jsonRaw(msg_obj, "payload");
    if (raw.empty() || raw == "null") return {};
    if (raw.front() == '"') {
        std::string decoded;
        if (net::parseJsonStringAt(raw, 0, decoded)) return decoded;
        return {};
    }
    if (raw.front() == '{') {
        media_url = bodyFromPayloadObject(raw);
        return media_url;
    }
    return raw;
}

void queueMediaDownload(const std::wstring& slug, int64_t server_id, const std::string& media_url) {
    if (slug.empty() || media_url.empty()) return;
    struct A {
        std::wstring slug;
        int64_t server_id;
        std::string url;
    };
    // Wave2: 走下载池(去重合并 + 有界线程),不再每条消息起一个 CreateThread。
    // 先登记待归一记录(下载完成回调时按 local_path 匹配填 path)。
    std::wstring path = fetch::mediaCachePathForUrl(media_url, L"chat");
    {
        PendingMediaResolve r;
        r.slug = slug;
        r.server_id = server_id;
        r.url = media_url;
        r.path = path;
        r.ok = !path.empty();
        std::lock_guard<std::mutex> lk(g_media_mtx);
        g_media_resolved.push_back(std::move(r));
    }
    if (path.empty()) {
        if (g_notify) PostMessageW(g_notify, WM_APP + 10, 0, 0);
        return;
    }
    std::wstring wurl(media_url.begin(), media_url.end());
    DownloadPool::instance().enqueue(wurl, path, g_notify, WM_APP + 10);
}

void applyMediaDownloads() {
    std::deque<PendingMediaResolve> local;
    {
        std::lock_guard<std::mutex> lk(g_media_mtx);
        local.swap(g_media_resolved);
    }
    for (auto& r : local) {
        if (!r.ok || r.path.empty() || r.slug.empty()) continue;
        std::wstring remote = utf8ToW(r.url);
        auto& msgs = chat::streamFor(r.slug);
        for (auto& msg : msgs) {
            bool same_msg = (r.server_id > 0 && msg.server_id == r.server_id)
                || (r.server_id <= 0 && !remote.empty() && msg.body == remote);
            if (same_msg && isMediaKind(msg.kind)) {
                msg.body = r.path;
                break;
            }
        }
    }
}
}

// Wave2: 暴露通知窗口给下载池等待方(历史图后台下载完成后 PostMessage 重绘)。
HWND mediaNotifyHwnd() { return g_notify; }

void start(HWND notify) {
    if (g_session_token.empty()) return;
    g_notify = notify;
    std::wstring path = L"/ws/chat?session_token=";
    for (char c : g_session_token) path.push_back((wchar_t)c);
    g_ws.connect(path, [](const std::string& body) {
        {
            std::lock_guard<std::mutex> lk(g_inbox_mtx);
            g_inbox.push_back(body);
        }
        if (g_notify) PostMessageW(g_notify, WM_APP + 10, 0, 0);
    });
}

void stop() {
    g_ws.close();
}

void drain() {
    applyMediaDownloads();
    std::deque<std::string> local;
    {
        std::lock_guard<std::mutex> lk(g_inbox_mtx);
        local.swap(g_inbox);
    }
    for (auto& m : local) {
        std::string type = net::jsonStr(m, "type");
        // 鍚庣鍙戦€?{ "type": "message", "data": <MessageOut> }
        // MessageOut 瀛楁锛歩d / chat_id / sender_id / msg_type / payload / created_at / deleted
        std::string event_type = net::jsonStr(m, "event_type");
        std::string legacy_type = net::jsonStr(m, "legacy_type");
        bool is_message = type == "message"
            || (type == "event" && (event_type == "message" || legacy_type == "message"));
        bool is_deleted = type == "message_deleted" || type == "delete"
            || (type == "event" && (event_type == "message_deleted" || legacy_type == "message_deleted"));
        if (is_message) {
            std::string data = net::jsonObject(m, "data");
            const std::string& obj = data.empty() ? m : data;
            std::string chat_id = net::jsonStr(obj, "chat_id");
            std::string kind    = net::jsonStr(obj, "msg_type");
            std::string sender  = net::jsonStr(obj, "sender_id");
            int64_t mid         = net::jsonInt(obj, "id");
            if (mid <= 0) continue;
            std::string client_msg_id = net::jsonStr(obj, "client_msg_id");
            bool from_self = !g_user_id.empty() && sender == g_user_id;

            std::string media_url;
            std::string body = bodyFromMessageObject(obj, media_url);
            for (auto& c : chat::g_channels) {
                if (c.id == chat_id) {
                    chat::Msg msg;
                    if (from_self) {
                        msg.from = L"me";
                        msg.author_key = utf8ToW(g_user_id);
                        msg.author = g_user.nickname;
                    } else {
                        msg.peer_key = utf8ToW(sender);
                        msg.author_key = msg.peer_key;
                        msg.from = sender.size() > 8 ? utf8ToW(sender.substr(0, 8)) : utf8ToW(sender);
                        msg.author = utf8ToW(net::jsonStr(obj, "sender_nickname"));
                        if (msg.author.empty()) msg.author = utf8ToW(net::jsonStr(obj, "sender_username"));
                        if (msg.author.empty()) msg.author = utf8ToW(net::jsonStr(obj, "sender_uid"));
                    }
                    msg.body = utf8ToW(body);
                    msg.reply_to_id = net::jsonInt(obj, "reply_to_id");
                    std::string reply = net::jsonObject(obj, "reply_snapshot");
                    if (!reply.empty()) {
                        msg.reply_author = utf8ToW(net::jsonStr(reply, "sender_nickname"));
                        if (msg.reply_author.empty()) msg.reply_author = utf8ToW(net::jsonStr(reply, "sender_username"));
                        if (msg.reply_author.empty()) msg.reply_author = utf8ToW(net::jsonStr(reply, "sender_uid"));
                        msg.reply_preview = utf8ToW(net::jsonStr(reply, "preview"));
                    }
                    int64_t created_at = net::jsonInt(obj, "created_at");
                    if (created_at <= 0) created_at = net::jsonInt(m, "server_time");
                    if (created_at > 0) {
                        time_t tt = (time_t)created_at;
                        tm local_tm{};
                        localtime_s(&local_tm, &tt);
                        wchar_t tb[16]{};
                        wcsftime(tb, 16, L"%H:%M", &local_tm);
                        msg.time = tb;
                    } else {
                        msg.time = L"now";
                    }
                    msg.status = L"online";
                    msg.server_id = mid;
                    msg.client_msg_id = client_msg_id;
                    if      (kind == "sticker")    msg.kind = chat::MsgKind::Sticker;
                    else if (kind == "image")      msg.kind = chat::MsgKind::Image;
                    else if (kind == "gif")        msg.kind = chat::MsgKind::Gif;
                    else if (kind == "video")      msg.kind = chat::MsgKind::Video;
                    else if (kind == "system")     msg.kind = chat::MsgKind::System;
                    else if (kind == "pack_share") {
                        msg.kind = chat::MsgKind::Text;
                        if (!msg.body.empty() && msg.body.find(L"launcher://") == std::wstring::npos) {
                            msg.body = L"launcher://pack/" + msg.body;
                        }
                    }
                    else                           msg.kind = chat::MsgKind::Text;
                    if (isMediaKind(msg.kind) && !media_url.empty()) {
                        queueMediaDownload(c.slug, mid, media_url);
                    }
                    chat::appendOrMergeMessage(c.slug, std::move(msg));
                    break;
                }
            }
        } else if (is_deleted) {
            int64_t mid = net::jsonInt(m, "message_id");
            if (mid > 0) chat::onWsMessageDeleted(mid);
        } else if (type == "event" && (event_type == "message_recalled" || legacy_type == "message_recalled")) {
            int64_t mid = net::jsonInt(m, "message_id");
            if (mid > 0) { chat::onWsMessageRecalled(mid); if (g_notify) PostMessageW(g_notify, WM_APP + 10, 0, 0); }
        } else if (type == "event" && (event_type == "member_muted" || event_type == "member_unmuted")) {
            std::string chat_id = net::jsonStr(m, "chat_id");
            std::string data = net::jsonObject(m, "data");
            std::string target = net::jsonObject(data, "target");
            std::string moderator = net::jsonObject(data, "moderator");
            auto label_from = [](const std::string& obj) -> std::wstring {
                std::wstring s = utf8ToW(net::jsonStr(obj, "nickname"));
                if (s.empty()) s = utf8ToW(net::jsonStr(obj, "username"));
                if (s.empty()) s = utf8ToW(net::jsonStr(obj, "uid"));
                if (s.empty()) s = utf8ToW(net::jsonStr(obj, "user_id"));
                return s;
            };
            std::wstring target_label = label_from(target);
            std::wstring mod_label = label_from(moderator);
            std::wstring reason = utf8ToW(net::jsonStr(data, "reason"));
            std::wstring duration = utf8ToW(net::jsonStr(data, "duration_label"));
            std::wstring line;
            if (event_type == "member_muted") {
                line = trW("ws.muted");
                if (auto pt = line.find(L"{target}"); pt != std::wstring::npos)
                    line.replace(pt, 8, target_label);
                if (auto pm = line.find(L"{mod}"); pm != std::wstring::npos)
                    line.replace(pm, 5, mod_label);
                if (!reason.empty()) {
                    std::wstring r = trW("ws.mute_reason");
                    if (auto pr = r.find(L"{reason}"); pr != std::wstring::npos)
                        r.replace(pr, 8, reason);
                    line += r;
                }
                if (!duration.empty()) {
                    std::wstring d = trW("ws.mute_duration");
                    if (auto pd = d.find(L"{duration}"); pd != std::wstring::npos)
                        d.replace(pd, 10, duration);
                    line += d;
                }
            } else {
                line = trW("ws.unmuted");
                if (auto pt = line.find(L"{target}"); pt != std::wstring::npos)
                    line.replace(pt, 8, target_label);
            }
            for (auto& c : chat::g_channels) {
                if (c.id == chat_id) {
                    chat::Msg msg;
                    msg.kind = chat::MsgKind::System;
                    msg.body = line;
                    msg.time = L"now";
                    chat::appendLocalMessage(std::move(msg));
                    break;
                }
            }
        } else if (type == "status") {
            std::string user_id = net::jsonStr(m, "user_id");
            std::string status = net::jsonStr(m, "status");
            if (!user_id.empty() && !status.empty()) {
                fetch::updatePeerStatus(utf8ToW(user_id), utf8ToW(status));
            }
        } else if (type == "event" && (event_type == "reaction" || legacy_type == "reaction")) {
            // { event_type:"reaction", message_id, actor_id, data:{emoji, remove} }
            int64_t message_id = net::jsonInt(m, "message_id");
            std::string actor = net::jsonStr(m, "actor_id");
            std::string data = net::jsonObject(m, "data");
            std::string emoji = net::jsonStr(data, "emoji");
            bool remove = net::jsonRaw(data, "remove") == "true";
            if (message_id > 0 && !emoji.empty()) {
                chat::onWsReaction(message_id, utf8ToW(actor), utf8ToW(emoji), remove);
            }
        } else if (type == "event" && (event_type == "read" || legacy_type == "read")) {
            // { event_type:"read", chat_id, actor_id, data:{up_to_message_id} }
            std::string chat_id = net::jsonStr(m, "chat_id");
            std::string actor = net::jsonStr(m, "actor_id");
            std::string data = net::jsonObject(m, "data");
            int64_t up_to = net::jsonInt(data, "up_to_message_id");
            if (up_to <= 0) up_to = net::jsonInt(m, "message_id");
            if (!chat_id.empty() && up_to > 0) {
                chat::onWsRead(utf8ToW(chat_id), utf8ToW(actor), up_to);
            }
        }
    }
}

}  // namespace launcher::d2d::ws
