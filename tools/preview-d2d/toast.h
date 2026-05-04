// 右下角 Toast — 1:1 复刻 tools/preview/loading_demo.cpp::Toast。
// fade in 0.20s + 2.5s 自动 fade out 0.30s。
#pragma once

#include "anim.h"
#include "d2d_app.h"
#include <string>

namespace launcher::d2d::toast {

struct Toast {
    std::wstring text;
    Tween t;
    float live = 0.0f;
};

extern Toast g_toast;

void show(const wchar_t* s);
inline void show(const std::wstring& s) { show(s.c_str()); }
void tick(float dt);
void paint(D2DApp& app, float W, float H);

}  // namespace launcher::d2d::toast
