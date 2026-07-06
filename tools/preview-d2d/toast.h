// 顶部居中 Dynamic-Island 通知 —— 从上边缘弹入的胶囊，可扩展成卡片/常驻广播。
// Enter(0.50s 下滑 easeOutBack 回弹 + 0.44s 宽度 morph + 0.20s 淡入)
//   → Hold(~2.4s，Broadcast 无限) → Exit(0.34s 上滑 + 0.30s 淡出)。
//
// Variant/Anim 两条正交枚举 + 附加字段扩展基础胶囊：
//   Variant = 长什么样（Toast 药丸 / Expanded 高卡片 / Broadcast 常驻卡片）
//   Anim    = 上一次内容变化怎么动（驱动 tick 子状态 + paint 变换）
// Phase 生命周期（Hidden/Enter/Hold/Exit）是容器时间线，保持不变。
#pragma once

#include "anim.h"
#include "d2d_app.h"
#include <cstdint>
#include <string>

namespace launcher::d2d::toast {

enum class Variant { Toast, Expanded, Broadcast };  // 药丸 | 高卡片 | 常驻卡片
enum class Anim    { Drop, Expand, Swap, Flip };     // 上次转场；驱动 tick 子状态 + paint 变换

struct Toast {
    std::wstring text;                          // 单行药丸标签（原有用法不变）
    std::wstring title, body;                   // Expanded/Broadcast 卡片内容
    std::wstring pendText, pendTitle, pendBody; // Swap/Flip 中点暂存
    enum class Phase { Hidden, Enter, Hold, Exit };
    Phase   phase   = Phase::Hidden;
    Variant variant = Variant::Toast;           // 外观
    Anim    anim    = Anim::Drop;               // 上次转场
    bool     sticky = false;                    // Broadcast: 禁用 live>timeout 自动离场
    uint32_t accent = 0;                        // 0 => pal.primary；否则如 pal.status_busy
    Tween y;      // 真实 y (DIP)：kHiddenY 落到 kTopY；easeOutBack 回弹落在屏幕坐标
    Tween width;  // 0..1 morph-open 进度；paint 里 lerp(wMin, fullW)
    Tween fade;   // 0..1 胶囊不透明度
    Tween expand; // 0..1 -> curH=lerp(kH,kHCard), curR=lerp(kR,kRCard)，揭示 title/body
    Tween flipY;  // Flip 的 scaleY 挤压；静止=1（守卫：started?value():1）
    Tween content;// 0..1 Swap 文字交叉淡入淡出；静止=1（守卫：started?value():1）
    bool  midDone = false;                       // Swap/Flip 两段式中点闩锁
    float live = 0.0f;                           // show() 以来的秒数；驱动 Hold->Exit
};

extern Toast g_toast;

// 原有默认入口 —— 逐字节不变：Drop-in 瞬态药丸。
void show(const wchar_t* s);
inline void show(const std::wstring& s) { show(s.c_str()); }

// 常驻扩展横幅（Drop-in + expand）。accent_rgb 0 => pal.primary；alert 传 pal.status_busy。
void showBroadcast(const wchar_t* title, const wchar_t* body,
                   uint32_t accent_rgb = 0, bool sticky = true);
inline void showBroadcast(const std::wstring& t, const std::wstring& b,
                          uint32_t accent_rgb = 0, bool sticky = true) {
    showBroadcast(t.c_str(), b.c_str(), accent_rgb, sticky);
}

void swap(const wchar_t* s);                                  // 交叉淡入淡出可见药丸内容
void swapExpanded(const wchar_t* title, const wchar_t* body); // 交叉淡入淡出可见卡片内容
void flip(const wchar_t* s);                                  // scaleY 挤压翻转（药丸）
void dismiss();                                               // 动画离场（常驻广播或任意存活岛）
inline void swap(const std::wstring& s) { swap(s.c_str()); }
inline void flip(const std::wstring& s) { flip(s.c_str()); }

void tick(float dt);
void paint(D2DApp& app, float W, float H);

}  // namespace launcher::d2d::toast
