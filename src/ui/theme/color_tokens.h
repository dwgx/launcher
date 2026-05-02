#pragma once

// 全项目颜色常量表。禁止在其他文件用魔法 RGBA 字面量。
// 颜色值与 docs/design-tokens.md 同步，更新时双写。

#include "app/common.h"

namespace launcher::theme {

struct Color {
    u8 r, g, b, a;
    constexpr u32 toSkColor() const {
        return (static_cast<u32>(a) << 24) |
               (static_cast<u32>(r) << 16) |
               (static_cast<u32>(g) <<  8) |
               (static_cast<u32>(b));
    }
};

constexpr Color rgba(u8 r, u8 g, u8 b, u8 a = 255) { return {r, g, b, a}; }

namespace light {
constexpr Color kBg          = rgba(0xFA, 0xF7, 0xF2);
constexpr Color kCard        = rgba(0xFF, 0xFF, 0xFF);
constexpr Color kDivider     = rgba(0xED, 0xE9, 0xE1);
constexpr Color kPrimary     = rgba(0xC9, 0x64, 0x42);
constexpr Color kPrimaryHover= rgba(0xD9, 0x77, 0x57);
constexpr Color kTextPrimary = rgba(0x1F, 0x1E, 0x1D);
constexpr Color kTextMuted   = rgba(0x6B, 0x6A, 0x67);
constexpr Color kShadow      = rgba(0x00, 0x00, 0x00,  10);  // 0.04 alpha
constexpr Color kShadowHover = rgba(0x00, 0x00, 0x00,  20);  // 0.08
constexpr Color kCloseHover  = rgba(0xE3, 0x4B, 0x4B);
}  // namespace light

namespace dark {
constexpr Color kBg          = rgba(0x1A, 0x18, 0x16);
constexpr Color kCard        = rgba(0x24, 0x22, 0x20);
constexpr Color kDivider     = rgba(0x36, 0x32, 0x2D);
constexpr Color kPrimary     = rgba(0xD9, 0x77, 0x57);
constexpr Color kPrimaryHover= rgba(0xE5, 0x86, 0x66);
constexpr Color kTextPrimary = rgba(0xF5, 0xF1, 0xEA);
constexpr Color kTextMuted   = rgba(0xA8, 0xA3, 0x9A);
constexpr Color kShadow      = rgba(0x00, 0x00, 0x00,  60);
constexpr Color kShadowHover = rgba(0x00, 0x00, 0x00,  90);
constexpr Color kCloseHover  = rgba(0xE3, 0x4B, 0x4B);
}  // namespace dark

// 半径档位 — 只允许这三档
constexpr f32 kRadiusSm = 8.0f;
constexpr f32 kRadiusMd = 12.0f;
constexpr f32 kRadiusLg = 16.0f;

}  // namespace launcher::theme
