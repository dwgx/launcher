#pragma once

// I18N 入口：T("key.path") 取译文。启动时按 Win32 GetUserDefaultLocaleName 选语言，
// 设置页可改。资源文件：assets/i18n/{en,zh-CN,ja-JP}.json。
//
// Why: 不用 ICU/gettext，避免引入 50MB 数据；自管理 JSON 平面 key→string map 已够用。

#include "app/common.h"
#include <unordered_map>
#include <vector>
#include <functional>

namespace launcher::ui::i18n {

enum class Language : u8 {
    System = 0,
    En     = 1,
    ZhCN   = 2,
    JaJP   = 3,
};

struct LanguageInfo {
    Language id;
    const char* code;        // "en", "zh-CN", "ja-JP"
    const char* native_name; // "English", "简体中文", "日本語"
};

class Locale {
public:
    static Locale& instance();

    Status loadFromDirectory(const std::string& dir);

    void setLanguage(Language lang);
    Language language() const { return m_lang; }
    Language resolved() const;   // System → 自动判定后的具体语言

    // 取译文。key 不存在时返回 fallback（如果传了）或 key 本身。
    std::string t(const std::string& key, const std::string& fallback = "") const;

    // 列出已加载的语言（设置页用）
    static std::vector<LanguageInfo> available();

    // 语言变更监听
    using OnChangeFn = std::function<void(Language)>;
    void subscribe(OnChangeFn fn) { m_listeners.push_back(std::move(fn)); }

private:
    Locale() = default;
    LAUNCHER_DISALLOW_COPY(Locale);

    Language m_lang{Language::System};
    std::unordered_map<std::string,                                   // language code
                       std::unordered_map<std::string, std::string>>  // key -> value
        m_strings;
    std::vector<OnChangeFn> m_listeners;

    Language detectSystemLanguage() const;
};

// 便捷宏，热路径：T("home.greet")
inline std::string T(const std::string& key, const std::string& fallback = "") {
    return Locale::instance().t(key, fallback);
}

}  // namespace launcher::ui::i18n
