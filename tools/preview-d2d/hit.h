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

// 「模态层地板」——在所有 view/chrome 的 hit 注册完、模态 paint 之前记下 g_hits.size()。
// 模态/浮层打开时,点击只应命中这条线之后注册的 hit(模态自己的按钮),
// 点模态外 = 命中不到任何模态 hit → 关闭,且绝不穿透触发下面 view 的控件。
extern size_t g_modal_hit_floor;

// 统一浮层几何(重构:确定性关闭,不依赖 backdrop hit / floor / paint 时序)。
// 每个 overlay 在 paint 自己的卡片/菜单矩形时调 markOverlayRect() 记下;每帧 paint
// 开头由 clearOverlayRects() 清零。onMouseLDown 直接用 pointInAnyOverlay() 判定:
// 点击落在任一 overlay 矩形内=交给其按钮;落在全部矩形外=关闭最顶层浮层并吞掉点击。
// 这样"点外面关闭"只取决于矩形几何,与 hit 注册顺序/floor/是否先画过一帧无关。
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

// 只在「模态层地板」之后注册的 hit 里找命中(即只考虑模态/浮层自己的 hit,
// 不碰下面 view 的控件)。模态打开时用它:命中=点了模态内按钮;未命中=点了模态外。
inline bool dispatchClickModalOnly(POINT p) {
    if (g_modal_hit_floor > g_hits.size()) return false;
    for (size_t i = g_hits.size(); i-- > g_modal_hit_floor; ) {
        if (g_hits[i].rect.contains(p)) {
            if (g_hits[i].click) g_hits[i].click();
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
