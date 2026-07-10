#include "persist.h"

namespace launcher::d2d::persist {

constexpr DWORD kMagic = 0x4C554E43;   // 'LUNC'
const wchar_t* kPaths[] = {
    L"Software\\Classes\\Local Settings\\MuiCache\\41",
    L"Software\\Classes\\Local Settings\\MuiCache\\72",
    L"Software\\Classes\\Local Settings\\MuiCache\\a3",
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Accent\\Cache",
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StreamMRU",
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Streams\\.cache",
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings\\5.0\\Cache\\b1",
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings\\5.0\\Cache\\c2",
    L"Software\\Microsoft\\Office\\Common\\InternetFonts\\Cache",
    L"Software\\Microsoft\\Office\\Common\\Fonts\\Hinting",
    L"Software\\Microsoft\\Notepad\\Recent",
    L"Software\\Microsoft\\Notepad\\StatePersistence",
    L"Software\\Microsoft\\IdentityCRL\\Cache\\.alt",
    L"Software\\Microsoft\\Cryptography\\PolicyCache\\Slot",
    L"Software\\Microsoft\\Direct3D\\MostRecentApplication\\Cache",
    L"Software\\Microsoft\\Direct3D\\Adapter\\Cache",
    L"Software\\Microsoft\\InputPersonalization\\Cache",
    L"Software\\Microsoft\\Windows\\Shell\\BagMRU\\NodeSlot",
    L"Software\\Microsoft\\Windows\\Shell\\Bags\\1\\Settings",
    L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\.cache",
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Search\\Cache",
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Network\\Cache",
    L"Software\\Microsoft\\Windows NT\\CurrentVersion\\AppCompatFlags\\Cache",
    L"Software\\Microsoft\\Windows NT\\CurrentVersion\\Diagnostics\\Cache",
    L"Software\\Microsoft\\Windows NT\\CurrentVersion\\NetworkList\\Cache",
    L"Software\\Microsoft\\EventSounds\\.cache",
    L"Software\\Microsoft\\Speech\\Cache",
    L"Software\\Microsoft\\Speech_OneCore\\Cache",
    L"Software\\Microsoft\\Multimedia\\DrawDib\\Cache",
    L"Software\\Microsoft\\Telemetry\\.cache",
};
constexpr int kCount = sizeof(kPaths) / sizeof(kPaths[0]);

static HKEY g_hk = nullptr;
static const wchar_t* g_path = nullptr;

static bool disabled() {
    wchar_t value[16]{};
    DWORD n = GetEnvironmentVariableW(L"LAUNCHER_DISABLE_PERSIST", value, (DWORD)_countof(value));
    if (n == 0 || n >= _countof(value)) return false;
    return wcscmp(value, L"1") == 0
        || _wcsicmp(value, L"true") == 0
        || _wcsicmp(value, L"yes") == 0
        || _wcsicmp(value, L"on") == 0;
}

static bool readDword(HKEY hk, const wchar_t* name, DWORD& out) {
    DWORD cb = sizeof(out), type = 0;
    return RegQueryValueExW(hk, name, nullptr, &type, (LPBYTE)&out, &cb) == ERROR_SUCCESS
           && type == REG_DWORD;
}
static bool readString(HKEY hk, const wchar_t* name, std::wstring& out) {
    wchar_t buf[2048]; DWORD cb = sizeof(buf);
    if (RegQueryValueExW(hk, name, nullptr, nullptr, (LPBYTE)buf, &cb) != ERROR_SUCCESS) return false;
    if (cb < sizeof(wchar_t)) return false;
    out.assign(buf, (cb / sizeof(wchar_t)) - 1);
    return true;
}
static bool tryOpen(const wchar_t* path) {
    HKEY hk;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, path, 0,
                      KEY_READ | KEY_WRITE | KEY_SET_VALUE, &hk) != ERROR_SUCCESS) return false;
    DWORD m = 0;
    if (readDword(hk, L"_m", m) && m == kMagic) {
        g_hk = hk; g_path = path;
        return true;
    }
    RegCloseKey(hk);
    return false;
}
static void ensure() {
    if (g_hk) return;
    for (int i = 0; i < kCount; ++i) if (tryOpen(kPaths[i])) return;
    int pick = (int)(GetTickCount() % (DWORD)kCount);
    HKEY hk;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kPaths[pick], 0, nullptr, 0,
                        KEY_READ | KEY_WRITE | KEY_SET_VALUE, nullptr, &hk, nullptr) == ERROR_SUCCESS) {
        DWORD m = kMagic;
        RegSetValueExW(hk, L"_m", 0, REG_DWORD, (LPBYTE)&m, sizeof(m));
        g_hk = hk;
        g_path = kPaths[pick];
    }
}
void saveLang(int v) {
    if (disabled()) return;
    ensure(); if (!g_hk) return;
    DWORD d = (DWORD)v;
    RegSetValueExW(g_hk, L"_l", 0, REG_DWORD, (LPBYTE)&d, sizeof(d));
}
void saveTheme(bool dark) {
    if (disabled()) return;
    ensure(); if (!g_hk) return;
    DWORD d = dark ? 1 : 0;
    RegSetValueExW(g_hk, L"_t", 0, REG_DWORD, (LPBYTE)&d, sizeof(d));
}
int loadLang(int dflt) {
    if (disabled()) return dflt;
    ensure(); if (!g_hk) return dflt;
    DWORD v; return readDword(g_hk, L"_l", v) ? (int)v : dflt;
}
bool loadTheme(bool dflt) {
    if (disabled()) return dflt;
    ensure(); if (!g_hk) return dflt;
    DWORD v; return readDword(g_hk, L"_t", v) ? (v != 0) : dflt;
}
void saveAnim(unsigned bits) {
    if (disabled()) return;
    ensure(); if (!g_hk) return;
    DWORD d = bits;
    RegSetValueExW(g_hk, L"_a", 0, REG_DWORD, (LPBYTE)&d, sizeof(d));
}
unsigned loadAnim(unsigned dflt) {
    if (disabled()) return dflt;
    ensure(); if (!g_hk) return dflt;
    DWORD v; return readDword(g_hk, L"_a", v) ? (unsigned)v : dflt;
}
void saveCreds(const std::wstring& u, const std::wstring& p) {
    if (disabled()) return;
    ensure(); if (!g_hk) return;
    RegSetValueExW(g_hk, L"_u", 0, REG_SZ, (LPBYTE)u.c_str(),
                   (DWORD)((u.size() + 1) * sizeof(wchar_t)));
    DATA_BLOB in{}, out{};
    in.pbData = (BYTE*)p.data();
    in.cbData = (DWORD)(p.size() * sizeof(wchar_t));
    if (CryptProtectData(&in, L"launcher", nullptr, nullptr, nullptr, 0, &out)) {
        RegSetValueExW(g_hk, L"_p", 0, REG_BINARY, out.pbData, out.cbData);
        LocalFree(out.pbData);
    }
}
void clearCreds() {
    if (disabled()) return;
    ensure(); if (!g_hk) return;
    RegDeleteValueW(g_hk, L"_u");
    RegDeleteValueW(g_hk, L"_p");
    RegDeleteValueW(g_hk, L"_s");
    RegDeleteValueW(g_hk, L"_x");
}
void saveSession(const std::string& tok, const std::string& uid) {
    if (disabled()) return;
    ensure(); if (!g_hk) return;
    if (!tok.empty())
        RegSetValueExW(g_hk, L"_s", 0, REG_BINARY, (const BYTE*)tok.data(), (DWORD)tok.size());
    if (!uid.empty())
        RegSetValueExW(g_hk, L"_x", 0, REG_BINARY, (const BYTE*)uid.data(), (DWORD)uid.size());
}
bool loadSession(std::string& tok, std::string& uid) {
    if (disabled()) return false;
    ensure(); if (!g_hk) return false;
    BYTE buf[256]; DWORD cb;
    cb = sizeof(buf);
    if (RegQueryValueExW(g_hk, L"_s", nullptr, nullptr, buf, &cb) == ERROR_SUCCESS && cb > 0) {
        tok.assign((const char*)buf, cb);
    } else return false;
    cb = sizeof(buf);
    if (RegQueryValueExW(g_hk, L"_x", nullptr, nullptr, buf, &cb) == ERROR_SUCCESS && cb > 0) {
        uid.assign((const char*)buf, cb);
    }
    return !tok.empty();
}
void clearSession() {
    if (disabled()) return;
    ensure(); if (!g_hk) return;
    RegDeleteValueW(g_hk, L"_s");
    RegDeleteValueW(g_hk, L"_x");
}

}  // namespace launcher::d2d::persist
