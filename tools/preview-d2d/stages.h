// 入场动画 stage state machine + paint 函数 — 1:1 复刻 GDI+ Preview。
//
// 流程：Dot → ExpandLoading → Loading → ExpandAuth → Auth
//      → ShrinkSuccess → CheckSuccess → ExpandMain → Main
// (登录成功后 480x540 卡片缩回 200x200 → 在小窗里画绿底 + 打勾动画 → 扩到 1100x720)
#pragma once

#include "anim.h"
#include "d2d_app.h"

namespace launcher::d2d::stages {

enum class Stage {
    Dot, ExpandLoading, Loading, Expanding, ExpandAuth, Auth,
    ShrinkSuccess, CheckSuccess, ExpandMain, Main
};
enum class AuthMode { Login, Register };
enum class View { Home, Lunching, Chat, Market, Cloud, Settings, Profile };

// ---------- 全局状态 ----------
extern Stage    g_stage;
extern AuthMode g_auth_mode;
extern View     g_view;
extern bool     g_skip_auth_after_loading;
extern float    g_time_in_stage;
extern float    g_spin_angle;        // spinner 度数累加
extern bool     g_auth_succeeded;

// 卡片 / 窗口 / 入场流程 Tween（命名同 GDI+ 那边）
extern Tween g_card_scale, g_card_opacity, g_card_fade_out;
extern Tween g_window_w, g_window_h;
extern Tween g_sidebar_x, g_topbar_y, g_main_opacity;
extern Tween g_view_fade;
extern Tween g_dot_size, g_dot_alpha;
extern Tween g_auth_card_y, g_auth_card_op;
extern Tween g_check_anim;

// ---------- Stage 切换 ----------
void enterDotStage();
void enterExpandLoadingStage();
void enterLoadingStage();
void enterExpandAuthStage();
void enterAuthStage();
void enterShrinkSuccessStage();
void enterCheckSuccessStage();
void enterExpandMainStage();
void enterMainStage();

// 模拟 Auth submit — Step 3 之前没真表单，按 Enter 触发这个
void simulateAuthSubmit();

// 退出登录 → 缩窗口 + 复位所有动画 tween 回到 Auth view（不走 ShrinkSuccess 那条线）
void enterAuthFromLogout();

// ---------- 帧循环钩子 ----------
// dt = 自上一帧起的秒数；驱动所有 tween + spin_angle。
void tick(float dt);

// 在主 frame loop 调（在 beginFrame 之前）—— 处理 stage 切换 + SetWindowPos 几何动画。
// sw / sh = 屏幕尺寸（GetSystemMetrics），用来居中 SetWindowPos。
// 返回 true 表示触发了 stage 切换或几何变化（业务可据此调度 InvalidateRect 等）。
bool driveTransitions(D2DApp& app, int sw, int sh);

// 任意 tween / spinner 还在动 → 帧循环不能 idle。
bool anyAnimating();

// ---------- Paint dispatch ----------
// 按当前 g_stage 选对应 paint*；W/H 是 client 物理像素（D2D 已 SetDpi，业务可直接用 DIP）。
void paint(D2DApp& app);

}  // namespace launcher::d2d::stages
