#include "ui/theme/theme_manager.h"
#include "ui/theme/animation.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

namespace launcher::theme {

namespace {
PaletteSnapshot lightPalette() {
    return {
        light::kBg, light::kCard, light::kDivider,
        light::kPrimary, light::kPrimaryHover,
        light::kTextPrimary, light::kTextMuted,
        light::kShadow, light::kShadowHover,
        light::kCloseHover
    };
}
PaletteSnapshot darkPalette() {
    return {
        dark::kBg, dark::kCard, dark::kDivider,
        dark::kPrimary, dark::kPrimaryHover,
        dark::kTextPrimary, dark::kTextMuted,
        dark::kShadow, dark::kShadowHover,
        dark::kCloseHover
    };
}

// Why: Win10/11 用注册表 AppsUseLightTheme 决定系统主题
bool systemPrefersDark() {
    HKEY hk = nullptr;
    if (::RegOpenKeyExW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            0, KEY_READ, &hk) != ERROR_SUCCESS) {
        return false;
    }
    DWORD value = 1, sz = sizeof(value);
    LSTATUS r = ::RegQueryValueExW(hk, L"AppsUseLightTheme", nullptr, nullptr,
                                   reinterpret_cast<LPBYTE>(&value), &sz);
    ::RegCloseKey(hk);
    return r == ERROR_SUCCESS && value == 0;
}
}  // namespace

ThemeManager& ThemeManager::instance() {
    static ThemeManager s;
    return s;
}

void ThemeManager::setMode(Mode m) {
    if (m == m_mode) return;
    m_target = m;
    m_transition = 0.0f;
    m_mode = m;
}

Mode ThemeManager::resolved() const {
    if (m_mode == Mode::System) {
        return systemPrefersDark() ? Mode::Dark : Mode::Light;
    }
    return m_mode;
}

PaletteSnapshot ThemeManager::palette() const {
    return resolved() == Mode::Dark ? darkPalette() : lightPalette();
}

void ThemeManager::tick(f32 dt_seconds) {
    if (m_transition >= 1.0f) return;
    m_transition = clamp01(m_transition + dt_seconds * (1000.0f / kThemeSwitchMs));
}

}  // namespace launcher::theme
