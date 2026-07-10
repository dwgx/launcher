// 全局动画设置 —— 只两项掌管全局:总开关 + 动画速度倍率。persist 持久化。
// 所有动画统一读这里:关了瞬发;速度倍率缩放所有时长/逼近率。
#pragma once

#include "anim.h"
#include <algorithm>

namespace launcher::d2d {

struct AnimSettings {
    bool  master = true;         // 总开关(关=全部动画瞬发)
    float speed  = 1.0f;         // 速度倍率(0.25..3.0;越大越快)。用户可调。

    // 兼容旧调用点:各分类累计都归到总开关(不再分类)。
    bool viewSwitch() const { return master; }
    bool channelAnim() const { return master; }
    bool pickerAnim() const { return master; }
    bool hoverAnim() const { return master; }

    float clampedSpeed() const { return (std::max)(0.25f, (std::min)(3.0f, speed)); }

    // 门控时长:关→瞬发;开→基准时长 / 速度(速度越大越短)。
    static float dur(bool on, float d);
};

extern AnimSettings g_anim;

// persist:低 1 bit=master,高位= speed×100(如 1.0→100)。
inline unsigned animBits(const AnimSettings& a) {
    unsigned sp = (unsigned)(a.clampedSpeed() * 100.0f + 0.5f);
    return (a.master ? 1u : 0u) | (sp << 1);
}
inline void animFromBits(AnimSettings& a, unsigned b) {
    a.master = (b & 1u) != 0;
    unsigned sp = b >> 1;
    float s = sp ? (float)sp / 100.0f : 1.0f;
    // sanitize:历史脏值(如曾把 0xFFFFFFFF 当默认 → sp 巨大)一律回落到 1.0×。
    if (s < 0.25f || s > 3.0f) s = 1.0f;
    a.speed = s;
}

}  // namespace launcher::d2d
