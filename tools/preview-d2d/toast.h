// 顶部居中 Dynamic-Island 通知 —— 从上边缘弹入的单行胶囊。
// Enter(0.50s 下滑 easeOutBack 回弹 + 0.44s 宽度 morph + 0.20s 淡入)
//   → Hold(~2.9s) → Exit(0.34s 上滑 + 0.30s 淡出)。
#pragma once

#include "anim.h"
#include "d2d_app.h"
#include <cstdint>
#include <string>

namespace launcher::d2d::toast {

struct Toast {
    std::wstring text;                          // 单行药丸标签
    enum class Phase { Hidden, Enter, Hold, Exit };
    Phase   phase   = Phase::Hidden;
    Tween y;      // 真实 y (DIP)：kHiddenY 落到 kTopY；easeOutBack 回弹落在屏幕坐标
    Tween width;  // 0..1 morph-open 进度；paint 里 lerp(wMin, fullW)
    Tween fade;   // 0..1 胶囊不透明度
    float live = 0.0f;                           // show() 以来的秒数；驱动 Hold->Exit
};

extern Toast g_toast;

// 默认入口 —— Drop-in 瞬态药丸。
void show(const wchar_t* s);
inline void show(const std::wstring& s) { show(s.c_str()); }

void tick(float dt);
void paint(D2DApp& app, float W, float H);

}  // namespace launcher::d2d::toast
