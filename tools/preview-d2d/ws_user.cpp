// WS 客户端 — net::WsClient 收 frame，PostMessage 通知 UI 队列。
//
// 简化的 message 处理：
//   * {"type":"chat","chat_id":"...","kind":"text","body":"...","from":"..."} → push 到 chat::streamFor
//   * {"type":"status","user_id":"...","status":"..."} → 更新对应 user
//   * 其他类型先收下不处理

#include "ws_user.h"
#include "net.h"
#include "user_state.h"
#include "chat.h"

#include <memory>
#include <deque>

namespace launcher::d2d::ws {

namespace {
net::WsClient g_ws;
std::mutex g_inbox_mtx;
std::deque<std::string> g_inbox;
HWND g_notify = nullptr;

std::wstring utf8ToW(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(n - 1, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}
}

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
    std::deque<std::string> local;
    {
        std::lock_guard<std::mutex> lk(g_inbox_mtx);
        local.swap(g_inbox);
    }
    for (auto& m : local) {
        std::string type = net::jsonStr(m, "type");
        // 后端发送 { "type": "message", "data": <MessageOut> }
        // MessageOut 字段：id / chat_id / sender_id / msg_type / payload / created_at / deleted
        std::string event_type = net::jsonStr(m, "event_type");
        std::string legacy_type = net::jsonStr(m, "legacy_type");
        bool is_message = type == "message"
            || (type == "event" && (event_type == "message" || legacy_type == "message"));
        bool is_deleted = type == "message_deleted" || type == "delete"
            || (type == "event" && (event_type == "message_deleted" || legacy_type == "message_deleted"));
        if (is_message) {
            // data 是 nested object — 在外层 m 里直接搜字段也行，因为 net::jsonStr 找到第一个匹配
            std::string chat_id = net::jsonStr(m, "chat_id");
            std::string kind    = net::jsonStr(m, "msg_type");
            std::string sender  = net::jsonStr(m, "sender_id");
            int64_t mid         = net::jsonInt(m, "id");
            // 自己发出的消息已经本地 push 过 — 跳过 echo
            if (!g_user_id.empty() && sender == g_user_id) {
                // 但要把 server_id 绑回去 — 简单做法：依赖 chat::applySendResult 已绑（POST/send 响应也含 id）
                continue;
            }
            // payload 解析 — 可能是字符串或对象
            std::string body;
            {
                auto pp = m.find("\"payload\":");
                if (pp != std::string::npos) {
                    pp += 10;
                    while (pp < m.size() && (m[pp] == ' ' || m[pp] == '\t')) ++pp;
                    if (pp < m.size() && m[pp] == '"') {
                        ++pp;
                        while (pp < m.size() && m[pp] != '"') {
                            if (m[pp] == '\\' && pp + 1 < m.size()) {
                                char nc = m[pp + 1];
                                if (nc == 'n') body.push_back('\n');
                                else body.push_back(nc);
                                pp += 2;
                            } else {
                                body.push_back(m[pp]); ++pp;
                            }
                        }
                    } else if (pp < m.size() && m[pp] == '{') {
                        body = net::jsonStr(m.substr(pp), "url");
                        if (body.empty()) body = net::jsonStr(m.substr(pp), "media_url");
                    }
                }
            }
            for (auto& c : chat::g_channels) {
                if (c.id == chat_id) {
                    chat::Msg msg;
                    std::string sender_short = sender.size() > 8 ? sender.substr(0, 8) : sender;
                    msg.peer_key = utf8ToW(sender);
                    msg.from = utf8ToW(sender_short);
                    msg.author = msg.from;
                    msg.body = utf8ToW(body);
                    int64_t created_at = net::jsonInt(m, "created_at");
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
                    if      (kind == "sticker")    msg.kind = chat::MsgKind::Sticker;
                    else if (kind == "image")      msg.kind = chat::MsgKind::Image;
                    else if (kind == "gif")        msg.kind = chat::MsgKind::Gif;
                    else if (kind == "video")      msg.kind = chat::MsgKind::Video;
                    else if (kind == "system")     msg.kind = chat::MsgKind::System;
                    else if (kind == "pack_share") {
                        msg.kind = chat::MsgKind::Text;
                        // pack_share payload 通常含 short_name；展示成 launcher://pack/<short>
                        // 让 chat 的 link 检测渲染成预览卡片
                        if (!msg.body.empty() && msg.body.find(L"launcher://") == std::wstring::npos) {
                            msg.body = L"launcher://pack/" + msg.body;
                        }
                    }
                    else                           msg.kind = chat::MsgKind::Text;
                    chat::streamFor(c.slug).push_back(std::move(msg));
                    break;
                }
            }
        } else if (is_deleted) {
            int64_t mid = net::jsonInt(m, "message_id");
            if (mid > 0) chat::onWsMessageDeleted(mid);
        }
        // type == "status" 留扩展
    }
}

}  // namespace launcher::d2d::ws
