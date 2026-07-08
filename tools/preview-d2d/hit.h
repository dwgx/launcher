// 鼠标命中区注册 — 跟 GDI+ Preview g_hits 一样：渲染时累加，WM_LBUTTONDOWN
// 时遍历 reverse 找命中并调 callback。简单粗暴但跟自绘 UI 一拍即合。
#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <functional>
#include <vector>

namespace launcher::d2d {

struct LayoutRect {
    float x{}, y{}, w{}, h{};
    float right()  const { return x + w; }
    float bottom() const { return y + h; }
    bool  contains(POINT p) const {
        return p.x >= x && p.x <= x + w && p.y >= y && p.y <= y + h;
    }
};

struct HitArea {
    LayoutRect rect;
    std::function<void()> click;
    bool button;            // true = 鼠标按钮（手型光标）；false = 普通命中
};

extern std::vector<HitArea> g_hits;

// 鼠标 DIP 坐标 + 按键状态 — 全局，渲染 helper / hover 判定都要用
extern POINT g_mouse;
extern bool  g_mouse_pressed;

// 统一浮层几何(旧机制,过渡期保留:markOverlayRect/pointInAnyOverlay 仍被少量
// 代码填充,但派发已全部走 overlay.h 的 OverlayStack)。
// TODO(cleanup): markOverlayRect / g_overlay_rects 可随后续清理移除,派发已不依赖它。
struct LayoutRect;
void clearOverlayRects();
void markOverlayRect(float x, float y, float w, float h);
bool pointInAnyOverlay(POINT p);

inline void hitClear() { g_hits.clear(); }

inline void hit(LayoutRect r, std::function<void()> cb, bool button = false) {
    g_hits.push_back({ r, std::move(cb), button });
}
inline void hit(float x, float y, float w, float h,
                std::function<void()> cb, bool button = false) {
    g_hits.push_back({ {x, y, w, h}, std::move(cb), button });
}

inline bool inRect(POINT p, LayoutRect r) { return r.contains(p); }

// 反向找命中 — 后注册的（modal / popover）盖在前面注册的上层
inline bool dispatchClick(POINT p) {
    for (auto it = g_hits.rbegin(); it != g_hits.rend(); ++it) {
        if (it->rect.contains(p)) {
            if (it->click) it->click();
            return true;
        }
    }
    return false;
}


inline bool anyHover(POINT p) {
    for (auto& h : g_hits) if (h.rect.contains(p)) return true;
    return false;
}

// 窗口拖动判定用:命中任何「真实交互区」返回 true(该处让给客户区,不拖窗)。
// 关键:忽略全屏遮罩/背景吞击 hit —— 公告弹窗、消息/用户菜单、聊天区都注册过
// 接近整窗大小的 (0,0,W,H) 背景 hit,若把它们算进去,整窗任何位置都被判为
// 客户区而拖不动。用尺寸阈值排除:任一边 >= 窗口的 92% 视为遮罩,不计。
// 这样气泡/频道/输入框/按钮等局部 hit 仍让路(点击不误拖),而空白/遮罩处可拖。
inline bool hoverInteractive(POINT p, float W, float H) {
    for (auto& h : g_hits) {
        if (!h.rect.contains(p)) continue;
        bool full_mask = (h.rect.w >= W * 0.92f) && (h.rect.h >= H * 0.92f);
        if (full_mask) continue;   // 跳过整窗遮罩
        return true;
    }
    return false;
}

}  // namespace launcher::d2d
