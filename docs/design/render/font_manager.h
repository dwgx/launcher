#pragma once

// 字体加载 + Shaping。中日文必须经 SkShaper + HarfBuzz，禁止 SkFont::drawText。
// 回退链：英文 Space Grotesk → 日文 BIZ UDPGothic → 中文 Source Han Sans CN
//        → 等宽 DejaVu Mono → 系统兜底

#include "app/common.h"

class SkCanvas;

namespace launcher::ui::render {

class FontManager {
public:
    FontManager();
    ~FontManager();
    LAUNCHER_DISALLOW_COPY(FontManager);

    // 注册 assets/fonts/ 下所有 ttf/otf
    Status loadFromDirectory(const std::string& dir);

    // y 是 baseline。color 是 SkColor (ARGB)
    void drawShapedText(SkCanvas* canvas, const std::string& utf8,
                        f32 x, f32 y, f32 size_px, u32 sk_color);

    // 仅测量宽度，用于 Clay MeasureText 桥接
    f32 measureWidth(const std::string& utf8, f32 size_px);

private:
    struct Impl;
    Impl* m_impl{nullptr};
};

}  // namespace launcher::ui::render
