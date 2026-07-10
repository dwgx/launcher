// 通用 keyed 动画基元 —— 每个可交互元素用稳定 key 记一个 0..1 进度,每帧朝 target
// 指数逼近。免去为每个 hover/press 手写 Tween。全 app hover/按压反馈统一走这里。
//
// 用法(paint 里):
//   float h = anim::hover(anim::key("sidebar.nav", i), rect.contains(g_mouse));
//   ...用 h(0..1) 插值底色/缩放...
// tick:main loop 每帧调 anim::tickAll(dt) 推进 + 清理久未访问 key(防 map 膨胀)。
#pragma once

#include <cstdint>
#include <string>

namespace launcher::d2d {

// ARGB 颜色按 t(0..1)线性插值(动画色渐变通用)。
inline uint32_t lerpArgb(uint32_t a, uint32_t b, float t) {
    if (t <= 0) return a; if (t >= 1) return b;
    auto ch = [](uint32_t c, int sh){ return (int)((c >> sh) & 0xFF); };
    int A = ch(a,24) + (int)((ch(b,24)-ch(a,24))*t);
    int R = ch(a,16) + (int)((ch(b,16)-ch(a,16))*t);
    int G = ch(a,8)  + (int)((ch(b,8) -ch(a,8)) *t);
    int B = ch(a,0)  + (int)((ch(b,0) -ch(a,0)) *t);
    return ((uint32_t)A<<24)|((uint32_t)R<<16)|((uint32_t)G<<8)|(uint32_t)B;
}

}  // namespace launcher::d2d

namespace launcher::d2d::anim {

// 由字符串前缀 + 整数索引生成稳定 key(FNV-1a)。
inline uint64_t key(const char* prefix, int idx = 0) {
    uint64_t h = 1469598103934665603ull;
    for (const char* p = prefix; *p; ++p) { h ^= (uint8_t)*p; h *= 1099511628211ull; }
    h ^= (uint64_t)(idx + 1) * 2654435761ull;
    return h;
}
inline uint64_t keyStr(const std::wstring& s, const char* prefix = "") {
    uint64_t h = 1469598103934665603ull;
    for (const char* p = prefix; *p; ++p) { h ^= (uint8_t)*p; h *= 1099511628211ull; }
    for (wchar_t c : s) { h ^= (uint16_t)c; h *= 1099511628211ull; }
    return h;
}

// 返回该 key 当前进度(0..1),并把目标设为 active?1:0。每帧 tickAll 推进。
// rate 越大越快(每帧逼近比例)。动画关时(g_anim)直接吸附到 target(瞬发)。
float hover(uint64_t k, bool active, float rate = 0.13f);

// 通用:朝任意浮点目标逼近(如滑动指示器位置)。首次见到该 key 直接取 target。
float toward(uint64_t k, float target, float rate = 0.13f);

// 每帧推进所有活跃 key + 清理这一帧没被访问过的(超时)条目。
void tickAll(float dt);

}  // namespace launcher::d2d::anim
