#include "ui/i18n/locale.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <fstream>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

namespace launcher::ui::i18n {

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

// 把嵌套 JSON 拍平成 "a.b.c" key
void flatten(const json& j, const std::string& prefix,
             std::unordered_map<std::string, std::string>& out) {
    if (j.is_object()) {
        for (auto it = j.begin(); it != j.end(); ++it) {
            std::string k = prefix.empty() ? it.key() : prefix + "." + it.key();
            flatten(it.value(), k, out);
        }
    } else if (j.is_string()) {
        out[prefix] = j.get<std::string>();
    } else if (j.is_number()) {
        out[prefix] = j.dump();
    }
}

const char* codeOf(Language l) {
    switch (l) {
        case Language::En:   return "en";
        case Language::ZhCN: return "zh-CN";
        case Language::JaJP: return "ja-JP";
        default:             return "en";
    }
}

}  // namespace

Locale& Locale::instance() { static Locale s; return s; }

std::vector<LanguageInfo> Locale::available() {
    return {
        { Language::En,   "en",    "English"     },
        { Language::ZhCN, "zh-CN", u8"简体中文"   },
        { Language::JaJP, "ja-JP", u8"日本語"     },
    };
}

Language Locale::detectSystemLanguage() const {
    wchar_t buf[LOCALE_NAME_MAX_LENGTH] = {0};
    if (::GetUserDefaultLocaleName(buf, LOCALE_NAME_MAX_LENGTH) == 0) {
        return Language::En;
    }
    std::wstring w(buf);
    if (w.find(L"zh") == 0) return Language::ZhCN;
    if (w.find(L"ja") == 0) return Language::JaJP;
    return Language::En;
}

Status Locale::loadFromDirectory(const std::string& dir) {
    fs::path d(dir);
    if (!fs::exists(d)) {
        spdlog::warn("i18n dir missing: {}", dir);
        return Status::Err(1);
    }

    int loaded = 0;
    for (auto& info : available()) {
        fs::path p = d / (std::string(info.code) + ".json");
        if (!fs::exists(p)) continue;
        std::ifstream in(p);
        json j;
        try { in >> j; }
        catch (...) { spdlog::warn("i18n parse failed: {}", p.string()); continue; }
        std::unordered_map<std::string, std::string> flat;
        flatten(j, "", flat);
        m_strings[info.code] = std::move(flat);
        ++loaded;
    }
    spdlog::info("i18n loaded {} language(s)", loaded);
    return loaded > 0 ? Status::Ok() : Status::Err(2);
}

void Locale::setLanguage(Language lang) {
    if (m_lang == lang) return;
    m_lang = lang;
    for (auto& fn : m_listeners) fn(lang);
}

Language Locale::resolved() const {
    return m_lang == Language::System ? detectSystemLanguage() : m_lang;
}

std::string Locale::t(const std::string& key, const std::string& fallback) const {
    const char* code = codeOf(resolved());
    auto it = m_strings.find(code);
    if (it != m_strings.end()) {
        auto kit = it->second.find(key);
        if (kit != it->second.end()) return kit->second;
    }
    // 退到 en
    auto en = m_strings.find("en");
    if (en != m_strings.end()) {
        auto kit = en->second.find(key);
        if (kit != en->second.end()) return kit->second;
    }
    return fallback.empty() ? key : fallback;
}

}  // namespace launcher::ui::i18n
