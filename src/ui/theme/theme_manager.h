#pragma once

#include "app/common.h"
#include "ui/theme/color_tokens.h"

namespace launcher::theme {

enum class Mode : u8 {
    System = 0,
    Light  = 1,
    Dark   = 2,
};

struct PaletteSnapshot {
    Color bg, card, divider;
    Color primary, primary_hover;
    Color text_primary, text_muted;
    Color shadow, shadow_hover;
    Color close_hover;
};

class ThemeManager {
public:
    static ThemeManager& instance();

    void setMode(Mode m);
    Mode mode() const { return m_mode; }

    // 解析 system mode 后返回 light/dark 二选一
    Mode resolved() const;
    PaletteSnapshot palette() const;

    // 主题切换插值 (0..1)，UI 线程每帧推进
    void tick(f32 dt_seconds);
    f32 transition() const { return m_transition; }

private:
    ThemeManager() = default;
    LAUNCHER_DISALLOW_COPY(ThemeManager);

    Mode m_mode{Mode::System};
    Mode m_target{Mode::System};
    f32  m_transition{1.0f};
};

}  // namespace launcher::theme
