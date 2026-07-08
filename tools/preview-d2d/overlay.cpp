// OverlayStack 派发实现。见 overlay.h 头注释。
#include "overlay.h"

namespace launcher::d2d {

OverlayStack g_overlays;

bool OverlayStack::onLDown(POINT p) {
    if (entries_.empty()) return false;   // 无浮层:交给下层 view

    // webview 拥有子 HWND:点击落在它上层时交给 dispatchClick(其内部 hit),
    // 但不做"点外关"(它靠自己的关闭按钮/ESC)。仅当它是栈顶时特殊处理。
    const OverlayEntry& topmost = entries_.back();
    if (topmost.owns_child_hwnd) {
        dispatchClick(p);
        return true;   // 吞掉,不穿透
    }

    // 点在任一浮层卡片内:交给其按钮/行 hit(反应/引用/删除/输入框聚焦等)。
    if (pointInAny(p)) {
        // picker:点在其(含表情按钮的并集)矩形内时,返回 false 让 WndProc 继续
        // 调 chat::onMouseLDown 处理 composer 聚焦 / pack tab 拖拽起点(修 C7)。
        if (topmost.defer_inside_to_caller && topmost.rect.contains(p)) {
            return false;
        }
        dispatchClick(p);
        return true;
    }

    // 点在全部浮层外:关最顶层(若允许点外关),吞掉点击绝不穿透。
    if (topmost.dismiss_on_outside && topmost.dismiss) {
        topmost.dismiss();
    }
    // 即使 dismiss_on_outside=false(Confirm/Popup),也吞掉:强制用户用按钮/ESC。
    return true;
}

bool OverlayStack::onRDown(POINT p) {
    if (entries_.empty()) return false;
    const OverlayEntry& topmost = entries_.back();
    // 右键仅对菜单类做"点内保留/点外关闭";其余阻塞浮层吞掉右键。
    if (topmost.kind == OverlayKind::Menu) {
        bool inside = topmost.rect.contains(p);
        if (!inside && topmost.dismiss) topmost.dismiss();
        return true;   // 菜单打开时吞掉右键(点内=保留,点外=已关)
    }
    // 阻塞式浮层(Modal/Popup)打开时,右键也不该穿透到下层。
    return anyBlocking();
}

bool OverlayStack::onEsc() {
    if (entries_.empty()) return false;
    const OverlayEntry& topmost = entries_.back();
    // ESC 总能关栈顶(即使 dismiss_on_outside=false —— Confirm/Popup 允许 ESC 取消)。
    if (topmost.dismiss) topmost.dismiss();
    return true;
}

}  // namespace launcher::d2d
