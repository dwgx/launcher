// Chat view — 1:1 复刻 tools/preview/chat_view.inl 关键骨架。
// 简化掉：picker / sticker pack / GIF/video bubble / 头像下载 / WS（留下一轮接通）。
// 保留：8 官方频道写死 / channel list 分组折叠 / text bubble + day/system / composer
//       (含选区 + 光标 + Ctrl+ACVX) + 真后端发消息（POST /api/chat/send）。
#pragma once

#include "d2d_app.h"
#include "inputbox.h"
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
};

extern std::vector<Channel> g_channels;
extern std::wstring g_active;        // 当前频道 slug
extern InputBox     g_composer;
extern bool         g_focus_composer;

enum class MsgKind { Text, System, DayDivider };

struct Msg {
    MsgKind kind = MsgKind::Text;
    std::wstring from;        // "me" 表示自己
    std::wstring author;
    std::wstring status;
    std::wstring body;
    std::wstring time;
};

std::vector<Msg>& streamFor(const std::wstring& slug);
void switchChannel(const std::wstring& slug);

// 顶层 paint — 分两半：list 240 / pane 1fr
void paintChatView(D2DApp& app, float ax, float ay, float aw, float ah);

void tick(float dt);

bool onMouseLDown(HWND hwnd, POINT dip);
void onChar(HWND hwnd, wchar_t c, bool ctrl);
void onKey(HWND hwnd, int vk, bool shift, bool ctrl);

// 启动后异步拉 GET /api/chat/official 把 slug → id 填入 g_channels
void fetchOfficialChannels(HWND notify);

// main thread WM_APP+5 调
void applyOfficialResult();

}  // namespace launcher::d2d::chat
