// Auth view — 1:1 复刻 tools/preview/loading_demo.cpp:2477 paintAuthView。
// 380 卡片 + 浮动 label + halo + 渐变 glow 按钮 + Login/Register 切换 + 登录成功打勾覆盖。
#pragma once

#include "d2d_app.h"
#include "inputbox.h"
#include "stages.h"   // for AuthMode

namespace launcher::d2d::auth {

struct Form {
    InputBox username, password, invite;
    int  focus{0};            // 0=username 1=password 2=invite
    bool busy{false};
    std::wstring error_msg;
};
extern Form g_form;

// 当前 AuthMode 跟 stages 那边共用（同一个状态机）
using stages::AuthMode;

// 完整 paint — Stage::Auth / Stage::ShrinkSuccess / Stage::ExpandMain (非 auto-login) 调
void paintAuthView(D2DApp& app, float W, float H);

// WM_APP+1 触发；起后台线程发 POST /api/auth/login 或 /api/auth/register。
void handleSubmit(HWND hwnd);

// WM_APP+2 触发；后台线程结束后由主线程调，wp = 1 成功 / 0 失败。
void onSubmitResult(HWND hwnd, bool success);

// AuthMode 切换（Login ↔ Register）— 重置表单 + 重启入场 tween
void switchMode();

// 鼠标 / 键盘事件路由 — Stage::Auth 时由 WndProc 调
bool onMouseLDown(HWND hwnd, POINT dip);
void onChar(HWND hwnd, wchar_t c, bool ctrl);
void onKey(HWND hwnd, int vk, bool shift, bool ctrl);

}  // namespace launcher::d2d::auth
