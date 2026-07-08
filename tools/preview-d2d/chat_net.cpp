// chat_net.cpp — 聊天网络层：历史拉取、发送、删除、官方频道/社区 bootstrap。
//
// 从 chat.cpp 拆出。后台线程发 HTTP（net::request/postJson），把结果塞进
// g_pending_* 队列再 PostMessage 回主线程，由 apply*Result() 合并进 streamFor。
// 仅本层使用的载荷结构（SendArg/LinkArg/OfficialArg/PendingOfficialChannel）
// 连同它们的 pending 队列保留在本文件的匿名命名空间里。共享状态
// （g_pending_history / g_history_state 等）声明见 chat_internal.h。
#include "chat.h"
#include "chat_internal.h"
#include "user_state.h"
#include "net.h"
#include "fetch.h"
#include "sticker.h"
#include "i18n.h"
#include "anim.h"

#include <algorithm>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace launcher::d2d::chat {

namespace {
// fetchHistory 后台线程的参数包（只在本文件用）。
struct HistArg { std::wstring slug; std::string chat_id; int64_t before_id = 0; HWND h; };
} // namespace

// ============== 历史拉取 ==============
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
            {   // recalled:后端 MessageOut.recalled=true → 渲染「已撤回」墓碑(不跳过)
                auto pr = obj.find("\"recalled\":");
                if (pr != std::string::npos && obj.compare(pr + 11, 4, "true") == 0)
                    m.recalled = true;
            }
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
    for (auto& existing : s) {
        normalizeMsgIdentity(existing);
    }
    auto exists = [&](Msg& candidate) -> bool {
        normalizeMsgIdentity(candidate);
        for (auto& existing : s) {
            if (sameMessageIdentity(existing, candidate)) {
                mergeServerIdentity(existing, candidate);
                return true;
            }
        }
        return false;
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

std::string makeClientMsgId() {
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

std::wstring localTimeText(time_t tt) {
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
                    it->error_text = normalizeSendErrorText(
                        la.error_text.empty() ? L"send failed" : la.error_text);
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

void sendChatMessage(HWND hwnd, const std::wstring& body, const char* kind,
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
    if (g_session_token.empty()) { fail(trW("chat.not_logged_in")); return; }
    auto* ch = activeChannel();
    if (!ch) { fail(L"channel not ready"); return; }
    if (ch->id.empty()) { fail(L"channel not ready"); return; }
    if (!canWriteChannel(ch)) { fail(activeWriteBlockedMessage()); return; }
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
        push_result(false, 0, r.body.empty() ? L"send failed" : normalizeSendErrorText(utf8wHist(r.body.substr(0, 80))));
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

// 异步撤回消息 — 调后端 chat/recall(30秒时窗,留痕迹)。本地立即标 recalled。
void recallMessage(HWND hwnd, const std::wstring& slug, int64_t server_id) {
    {
        auto& msgs = streamFor(slug);
        for (auto& m : msgs) {
            if (m.server_id == server_id) { m.recalled = true; break; }
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
        auto r = net::postJson(L"/api/chat/recall", body);
        PostMessageW(a->h, WM_APP + 68, r.ok() ? 1 : 0, 0);
        return 0;
    }, a, 0, nullptr);
}

// WS 收到别人撤回 — 在所有 stream 里找 server_id 标 recalled
void onWsMessageRecalled(int64_t server_id) {
    for (auto& kv : g_streams) {
        for (auto& m : kv.second) {
            if (m.server_id == server_id) { m.recalled = true; return; }
        }
    }
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

// ============== 已读回执 + emoji 反应 ==============
namespace {
// 每频道最后一次已经 POST 出去的 up_to_id — 单调递增去抖，避免 paint 每帧刷 POST。
std::unordered_map<std::wstring, int64_t> g_last_sent_read;
// 每频道 peer 已读位置：slug → (peer_key → last_read message id)。渲染自己消息下的"已读"标记用。
std::unordered_map<std::wstring, std::unordered_map<std::wstring, int64_t>> g_peer_read;
// react 失败回滚队列（后台线程写，WM_APP+65 主线程读）。
struct ReactFail {
    std::wstring slug;
    int64_t      server_id = 0;
    std::wstring emoji;
    bool         was_remove = false;   // 失败前乐观执行的动作：true=当时在移除，回滚=重新加回
};
std::mutex g_react_fail_mtx;
std::vector<ReactFail> g_react_fail;

// 在指定消息上应用一次反应增量。add=true 表示"加上我的反应"，false 表示"去掉我的反应"。
// touch_mine=true 时同步 mine 标记（本地乐观 / 自己的 WS 回显）；别人反应时 touch_mine=false。
void applyReactionDelta(Msg& m, const std::wstring& emoji, bool add, bool touch_mine) {
    for (auto it = m.reactions.begin(); it != m.reactions.end(); ++it) {
        if (it->emoji == emoji) {
            if (add) {
                it->count += 1;
                if (touch_mine) it->mine = true;
            } else {
                it->count -= 1;
                if (touch_mine) it->mine = false;
                if (it->count <= 0) m.reactions.erase(it);
            }
            return;
        }
    }
    if (add) {
        m.reactions.push_back(Reaction{ emoji, 1, touch_mine });
    }
}

Msg* findMsgByServerId(std::vector<Msg>& msgs, int64_t server_id) {
    for (auto& m : msgs) if (m.server_id == server_id) return &m;
    return nullptr;
}
} // namespace

// 标记已读 — fire-and-forget（clone statusSync）。解析 chat_id 同 fetchHistory。
void markRead(HWND hwnd, const std::wstring& slug, int64_t up_to_id) {
    if (up_to_id <= 0 || g_session_token.empty()) return;
    // 单调去抖：只在 tail 前进时发。
    auto& last = g_last_sent_read[slug];
    if (up_to_id <= last) return;
    std::string chat_id;
    for (auto& c : g_channels) if (c.slug == slug) { chat_id = c.id; break; }
    if (chat_id.empty()) return;
    last = up_to_id;
    struct A { std::string chat_id; int64_t up_to; };
    auto* a = new A{ chat_id, up_to_id };
    CreateThread(nullptr, 0, [](LPVOID lp) -> DWORD {
        std::unique_ptr<A> a((A*)lp);
        char idbuf[32]; sprintf_s(idbuf, "%lld", (long long)a->up_to);
        std::string body = "{\"session_token\":\"" + g_session_token
                         + "\",\"chat_id\":\"" + a->chat_id
                         + "\",\"up_to_message_id\":" + idbuf + "}";
        net::postJson(L"/api/chat/read", body);   // 204 No Content；不关心回执
        return 0;
    }, a, 0, nullptr);
    (void)hwnd;
}

// emoji 反应 — 本地乐观切换 + POST /api/chat/react（失败 WM_APP+65 回滚）。
void reactToMessage(HWND hwnd, const std::wstring& slug, int64_t server_id,
                    const std::wstring& emoji, bool remove) {
    if (server_id <= 0 || g_session_token.empty() || emoji.empty()) return;
    // 乐观切换本地聚合（add = !remove）。
    {
        auto& msgs = streamFor(slug);
        if (Msg* m = findMsgByServerId(msgs, server_id)) {
            applyReactionDelta(*m, emoji, !remove, /*touch_mine=*/true);
        }
    }
    struct A { std::wstring slug; int64_t server_id; std::wstring emoji; bool remove; HWND h; };
    auto* a = new A{ slug, server_id, emoji, remove, hwnd };
    CreateThread(nullptr, 0, [](LPVOID lp) -> DWORD {
        std::unique_ptr<A> a((A*)lp);
        char idbuf[32]; sprintf_s(idbuf, "%lld", (long long)a->server_id);
        std::string body = "{\"session_token\":\"" + g_session_token
                         + "\",\"message_id\":" + idbuf
                         + ",\"emoji\":\"" + net::jsonEscape(a->emoji) + "\""
                         + ",\"remove\":" + (a->remove ? "true" : "false") + "}";
        auto r = net::postJson(L"/api/chat/react", body);   // 成功 204 No Content
        if (!r.ok()) {
            {
                std::lock_guard<std::mutex> lk(g_react_fail_mtx);
                g_react_fail.push_back(ReactFail{ a->slug, a->server_id, a->emoji, a->remove });
            }
            PostMessageW(a->h, WM_APP + 65, 0, 0);
        }
        return 0;
    }, a, 0, nullptr);
}

// WM_APP+65 — react 失败：把乐观动作反向撤销。
void applyReactFailure() {
    std::vector<ReactFail> arr;
    {
        std::lock_guard<std::mutex> lk(g_react_fail_mtx);
        arr.swap(g_react_fail);
    }
    for (auto& f : arr) {
        auto& msgs = streamFor(f.slug);
        if (Msg* m = findMsgByServerId(msgs, f.server_id)) {
            // 乐观时 add = !was_remove；回滚 = 相反动作。
            applyReactionDelta(*m, f.emoji, /*add=*/f.was_remove, /*touch_mine=*/true);
        }
    }
}

// WS "reaction" 事件。自己的动作已在 reactToMessage 乐观处理，跳过回显避免重复计数。
void onWsReaction(int64_t message_id, const std::wstring& actor_id,
                  const std::wstring& emoji, bool remove) {
    if (message_id <= 0 || emoji.empty()) return;
    if (!g_user_id.empty() && actor_id == utf8wHist(g_user_id)) return;
    std::lock_guard<std::mutex> lk(g_streams_mtx);
    for (auto& [slug, msgs] : g_streams) {
        if (Msg* m = findMsgByServerId(msgs, message_id)) {
            applyReactionDelta(*m, emoji, /*add=*/!remove, /*touch_mine=*/false);
            return;
        }
    }
}

// WS "read" 事件 — 记录 peer 已读位置（自己的回执忽略）。
void onWsRead(const std::wstring& chat_id, const std::wstring& actor_id, int64_t up_to_message_id) {
    if (up_to_message_id <= 0 || actor_id.empty()) return;
    if (!g_user_id.empty() && actor_id == utf8wHist(g_user_id)) return;
    std::wstring slug;
    std::string chat_id_a;
    chat_id_a.reserve(chat_id.size());
    for (wchar_t c : chat_id) chat_id_a.push_back((char)c);
    for (auto& c : g_channels) if (c.id == chat_id_a) { slug = c.slug; break; }
    if (slug.empty()) return;
    auto& peers = g_peer_read[slug];
    int64_t& cur = peers[actor_id];
    if (up_to_message_id > cur) cur = up_to_message_id;   // 单调
}

// 某条自己的消息是否已被任一 peer 读过（server_id <= peer last_read）。
bool messageReadByPeer(const std::wstring& slug, int64_t server_id) {
    if (server_id <= 0) return false;
    auto it = g_peer_read.find(slug);
    if (it == g_peer_read.end()) return false;
    for (auto& [peer, last_read] : it->second) {
        if (last_read >= server_id) return true;
    }
    return false;
}

// 右键菜单 "React" → 进入选 emoji 模式并打开 picker。
void beginReactPick(const std::wstring& slug, int64_t server_id) {
    if (server_id <= 0) return;
    g_react_target.slug = slug;
    g_react_target.server_id = server_id;
    g_react_target.active = true;
    if (!g_picker_open) setPickerOpen(true);
}


// 兼容老调用名
bool sendTextMessage(HWND hwnd, const std::wstring& text) {
    if (!requireActiveChannelWrite()) return false;
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
    if (g_picker_open) setPickerOpen(false);
    return true;
}

// ============== 启动后异步 fetch official channels ==============
namespace {
struct OfficialArg { HWND h; };
std::mutex g_official_mtx;
struct PendingOfficialChannel {
    std::wstring slug;
    std::wstring name;
    std::wstring group;
    bool is_market = false;
    std::string id;
    int write_role = 0;
    bool notice = false;
    std::string write_policy;
    std::string allowed_role;
    int min_level = 1;
    int slowmode_seconds = 0;
    bool requires_subscription = false;
    bool is_readonly = false;
    bool is_locked = false;
};
std::vector<PendingOfficialChannel> g_pending_official;
bool g_pending_official_replace = false;
std::vector<AnnouncementItem> g_pending_announcements;
std::wstring g_pending_me_role;
bool g_pending_me_admin = false;
int g_pending_me_level = 1;

std::vector<std::string> jsonObjectsInArray(const std::string& arr) {
    std::vector<std::string> out;
    size_t p = 0;
    while (true) {
        auto open_brace = arr.find('{', p);
        if (open_brace == std::string::npos) break;
        auto close_brace = net::findJsonObjectEnd(arr, open_brace);
        if (close_brace == std::string::npos) break;
        out.push_back(arr.substr(open_brace, close_brace - open_brace + 1));
        p = close_brace + 1;
    }
    return out;
}

int writeRoleFromPolicy(const std::string& policy, bool readonly, bool locked) {
    if (readonly || locked) return 1;
    if (policy == "admin_only" || policy == "readonly" || policy == "locked") return 1;
    return 0;
}
}

void fetchOfficialChannels(HWND notify) {
    if (g_session_token.empty()) return;
    auto* a = new OfficialArg{ notify };
    CreateThread(nullptr, 0, [](LPVOID lp) -> DWORD {
        std::unique_ptr<OfficialArg> a((OfficialArg*)lp);
        std::vector<PendingOfficialChannel> tmp;
        std::vector<AnnouncementItem> anns;
        std::wstring me_role;
        bool me_admin = false;
        int me_level = 1;
        bool replace = false;
        {
            std::string path = "/api/community/bootstrap?session_token=" + g_session_token;
            std::wstring wpath(path.begin(), path.end());
            auto resp = net::request(L"GET", wpath.c_str(), {}, L"");
            if (resp.ok()) {
                std::string me = net::jsonRaw(resp.body, "me");
                if (!me.empty()) {
                    me_role = utf8wHist(net::jsonStr(me, "role"));
                    me_admin = net::jsonRaw(me, "is_admin") == "true";
                    me_level = (int)net::jsonInt(me, "level");
                    if (me_level < 1) me_level = 1;
                }
                std::string channels = net::jsonRaw(resp.body, "channels");
                for (const auto& obj : jsonObjectsInArray(channels)) {
                    std::string id = net::jsonStr(obj, "id");
                    std::string slug = net::jsonStr(obj, "slug");
                    if (id.empty() || slug.empty()) continue;
                    std::string title = net::jsonStr(obj, "title");
                    std::string area_name = net::jsonStr(obj, "area_name");
                    std::string area_slug = net::jsonStr(obj, "area_slug");
                    std::string policy = net::jsonStr(obj, "write_policy");
                    bool readonly = net::jsonRaw(obj, "is_readonly") == "true";
                    bool locked = net::jsonRaw(obj, "is_locked") == "true";
                    int min_level = (int)net::jsonInt(obj, "min_level");
                    int slowmode_seconds = (int)net::jsonInt(obj, "slowmode_seconds");
                    bool requires_subscription = net::jsonRaw(obj, "requires_subscription") == "true";
                    PendingOfficialChannel ch;
                    ch.slug = utf8wHist(slug);
                    ch.name = utf8wHist(title.empty() ? slug : title);
                    ch.group = utf8wHist(area_name.empty() ? area_slug : area_name);
                    if (ch.group.empty()) ch.group = L"GENERAL";
                    ch.id = id;
                    ch.is_market = slug == "market" || area_slug == "shop";
                    ch.write_role = writeRoleFromPolicy(policy, readonly, locked);
                    ch.write_policy = policy;
                    ch.allowed_role = net::jsonStr(obj, "allowed_role");
                    ch.min_level = min_level > 0 ? min_level : 1;
                    ch.slowmode_seconds = slowmode_seconds > 0 ? slowmode_seconds : 0;
                    ch.requires_subscription = requires_subscription;
                    ch.is_readonly = readonly;
                    ch.is_locked = locked;
                    tmp.push_back(std::move(ch));
                }
                std::string announcements = net::jsonRaw(resp.body, "announcements");
                for (const auto& obj : jsonObjectsInArray(announcements)) {
                    AnnouncementItem a;
                    a.id = net::jsonStr(obj, "id");
                    if (a.id.empty()) continue;
                    a.title = utf8wHist(net::jsonStr(obj, "title"));
                    a.body = utf8wHist(net::jsonStr(obj, "body"));
                    a.severity = net::jsonStr(obj, "severity");
                    a.force_popup = net::jsonRaw(obj, "force_popup") == "true";
                    a.red_dot = net::jsonRaw(obj, "red_dot") == "true";
                    a.unread = net::jsonRaw(obj, "unread") == "true";
                    a.acknowledged = net::jsonRaw(obj, "acknowledged_at") != "null"
                        && !net::jsonRaw(obj, "acknowledged_at").empty();
                    anns.push_back(std::move(a));
                }
                bool notice = false;
                for (const auto& a : anns) {
                    if (a.red_dot && a.unread) {
                        notice = true;
                        break;
                    }
                }
                for (auto& ch : tmp) {
                    if (ch.slug == L"announcements") ch.notice = notice;
                }
                replace = !tmp.empty();
            }
        }
        if (!replace) {
            std::string path = "/api/chat/official?session_token=" + g_session_token;
            std::wstring wpath(path.begin(), path.end());
            auto resp = net::request(L"GET", wpath.c_str(), {}, L"");
            if (!resp.ok()) return 0;
            for (const auto& obj : jsonObjectsInArray(resp.body)) {
                std::string id = net::jsonStr(obj, "id");
                std::string slug = net::jsonStr(obj, "slug");
                if (id.empty() || slug.empty()) continue;
                PendingOfficialChannel ch;
                ch.slug = utf8wHist(slug);
                ch.id = id;
                tmp.push_back(std::move(ch));
            }
        }
        {
            std::lock_guard<std::mutex> lk(g_official_mtx);
            g_pending_official = std::move(tmp);
            g_pending_official_replace = replace;
            g_pending_announcements = std::move(anns);
            g_pending_me_role = std::move(me_role);
            g_pending_me_admin = me_admin;
            g_pending_me_level = me_level;
        }
        PostMessageW(a->h, WM_APP + 5, 0, 0);
        return 0;
    }, a, 0, nullptr);
}

// 由 main thread WM_APP+5 调用
void applyOfficialResult() {
    std::lock_guard<std::mutex> lk(g_official_mtx);
    if (!g_pending_me_role.empty() || g_pending_official_replace) {
        g_bootstrap_role = g_pending_me_role;
        g_bootstrap_is_admin = g_pending_me_admin;
        g_user.level = g_pending_me_level;
    }
    if (g_pending_official_replace && !g_pending_official.empty()) {
        g_channel_string_pool.clear();
        g_channel_string_pool.reserve(g_pending_official.size() * 3);
        std::vector<Channel> next;
        next.reserve(g_pending_official.size());
        for (auto& p : g_pending_official) {
            g_channel_string_pool.push_back(p.slug);
            const wchar_t* slug = g_channel_string_pool.back().c_str();
            g_channel_string_pool.push_back(p.name.empty() ? p.slug : p.name);
            const wchar_t* name = g_channel_string_pool.back().c_str();
            g_channel_string_pool.push_back(p.group.empty() ? L"GENERAL" : p.group);
            const wchar_t* group = g_channel_string_pool.back().c_str();
            next.push_back(Channel{ slug, name, group, p.is_market, p.id, p.write_role, p.notice,
                                    p.write_policy, p.allowed_role, p.min_level, p.slowmode_seconds,
                                    p.requires_subscription, p.is_readonly, p.is_locked });
        }
        g_channels = std::move(next);
        bool active_ok = false;
        for (auto& c : g_channels) {
            if (g_active == c.slug) { active_ok = true; break; }
        }
        if (!active_ok && !g_channels.empty()) {
            for (auto& c : g_channels) {
                if (wcscmp(c.slug, L"general") == 0) {
                    g_active = c.slug;
                    active_ok = true;
                    break;
                }
            }
            if (!active_ok) g_active = g_channels.front().slug;
        }
    } else {
        for (auto& pending : g_pending_official) {
            for (auto& c : g_channels) {
                if (pending.slug == c.slug) { c.id = pending.id; break; }
            }
        }
    }
    if (g_pending_official_replace) {
        AnnouncementItem popup;
        bool have_popup = false;
        {
            std::lock_guard<std::mutex> alk(g_announcements_mtx);
            g_announcements = std::move(g_pending_announcements);
            g_announcements_stream_dirty = true;
            for (const auto& a : g_announcements) {
                if (a.force_popup && a.unread && !a.acknowledged) {
                    popup = a;
                    have_popup = true;
                    break;
                }
            }
        }
        if (have_popup) {
            g_popup_announcement = std::move(popup);
            g_popup_open = true;
            g_popup_t.start(g_popup_t.value(), 1.0f, 0.18f, 0, curve::easeOutCubic);
        }
        if (g_active == L"announcements") rebuildAnnouncementStream();
        syncAnnouncementNoticeOnChannels();
    }
    g_pending_official.clear();
    g_pending_announcements.clear();
    g_pending_me_role.clear();
    g_pending_me_admin = false;
    g_pending_official_replace = false;
}

} // namespace launcher::d2d::chat
