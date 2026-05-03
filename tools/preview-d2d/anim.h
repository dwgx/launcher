// 动画 Tween + curve — 1:1 复刻 tools/preview/loading_demo.cpp Tween / curve::*
// 数学纯，跟 GDI+ / D2D 都不耦合。
#pragma once

namespace launcher::d2d {

namespace curve {
inline float easeOutQuint(float t) { float i = 1.0f - t; return 1.0f - i*i*i*i*i; }
inline float easeOutCubic(float t) { float i = 1.0f - t; return 1.0f - i*i*i; }
inline float easeOutBack(float t)  {
    constexpr float c1 = 1.70158f, c3 = c1 + 1.0f;
    float i = t - 1.0f;
    return 1.0f + c3 * i*i*i + c1 * i*i;
}
}  // namespace curve

struct Tween {
    float from{0}, to{0}, duration{0.2f}, delay{0}, elapsed{0};
    float (*c)(float){curve::easeOutQuint};
    bool started{false};

    void start(float f, float t, float d, float dl = 0,
               float (*cv)(float) = curve::easeOutQuint) {
        from = f; to = t; duration = d; delay = dl;
        elapsed = -dl; c = cv; started = true;
    }
    void tick(float dt) { if (started) elapsed += dt; }
    bool done() const { return started && elapsed >= duration; }
    float value() const {
        if (!started || elapsed <= 0) return from;
        if (elapsed >= duration) return to;
        return from + (to - from) * c(elapsed / duration);
    }
};

}  // namespace launcher::d2d
