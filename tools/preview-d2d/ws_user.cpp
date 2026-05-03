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
        // 简单解析 {"type":"chat",...}
        std::string type = net::jsonStr(m, "type");
        if (type == "chat") {
            std::string chat_id = net::jsonStr(m, "chat_id");
            std::string kind = net::jsonStr(m, "kind");
            std::string body = net::jsonStr(m, "body");
            std::string from = net::jsonStr(m, "from");
            std::string author = net::jsonStr(m, "author");
            // 找 chat_id 对应的 slug
            for (auto& c : chat::g_channels) {
                if (c.id == chat_id) {
                    chat::Msg msg;
                    msg.kind = chat::MsgKind::Text;
                    msg.from = utf8ToW(from);
                    msg.author = utf8ToW(author);
                    msg.body = utf8ToW(body);
                    msg.time = L"now";
                    msg.status = L"online";
                    chat::streamFor(c.slug).push_back(std::move(msg));
                    break;
                }
            }
        }
        // type == "status" 等留扩展
    }
}

}  // namespace launcher::d2d::ws
