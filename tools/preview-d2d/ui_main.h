// Main view 容器 — Topbar + Sidebar + Account dropdown + 各 view dispatch。
#pragma once

#include "d2d_app.h"
#include "stages.h"

namespace launcher::d2d::ui {

constexpr float kSidebarW = 200.0f;
constexpr float kTopbarH  = 48.0f;

// Account dropdown 状态
extern bool   g_account_dropdown;
extern bool   g_status_fold_open;
extern Tween  g_dropdown_t;
extern Tween  g_status_fold_t;
extern Tween  g_seg_lang_x, g_seg_lang_w, g_seg_theme_x, g_seg_theme_w;

void switchView(stages::View v);

void paintMain(D2DApp& app, float W, float H);

void tickMain(float dt);   // 给 main loop 调，tick 各 ui Tween

}  // namespace launcher::d2d::ui
