// Modals — 简化版本：ChangePw / Confirm / CS2 / History。
// 完整 GDI+ 等价见 tools/preview/modals.inl 980 行。
//
// 简化范围：
//   * ChangePw：3 InputBox + 提交，接 POST /api/profile/password
//   * Confirm：title + msg + 双按钮
//   * CS2：游戏详情卡 + 启动按钮（占位 — 真启动 Steam 集成留下一轮）
//   * History：登录历史 modal（占位 — 拉 /api/profile/login-history 留下一轮）
#pragma once

#include "d2d_app.h"
#include "inputbox.h"
#include "anim.h"
#include <functional>

namespace launcher::d2d::modal {

struct ChangePwState {
    InputBox old_pw, new_pw, repeat_pw;
    int  focus = 0;
    bool busy = false;
    std::wstring error_msg;
    bool open = false;
    Tween t;          // fade in tween
};
extern ChangePwState g_change_pw;
void openChangePw();
void paintChangePwModal(D2DApp& app, float W, float H);
// WM_APP+4 触发；后台线程结束后由主线程调
void onChangePwResult(bool success);

struct ConfirmState {
    bool open = false;
    Tween t;
    std::wstring title;
    std::wstring msg;
    std::wstring yes_label;
    std::wstring no_label;
    std::function<void()> on_yes;
    bool danger = false;
};
extern ConfirmState g_confirm;
void openConfirm(const std::wstring& title, const std::wstring& msg,
                 std::function<void()> on_yes,
                 const std::wstring& yes_label = L"确定",
                 const std::wstring& no_label = L"取消",
                 bool danger = false);
void paintConfirmModal(D2DApp& app, float W, float H);

struct CS2State {
    bool open = false;
    Tween t;
};
extern CS2State g_cs2;
void openCS2();
void paintCS2Modal(D2DApp& app, float W, float H);

struct HistoryState {
    bool open = false;
    Tween t;
};
extern HistoryState g_history;
void openHistory();
void paintHistoryModal(D2DApp& app, float W, float H);

// 主帧循环 tick
void tickAll(float dt);

// 任意 modal 在显示
bool anyOpen();

// 鼠标 / 键盘事件路由：return true = 已处理（modal 吃掉事件）
bool onMouseLDown(HWND hwnd, POINT dip);
bool onChar(HWND hwnd, wchar_t c, bool ctrl);
bool onKey(HWND hwnd, int vk, bool shift, bool ctrl);

}  // namespace launcher::d2d::modal
