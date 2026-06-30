// chat_announcements.cpp — 公告子系统（数据流构建 + 已读/确认上报 + 频道红点同步）。
//
// 从 chat.cpp 拆出。状态 g_announcements 等定义在 chat_state.cpp，
// 声明见 chat_internal.h。paintAnnouncementModal 的渲染留在 chat_render.cpp。
#include "chat.h"
#include "chat_internal.h"
#include "user_state.h"
#include "net.h"
#include "i18n.h"

#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace launcher::d2d::chat {

void syncAnnouncementNoticeOnChannels() {
    bool notice = false;
    {
        std::lock_guard<std::mutex> lk(g_announcements_mtx);
        for (const auto& a : g_announcements) {
            if (a.red_dot && a.unread) {
                notice = true;
                break;
            }
        }
    }
    for (auto& c : g_channels) {
        if (wcscmp(c.slug, L"announcements") == 0) c.notice = notice;
    }
}

std::wstring announcementSeverityLabel(const std::string& severity) {
    if (severity == "critical") return trW("announce.severity_critical");
    if (severity == "important") return trW("announce.label");
    return trW("announce.label");
}

void rebuildAnnouncementStream() {
    std::vector<AnnouncementItem> anns;
    {
        std::lock_guard<std::mutex> lk(g_announcements_mtx);
        anns = g_announcements;
    }

    auto& stream = streamFor(L"announcements");
    stream.clear();
    if (anns.empty()) {
        Msg empty;
        empty.kind = MsgKind::System;
        empty.body = trW("announce.empty");
        stream.push_back(std::move(empty));
        g_announcements_stream_dirty = false;
        return;
    }

    for (const auto& a : anns) {
        Msg header;
        header.kind = MsgKind::System;
        header.body = announcementSeverityLabel(a.severity);
        if (a.unread && a.red_dot) header.body += trW("announce.unread_suffix");
        stream.push_back(std::move(header));

        Msg msg;
        msg.kind = MsgKind::Text;
        msg.from = L"system";
        msg.author = L"Launcher Server";
        msg.author_key = L"announcements";
        msg.peer_key = L"announcements";
        msg.status = L"official";
        msg.time = localTimeText();
        msg.body = a.title.empty() ? trW("announce.label") : a.title;
        if (!a.body.empty()) {
            msg.body += L"\n";
            msg.body += a.body;
        }
        stream.push_back(std::move(msg));
    }
    g_announcements_stream_dirty = false;
}

bool hasUnreadAnnouncements() {
    std::lock_guard<std::mutex> lk(g_announcements_mtx);
    for (const auto& a : g_announcements) {
        if (a.red_dot && a.unread) return true;
    }
    return false;
}

void postAnnouncementMark(const std::string& id, bool ack) {
    if (id.empty() || g_session_token.empty()) return;
    struct Arg { std::string id; bool ack; };
    auto* a = new Arg{ id, ack };
    CreateThread(nullptr, 0, [](LPVOID lp) -> DWORD {
        std::unique_ptr<Arg> a((Arg*)lp);
        std::wstring path = L"/api/announcements/";
        for (char c : a->id) path.push_back((wchar_t)c);
        path += a->ack ? L"/ack" : L"/read";
        std::string body = "{\"session_token\":\"" + g_session_token + "\"}";
        net::postJson(path.c_str(), body);
        return 0;
    }, a, 0, nullptr);
}

void markAnnouncementsReadLocal(bool ack_popup, const std::string& only_id) {
    std::vector<std::string> ids_to_post;
    {
        std::lock_guard<std::mutex> lk(g_announcements_mtx);
        for (auto& a : g_announcements) {
            if (!only_id.empty() && a.id != only_id) continue;
            if (a.unread || (ack_popup && !a.acknowledged)) {
                ids_to_post.push_back(a.id);
            }
            a.unread = false;
            if (ack_popup) a.acknowledged = true;
        }
    }
    if (!ids_to_post.empty()) g_announcements_stream_dirty = true;
    for (const auto& id : ids_to_post) postAnnouncementMark(id, ack_popup);
    syncAnnouncementNoticeOnChannels();
}

} // namespace launcher::d2d::chat
