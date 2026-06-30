// I18N — 三语 en / zh-cn / ja-jp 字符串表。
// 1:1 复刻 tools/preview/loading_demo.cpp 顶部的 tr() 表。
#pragma once

#include <string>

namespace launcher::d2d {

enum class Lang { En, ZhCN, JaJP };
extern Lang g_lang;

// 返回系统 UI 语言对应的 Lang（首次启动无持久化设置时用作默认）
Lang detectSystemLang();

// 返回 utf-8 字符串
const char* tr(const char* key);

// utf-8 → wstring
std::wstring trW(const char* key);

}  // namespace launcher::d2d
