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
extern Tween  g_seg_lang_x, g_seg_theme_x;

void switchView(stages::View v);


// 松开鼠标 — 结束主视图滚动条拖动（复刻 chat::onMouseLUp 里的 g_scroll_drag 清零）。
void onViewMouseUp();

void paintMain(D2DApp& app, float W, float H);

void tickMain(float dt);   // 给 main loop 调，tick 各 ui Tween

}  // namespace launcher::d2d::ui
