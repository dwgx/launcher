// Design tokens — 复刻自 tools/preview/loading_demo.cpp Palette。
// 用 ARGB32 hex 替代 GDI+ Color，跟 BrushCache::solid(uint32_t argb) 直接对接。
#pragma once

#include <cstdint>

namespace launcher::d2d {

struct Palette {
    uint32_t bg, surface, card, divider;
    uint32_t primary, primary_hover;
    uint32_t text, text_muted, text_faint;
    uint32_t sidebar_bg, sidebar_active;
    uint32_t shadow_card, shadow_card_hover;
    uint32_t overlay_dim;
    uint32_t status_online, status_busy, status_away, status_sleep, status_offline;
};

constexpr Palette kLight = {
    0xFFFAF7F2, 0xFFF3EFE8, 0xFFFFFFFF, 0xFFEDE9E1,
    0xFFC96442, 0xFFD97757,
    0xFF1F1E1D, 0xFF6B6A67, 0xFFA8A39A,
    0xFFFAF7F2, 0xFFE9E1D3,
    0x0A000000, 0x14000000,
    0x73000000,
    0xFF4ADE80, 0xFFE34B4B, 0xFFF5A524, 0xFF8B7BD9, 0xFF6B6A67
};

constexpr Palette kDark = {
    0xFF1A1816, 0xFF201E1B, 0xFF242220, 0xFF36322D,
    0xFFD97757, 0xFFE58666,
    0xFFF5F1EA, 0xFFA8A39A, 0xFF6B6A67,
    0xFF1A1816, 0xFF363029,
    0x3C000000, 0x5C000000,
    0x73000000,
    0xFF4ADE80, 0xFFE34B4B, 0xFFF5A524, 0xFF8B7BD9, 0xFF6B6A67
};

extern bool g_dark;
inline const Palette& palette() { return g_dark ? kDark : kLight; }

}  // namespace launcher::d2d
