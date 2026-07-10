#include "anim_settings.h"
#include <algorithm>

namespace launcher::d2d {
AnimSettings g_anim;

float AnimSettings::dur(bool on, float d) {
    if (!on) return 0.001f;
    return d / g_anim.clampedSpeed();   // 速度越大 → 时长越短
}
}  // namespace launcher::d2d
