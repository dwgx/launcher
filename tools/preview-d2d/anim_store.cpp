#include "anim_store.h"
#include "anim_settings.h"
#include <unordered_map>

namespace launcher::d2d::anim {

namespace {
struct Slot {
    float cur;        // 当前值
    float target;     // 目标值
    float rate;       // 每帧逼近比例
    uint32_t touched; // 最近访问的帧序号(用于清理)
};
std::unordered_map<uint64_t, Slot> g_slots;
uint32_t g_frame = 0;
}  // namespace

float hover(uint64_t k, bool active, float rate) {
    float tgt = active ? 1.0f : 0.0f;
    auto it = g_slots.find(k);
    if (it == g_slots.end()) {
        // 首次:直接取目标(避免刚出现就从 0 爬)
        g_slots[k] = { tgt, tgt, rate, g_frame };
        return tgt;
    }
    it->second.target = tgt;
    it->second.rate = rate;
    it->second.touched = g_frame;
    // 动画关:吸附
    if (!g_anim.master) { it->second.cur = tgt; return tgt; }
    return it->second.cur;
}

float toward(uint64_t k, float target, float rate) {
    auto it = g_slots.find(k);
    if (it == g_slots.end()) {
        g_slots[k] = { target, target, rate, g_frame };
        return target;
    }
    it->second.target = target;
    it->second.rate = rate;
    it->second.touched = g_frame;
    if (!g_anim.master) { it->second.cur = target; return target; }
    return it->second.cur;
}

void tickAll(float /*dt*/) {
    ++g_frame;
    float spd = g_anim.clampedSpeed();
    for (auto it = g_slots.begin(); it != g_slots.end(); ) {
        Slot& s = it->second;
        // 推进:指数逼近,乘全局速度倍率(封顶 0.9 防越过)
        float r = (s.rate * spd > 0.9f) ? 0.9f : s.rate * spd;
        s.cur += (s.target - s.cur) * r;
        if (std::abs(s.cur - s.target) < 0.001f) s.cur = s.target;
        // 清理:已到目标 + 连续多帧没被访问(元素已消失)→ 删
        bool settled = (s.cur == s.target);
        bool stale = (g_frame - s.touched) > 120;   // ~2s 没画到
        if (settled && stale) it = g_slots.erase(it);
        else ++it;
    }
}

}  // namespace launcher::d2d::anim
