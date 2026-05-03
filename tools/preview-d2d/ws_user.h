// WS 客户端 wrapper — 跑一个 WsClient 收 /ws/chat 推送，在 UI 线程消费。
//
// GDI+ 等价 chat_view.inl::WsInbox + wsClient()。
#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <string>
#include <mutex>
#include <vector>

namespace launcher::d2d::ws {

// 启动 WS — 登录成功后调
void start(HWND notify);

// 关闭 WS — 退出登录 / 程序结束时调
void stop();

// main thread 收到 WM_APP+10 后调，drain 所有待处理消息
void drain();

}  // namespace launcher::d2d::ws
