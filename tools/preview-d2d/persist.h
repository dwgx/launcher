// 隐秘注册表持久化 — 1:1 复刻 tools/preview/loading_demo.cpp::persist。
// 30 候选路径 (伪装成系统/Office/MuiCache 子键)，启动遍历找 _m=LUNC magic 的那一个。
// 凭据用 DPAPI（CryptProtectData，绑定当前用户）加密存 _p 字段。
#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <wincrypt.h>
#include <string>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "crypt32.lib")

namespace launcher::d2d::persist {

void saveLang(int v);
void saveTheme(bool dark);
int  loadLang(int dflt);
bool loadTheme(bool dflt);
void saveCreds(const std::wstring& u, const std::wstring& p);
bool loadCreds(std::wstring& u, std::wstring& p);
void clearCreds();
void saveSession(const std::string& tok, const std::string& uid);
bool loadSession(std::string& tok, std::string& uid);

}  // namespace launcher::d2d::persist
