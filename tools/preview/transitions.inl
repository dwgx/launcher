// 通用 transition 框架 — 包装 Tween 给 view / modal / popover 复用。
// 每种 transition 自带 enter / exit + apply 给 area 或 alpha 偏移。
#pragma once

namespace tx {

// ---------- Slide ----------
// view 切换、modal 入场常用：translate Y / X + opacity 双 tween
struct Slide {
    Tween x, y, op;
    bool active() const { return op.started && !op.done(); }

    // 进入：从 (dx, dy) 滑回 (0, 0)，op 0→1
    void enter(float dx = 0, float dy = 12.0f, float dur = 0.30f) {
        x.start(dx, 0.0f, dur, 0, curve::easeOutCubic);
        y.start(dy, 0.0f, dur, 0, curve::easeOutCubic);
        op.start(0.0f, 1.0f, dur, 0, curve::easeOutCubic);
    }
    // 退出：滑出到 (dx, dy)，op 1→0
    void exit(float dx = 0, float dy = 12.0f, float dur = 0.20f) {
        x.start(x.value(), dx, dur, 0, curve::easeOutCubic);
        y.start(y.value(), dy, dur, 0, curve::easeOutCubic);
        op.start(op.value(), 0.0f, dur, 0, curve::easeOutCubic);
    }
    void tick(float dt) { x.tick(dt); y.tick(dt); op.tick(dt); }
};

// ---------- Fade ----------
struct Fade {
    Tween op;
    bool active() const { return op.started && !op.done(); }
    void enter(float dur = 0.25f) { op.start(0.0f, 1.0f, dur, 0, curve::easeOutCubic); }
    void exit(float dur = 0.20f)  { op.start(op.value(), 0.0f, dur, 0, curve::easeOutCubic); }
    void tick(float dt) { op.tick(dt); }
};

// ---------- Scale ----------
// modal popup 入场：scale 0.94→1 + op 0→1
struct Scale {
    Tween s, op;
    bool active() const { return op.started && !op.done(); }
    void enter(float from = 0.94f, float dur = 0.28f) {
        s.start(from, 1.0f, dur, 0, curve::easeOutBack);
        op.start(0.0f, 1.0f, dur, 0, curve::easeOutCubic);
    }
    void exit(float to = 0.94f, float dur = 0.20f) {
        s.start(s.value(), to, dur, 0, curve::easeOutCubic);
        op.start(op.value(), 0.0f, dur, 0, curve::easeOutCubic);
    }
    void tick(float dt) { s.tick(dt); op.tick(dt); }
};

// 给 Color 应用 alpha multiplier
inline Color fadeColor(Color c, float a) {
    return Color((BYTE)(c.GetA() * a), c.GetR(), c.GetG(), c.GetB());
}

// 把 Slide 应用到 Graphics transform — 调 apply() 后画的内容会被 slide 偏移
struct PushTransform {
    Graphics& g;
    PushTransform(Graphics& gg, float x, float y) : g(gg) { g.TranslateTransform(x, y); }
    ~PushTransform() { g.ResetTransform(); }
};

}  // namespace tx
