// visual_smoke.h — 可插拔视觉冒烟钩子（detachable visual-smoke hook）。
//
// 整个 TU 都在 #ifdef LAUNCHER_VISUAL_SMOKE 之内：宏未定义时（正常 build_d2d.bat）
// 本文件编译为空翻译单元，不向运行期路径泄漏任何符号，call-site 也被 #ifdef 掉。
// 详见 tmp/visual-smoke/PLAN-*.md（hook_design / capture_mechanism /
// detachability_plan / scenario_matrix）。
//
// 契约：seam = 冻结的全局 tween + 公共 accessor（ctx()/wic()），不碰 D2DApp 私有成员，
// 也不给任何现有类新增公共方法。移除方法见 detachability_plan：删本文件 + .cpp +
// build_d2d_visual.bat + wWinMain 里那段 #ifdef 块即可，grep visual_smoke / 宏名 应为 0。
#pragma once

#ifdef LAUNCHER_VISUAL_SMOKE

#include "d2d_app.h"

namespace launcher::d2d::visual_smoke {

// 是否请求进入冒烟模式：环境变量 LAUNCHER_VISUAL_SMOKE 非空，或命令行含 --visual-smoke。
bool requested();

// 驱动全部 scenario、逐屏截 PNG，返回进程 exit code（0 = 全成功）。
// 自带 beginFrame→paint→capture→endFrame 循环，绝不进正常消息循环。
int run(D2DApp& app, HWND hwnd);

}  // namespace launcher::d2d::visual_smoke

#endif  // LAUNCHER_VISUAL_SMOKE
