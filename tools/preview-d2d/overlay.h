// 统一浮层栈 —— 单一所有者,取代早期并存的多套命中测试机制,
// 以及账号下拉/公告弹窗的全窗背景吞击。
//
// 核心思想:paint 顺序已经正确编码了视觉 z-order(dropdown→modals→menus→toast,
// 自底向上)。bug 从来不是 z-order 错,而是"派发不一致地使用它"。所以本栈**每帧
// 在 paint 时按 paint 顺序重建**:每个 overlay 在自己 paint 出卡片/菜单矩形处调用
// g_overlays.add(...) 记下矩形 + 策略。派发(左键/右键/滚轮/ESC/NCHITTEST)全部只
// 查这一个栈,栈顶(最后 add 的)即最上层。
//
// 加一个浮层 = 在其 paint 处加一行 add(),不再需要同时改多处
// (open布尔/close函数/closeOpenOverlay优先级链/anyOpen/onChar/onKey/onMouseRDown)。
#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <functional>
#include <vector>
#include "hit.h"

namespace launcher::d2d {

// 浮层语义类别 —— 决定默认派发策略
enum class OverlayKind {
    Modal,      // 阻塞式对话框:吞滚轮、dim 背景不可拖窗、点外关(除 Confirm)
    Menu,       // 右键上下文菜单 / More 菜单:不吞滚轮、点外关、右键也 top-only
    Popover,    // 表情选择器等:吞滚轮由 blocks_wheel 决定、点外关
    Dropdown,   // 账号下拉:不吞滚轮、点外关、无 dim
    Popup,      // 公告弹窗:吞滚轮、dim、但仅按钮/ESC 关(dismiss_on_outside=false)
};

// 一个浮层在某一帧的注册项。每帧重建,不持久化。
struct OverlayEntry {
    int   id = 0;                 // 稳定标识(OverlayId 枚举),用于 top()==x 判定
    OverlayKind kind = OverlayKind::Modal;
    LayoutRect rect{};            // 卡片/菜单矩形(命中判定用)
    bool  dismiss_on_outside = true;  // 点外是否关闭(Confirm/Popup=false)
    bool  blocks_wheel = true;        // 打开时是否吞掉滚轮(Modal/Popup=true)
    bool  blocks_drag_bg = true;      // dim/背景区是否禁止拖窗(NCHITTEST,修 C4)
    bool  owns_child_hwnd = false;    // webview:点击交给子 HWND,不走矩形派发
    // picker 专用:点在其矩形"内"时,不由栈的 dispatchClick 处理,而是返回 false
    // 让 WndProc 继续调 chat::onMouseLDown(它负责 composer 聚焦 / pack tab 拖拽起点)。
    // 点"外"仍由栈关闭并吞掉。这样既修 C5(表情按钮 toggle)又修 C7(pack 拖拽初始化)。
    bool  defer_inside_to_caller = false;
    std::function<void()> dismiss;    // 关闭回调(调对应 closeXxx)
};

// 稳定 overlay 标识。值无所谓,只要唯一。
enum OverlayId {
    OV_NONE = 0,
    OV_CHANGE_PW, OV_CONFIRM, OV_CS2, OV_MARKET_DETAIL, OV_HISTORY,
    OV_ADDTAG, OV_CREATEPACK, OV_RENAMEPACK, OV_USER_PROFILE,
    OV_EDIT_STATUS, OV_EDIT_BIO, OV_EDIT_NICKNAME, OV_MUTE_USER,
    OV_PACK_PREVIEW, OV_SEARCH, OV_WEBVIEW,
    OV_MSG_MENU, OV_USER_MENU, OV_CHAT_MORE,
    OV_PICKER, OV_DROPDOWN, OV_ANNOUNCEMENT,
};

// 每帧重建的浮层栈。paint 顺序 = 入栈顺序 = z-order(后入者在上)。
class OverlayStack {
public:
    void clear() { entries_.clear(); }

    // 各 overlay 在 paint 自己矩形处调用。只在 open 时调(调用方已判 open)。
    void add(const OverlayEntry& e) { entries_.push_back(e); }
    void add(int id, OverlayKind kind, LayoutRect rect,
             std::function<void()> dismiss,
             bool dismiss_on_outside = true,
             bool blocks_wheel = true,
             bool blocks_drag_bg = true,
             bool owns_child_hwnd = false,
             bool defer_inside_to_caller = false) {
        entries_.push_back(OverlayEntry{ id, kind, rect, dismiss_on_outside,
                                         blocks_wheel, blocks_drag_bg,
                                         owns_child_hwnd, defer_inside_to_caller,
                                         std::move(dismiss) });
    }

    bool empty()   const { return entries_.empty(); }
    size_t size()  const { return entries_.size(); }


    // 有阻塞式浮层(Modal/Popup)打开 —— 取代 hasBlockingModalOpen。
    bool anyBlocking() const {
        for (auto& e : entries_)
            if (e.kind == OverlayKind::Modal || e.kind == OverlayKind::Popup)
                return true;
        return false;
    }

    // 打开的浮层里有吞滚轮的 —— WM_MOUSEWHEEL gate。
    bool blocksWheel() const {
        for (auto& e : entries_) if (e.blocks_wheel) return true;
        return false;
    }

    // NCHITTEST:点是否落在"禁止拖窗"的浮层(卡片 or 其 dim 背景)。
    // 关键(修菜单点外关不掉的真因):任一"点外可关"的浮层(菜单/下拉/模态)打开时,
    // 整窗都不可拖窗 → 全部点击都到达 WM_LBUTTONDOWN → onLDown 才能判定
    //   点内=交给菜单项 / 点外=关闭浮层。
    // 否则菜单外的空白会被 NCHITTEST 判成 HTCAPTION,Windows 直接进入拖窗、
    // 根本不发 WM_LBUTTONDOWN,导致"点外关闭"永远收不到事件。
    // (dim 背景的 Modal/Popup 本就 blocks_drag_bg=true,这里对无 dim 的菜单/下拉补齐。)
    bool pointBlocksDrag(POINT p) const {
        for (auto& e : entries_) {
            if (e.blocks_drag_bg) return true;        // dim 铺满全窗
            if (e.dismiss_on_outside) return true;    // 点外可关的浮层:整窗都要能收到点击
            if (e.rect.contains(p)) return true;      // 其余:仅卡片区
        }
        return false;
    }

    // 点是否落在任一浮层卡片矩形内。
    bool pointInAny(POINT p) const {
        for (auto& e : entries_) if (e.rect.contains(p)) return true;
        return false;
    }

    // 左键派发。返回 true = 已消费(调用方 return,绝不穿透到下层 view)。
    // 命中某浮层矩形 → dispatchClick 交给其按钮/行 hit;全部落空 → 关栈顶(若允许)。
    bool onLDown(POINT p);

    // 右键派发(菜单)。返回 true = 已消费。
    bool onRDown(POINT p);

    // ESC:关栈顶浮层(Confirm/Popup 的 dismiss_on_outside=false 不影响 ESC —— ESC 总能关)。
    bool onEsc();

private:
    std::vector<OverlayEntry> entries_;
};

extern OverlayStack g_overlays;

}  // namespace launcher::d2d
