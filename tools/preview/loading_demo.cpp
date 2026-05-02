// Launcher 完整预览 demo - v3
// 改动 (vs v2):
//   * Loading 200x200 -> Main 640x400 (用户要求紧凑)
//   * 整个窗口可拖动（WM_NCHITTEST 默认 HTCAPTION，交互元素白名单返 HTCLIENT）
//   * 自绘 Input (替代 Win32 EDIT, 不再乱跳消失)
//   * 头像 hover 自动展开下拉 (无需点击) + scale-up 动画
//   * Lunching 只保留 CS2 一款 (用户要求)
//   * topbar 36px / sidebar 56px (icon-only + tooltip) 节省空间
//   * 全局字体 Microsoft YaHei UI (中文 ClearType 锯齿小)
//   * Font 对象预创建复用 (帧率)
//
// 快捷键: D 主题 / 1-4 切 view / S 跳过 loading / H 历史登录 / Esc/右键 退出

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <wincrypt.h>
#include <vector>
#include <string>
#include <functional>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <unordered_map>
#include <map>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "user32.lib")

using namespace Gdiplus;

// ====================================================================
// 全局字体 (中文 + 英文都用 YaHei UI；日文系统会自动 fallback)
// ====================================================================
const wchar_t* kFontFace = L"Microsoft YaHei UI";

// ====================================================================
// Design tokens
// ====================================================================
// design tokens 直接对齐 Claude Design styles.css
struct Palette {
    Color bg, surface, card, divider;
    Color primary, primary_hover;
    Color text, text_muted, text_faint;
    Color sidebar_bg, sidebar_active;
    Color shadow_card, shadow_card_hover;
    Color overlay_dim;
    Color status_online;     // #4ADE80
    Color status_busy;       // #E34B4B
    Color status_away;       // #F5A524
    Color status_sleep;      // #8B7BD9
    Color status_offline;    // #6B6A67
};
const Palette kLight = {
    Color(255, 0xFA, 0xF7, 0xF2), Color(255, 0xF3, 0xEF, 0xE8),
    Color(255, 0xFF, 0xFF, 0xFF), Color(255, 0xED, 0xE9, 0xE1),
    Color(255, 0xC9, 0x64, 0x42), Color(255, 0xD9, 0x77, 0x57),
    Color(255, 0x1F, 0x1E, 0x1D), Color(255, 0x6B, 0x6A, 0x67),
    Color(255, 0xA8, 0xA3, 0x9A),
    Color(255, 0xFA, 0xF7, 0xF2), Color(255, 0xE9, 0xE1, 0xD3),
    Color( 10, 0, 0, 0),          Color( 20, 0, 0, 0),
    Color(115, 0, 0, 0),
    Color(255, 0x4A, 0xDE, 0x80), Color(255, 0xE3, 0x4B, 0x4B),
    Color(255, 0xF5, 0xA5, 0x24), Color(255, 0x8B, 0x7B, 0xD9),
    Color(255, 0x6B, 0x6A, 0x67)
};
const Palette kDark = {
    Color(255, 0x1A, 0x18, 0x16), Color(255, 0x20, 0x1E, 0x1B),
    Color(255, 0x24, 0x22, 0x20), Color(255, 0x36, 0x32, 0x2D),
    Color(255, 0xD9, 0x77, 0x57), Color(255, 0xE5, 0x86, 0x66),
    Color(255, 0xF5, 0xF1, 0xEA), Color(255, 0xA8, 0xA3, 0x9A),
    Color(255, 0x6B, 0x6A, 0x67),
    Color(255, 0x1A, 0x18, 0x16), Color(255, 0x36, 0x30, 0x29),
    Color( 60, 0, 0, 0),          Color( 92, 0, 0, 0),
    Color(115, 0, 0, 0),
    Color(255, 0x4A, 0xDE, 0x80), Color(255, 0xE3, 0x4B, 0x4B),
    Color(255, 0xF5, 0xA5, 0x24), Color(255, 0x8B, 0x7B, 0xD9),
    Color(255, 0x6B, 0x6A, 0x67)
};
bool g_dark = false;
const Palette& palette() { return g_dark ? kDark : kLight; }

// ====================================================================
// Curves
// ====================================================================
namespace curve {
inline float easeOutQuint(float t) { float i=1-t; return 1-i*i*i*i*i; }
inline float easeOutCubic(float t) { float i=1-t; return 1-i*i*i; }
inline float easeOutBack(float t)  {
    const float c1=1.70158f, c3=c1+1; float i=t-1;
    return 1 + c3*i*i*i + c1*i*i;
}
}

// ====================================================================
// I18N
// ====================================================================
enum class Lang { En, ZhCN, JaJP };
Lang g_lang = Lang::ZhCN;

const char* tr(const char* key) {
    struct E { const char* k; const char* en; const char* cn; const char* ja; };
    static const E T[] = {
        {"app.name",            "Launcher",         u8"启动器",            u8"ランチャー"},
        {"app.tagline",         "Private",          u8"私人启动器",        u8"プライベート"},
        {"loading.connecting",  "Connecting...",    u8"连接中…",           u8"接続中…"},
        {"home.greet",          "Hello, {nickname}",u8"你好，{nickname}",  u8"こんにちは、{nickname}"},
        {"home.subtitle",       "Welcome back",     u8"欢迎回来",          u8"おかえり"},
        {"home.tier",           "Plan",             u8"档位",              u8"プラン"},
        {"home.expires",        "Expires",          u8"到期",              u8"期限"},
        {"home.device_id",      "Device",           u8"设备",              u8"デバイス"},
        {"home.last_login",     "Last sign in",     u8"上次登录",          u8"前回"},
        {"home.history",        "History",          u8"历史",              u8"履歴"},
        {"menu.home",           "Home",             u8"主页",              u8"ホーム"},
        {"menu.lunching",       "Lunching",         u8"Lunching",          u8"Lunching"},
        {"menu.chat",           "Chat",             u8"聊天",              u8"チャット"},
        {"menu.cloud",          "Cloud",            u8"云端",              u8"クラウド"},
        {"menu.settings",       "Settings",         u8"设置",              u8"設定"},
        {"acc.profile",         "Profile",          u8"个人信息",          u8"プロフィール"},
        {"acc.history",         "Login history",    u8"历史登录",          u8"履歴"},
        {"acc.nickname",        "Edit nickname",    u8"修改昵称",          u8"ニックネーム"},
        {"acc.avatar",          "Change avatar",    u8"修改头像",          u8"アバター"},
        {"acc.password",        "Change password",  u8"修改密码",          u8"パスワード"},
        {"acc.signout",         "Sign out",         u8"退出登录",          u8"サインアウト"},
        {"lunching.launch",     "Launch",           u8"发射",              u8"起動"},
        {"library.count",       "games",            u8"个游戏",        u8"件"},
        {"library.launch",      "Launch",           u8"启动",              u8"起動"},
        {"settings.about",      "About",            u8"关于",              u8"情報"},
        {"profile.title",       "Profile",          u8"个人信息",          u8"プロフィール"},
        {"profile.uid",         "UID",              u8"UID",               u8"UID"},
        {"profile.username",    "Username",         u8"用户名",            u8"ユーザー名"},
        {"profile.nickname",    "Nickname",         u8"昵称",              u8"ニックネーム"},
        {"profile.change_pw",   "Change password",  u8"修改密码",          u8"パスワード変更"},
        {"profile.upload_avatar","Upload avatar",   u8"上传头像",          u8"アバター"},
        {"hist.title",          "Login history",    u8"历史登录",          u8"ログイン履歴"},
        {"hist.success",        "OK",               u8"成功",              u8"成功"},
        {"hist.failed",         "Failed",           u8"失败",              u8"失敗"},
        {"settings.language",   "Language",         u8"语言",              u8"言語"},
        {"settings.theme",      "Theme",            u8"主题",              u8"テーマ"},
        {"settings.theme_light","Light",            u8"亮",                u8"ライト"},
        {"settings.theme_dark", "Dark",             u8"暗",                u8"ダーク"},
        {"cloud.title",         "Cloud",            u8"云端",              u8"クラウド"},
        {"cloud.no_data",       "Empty",            u8"暂无",              u8"なし"},
        {"tier.1week",          "1 week",           u8"1 周",              u8"1週間"},
        {"auth.login.title",    "Sign in",          u8"登录到 Launcher",    u8"Launcher にログイン"},
        {"auth.login.sub",      "Welcome back",     u8"输入用户名和密码",   u8"ユーザー名とパスワード"},
        {"auth.register.title", "Create account",   u8"创建账号",          u8"アカウント作成"},
        {"auth.register.sub",   "with invite code", u8"用邀请码注册",      u8"招待コードで作成"},
        {"auth.username",       "Username",         u8"用户名",            u8"ユーザー名"},
        {"auth.password",       "Password",         u8"密码",              u8"パスワード"},
        {"auth.invite",         "Invite code",      u8"邀请码",            u8"招待コード"},
        {"auth.login",          "Sign in",          u8"登录",              u8"ログイン"},
        {"auth.register",       "Create",           u8"创建账号",          u8"作成"},
        {"auth.to_register",    "Need an account?", u8"还没有账号？",       u8"アカウントなし？"},
        {"auth.to_login",       "Have an account?", u8"已有账号？",         u8"アカウント済？"},
        {"auth.go_register",    "Sign up",          u8"去注册",            u8"登録"},
        {"auth.go_login",       "Sign in",          u8"去登录",            u8"ログイン"},
        {"auth.busy",           "Connecting…",      u8"正在连接…",          u8"接続中…"},
        {nullptr,nullptr,nullptr,nullptr}
    };
    for (int i = 0; T[i].k; ++i) {
        if (strcmp(T[i].k, key) == 0) {
            switch (g_lang) {
                case Lang::En:   return T[i].en;
                case Lang::ZhCN: return T[i].cn;
                case Lang::JaJP: return T[i].ja;
            }
        }
    }
    return key;
}
void detectSystemLanguage() {
    wchar_t buf[LOCALE_NAME_MAX_LENGTH] = {0};
    if (GetUserDefaultLocaleName(buf, LOCALE_NAME_MAX_LENGTH) > 0) {
        std::wstring w(buf);
        if (w.find(L"zh") == 0)      g_lang = Lang::ZhCN;
        else if (w.find(L"ja") == 0) g_lang = Lang::JaJP;
        else                         g_lang = Lang::En;
    }
}

// ====================================================================
// Persist — 隐秘注册表持久化（lang/theme/凭据）
//   策略：写死 30 个候选路径（伪装成系统/Office/MuiCache 子键），启动遍历找
//   _m magic = 'LUNC' 的那一个。没找到就按 GetTickCount 随机选一个写入。
//   凭据用 DPAPI（CryptProtectData，绑定当前用户）加密存 _p 字段。
// ====================================================================
namespace persist {
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

HKEY g_hk = nullptr;
const wchar_t* g_path = nullptr;

bool readDword(HKEY hk, const wchar_t* name, DWORD& out) {
    DWORD cb = sizeof(out), type = 0;
    return RegQueryValueExW(hk, name, nullptr, &type, (LPBYTE)&out, &cb) == ERROR_SUCCESS
           && type == REG_DWORD;
}
bool readString(HKEY hk, const wchar_t* name, std::wstring& out) {
    wchar_t buf[2048]; DWORD cb = sizeof(buf);
    if (RegQueryValueExW(hk, name, nullptr, nullptr, (LPBYTE)buf, &cb) != ERROR_SUCCESS) return false;
    if (cb < sizeof(wchar_t)) return false;
    out.assign(buf, (cb / sizeof(wchar_t)) - 1);
    return true;
}
bool tryOpen(const wchar_t* path) {
    HKEY hk;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, path, 0, KEY_READ | KEY_WRITE | KEY_SET_VALUE, &hk) != ERROR_SUCCESS) {
        return false;
    }
    DWORD m = 0;
    if (readDword(hk, L"_m", m) && m == kMagic) {
        g_hk = hk;
        g_path = path;
        return true;
    }
    RegCloseKey(hk);
    return false;
}
void ensure() {
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
    ensure(); if (!g_hk) return;
    DWORD d = (DWORD)v;
    RegSetValueExW(g_hk, L"_l", 0, REG_DWORD, (LPBYTE)&d, sizeof(d));
}
void saveTheme(bool dark) {
    ensure(); if (!g_hk) return;
    DWORD d = dark ? 1 : 0;
    RegSetValueExW(g_hk, L"_t", 0, REG_DWORD, (LPBYTE)&d, sizeof(d));
}
int loadLang(int dflt) {
    ensure(); if (!g_hk) return dflt;
    DWORD v; return readDword(g_hk, L"_l", v) ? (int)v : dflt;
}
bool loadTheme(bool dflt) {
    ensure(); if (!g_hk) return dflt;
    DWORD v; return readDword(g_hk, L"_t", v) ? (v != 0) : dflt;
}
void saveCreds(const std::wstring& u, const std::wstring& p) {
    ensure(); if (!g_hk) return;
    RegSetValueExW(g_hk, L"_u", 0, REG_SZ, (LPBYTE)u.c_str(), (DWORD)((u.size() + 1) * sizeof(wchar_t)));
    DATA_BLOB in{}, out{};
    in.pbData = (BYTE*)p.data();
    in.cbData = (DWORD)(p.size() * sizeof(wchar_t));
    if (CryptProtectData(&in, L"launcher", nullptr, nullptr, nullptr, 0, &out)) {
        RegSetValueExW(g_hk, L"_p", 0, REG_BINARY, out.pbData, out.cbData);
        LocalFree(out.pbData);
    }
}
bool loadCreds(std::wstring& u, std::wstring& p) {
    ensure(); if (!g_hk) return false;
    if (!readString(g_hk, L"_u", u) || u.empty()) return false;
    BYTE buf[4096]; DWORD cb = sizeof(buf), type = 0;
    if (RegQueryValueExW(g_hk, L"_p", nullptr, &type, buf, &cb) != ERROR_SUCCESS) return false;
    if (type != REG_BINARY || cb == 0) return false;
    DATA_BLOB in{}, out{};
    in.pbData = buf; in.cbData = cb;
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, 0, &out)) return false;
    p.assign((wchar_t*)out.pbData, out.cbData / sizeof(wchar_t));
    LocalFree(out.pbData);
    return true;
}
void clearCreds() {
    ensure(); if (!g_hk) return;
    RegDeleteValueW(g_hk, L"_u");
    RegDeleteValueW(g_hk, L"_p");
}
}  // namespace persist

// ====================================================================
// Tween
// ====================================================================
struct Tween {
    float from{0}, to{0}, duration{0.2f}, delay{0}, elapsed{0};
    float (*c)(float){curve::easeOutQuint};
    bool started{false};
    void start(float f, float t, float d, float dl=0, float (*cv)(float)=curve::easeOutQuint) {
        from=f; to=t; duration=d; delay=dl; elapsed=-dl; c=cv; started=true;
    }
    void tick(float dt) { if (started) elapsed += dt; }
    bool done() const { return started && elapsed >= duration; }
    float value() const {
        if (!started || elapsed <= 0) return from;
        if (elapsed >= duration) return to;
        return from + (to - from) * c(elapsed / duration);
    }
};

// ====================================================================
// 状态
// ====================================================================
// 入场流程：Dot → ExpandLoading → Loading → ExpandAuth → Auth → ExpandMain → Main
enum class Stage { Dot, ExpandLoading, Loading, Expanding, ExpandAuth, Auth, ExpandMain, Main };
enum class AuthMode { Login, Register };
enum class View  { Home, Lunching, Chat, Cloud, Settings, Profile };
enum class Overlay { None, History };

Stage    g_stage    = Stage::Dot;
AuthMode g_auth_mode = AuthMode::Login;
bool     g_skip_auth_after_loading = false;   // auto_login 模式标记
View     g_view  = View::Home;
Overlay  g_overlay = Overlay::None;
bool     g_account_dropdown = false;

Tween g_card_scale, g_card_opacity, g_card_fade_out;
Tween g_window_w, g_window_h;
Tween g_sidebar_x, g_topbar_y, g_main_opacity;
Tween g_view_fade;
Tween g_dropdown_t;
Tween g_overlay_t;
// Dot 阶段：起始小点
Tween g_dot_size, g_dot_alpha;
Tween g_auth_card_y, g_auth_card_op;

float g_time_in_stage = 0.0f;
float g_spin_angle = 0.0f;
HWND  g_hwnd = nullptr;
POINT g_mouse{-1, -1};

struct UserInfo {
    const wchar_t* uid       = L"3277380";
    const wchar_t* username  = L"dwgx";
    const wchar_t* nickname  = L"dwgx";
    const wchar_t* email     = L"dwgx1337@outlook.com";
    const wchar_t* device_id = L"f8a1c2d4...e5b6";
    const wchar_t* expires   = L"2026-05-09";
    const wchar_t* last_login= L"05-02 10:32";
} g_user;

// Steam 集成 — 从 HKCU\Software\Valve\Steam 读 PersonaName + LastGameNameUsed。
// 游玩时长精确值要 Steam Web API（需 key），暂占位。
struct SteamInfo {
    std::wstring persona;        // 当前 Steam 账号名
    std::wstring last_game;      // Steam 记录的"上次玩的游戏名"
    std::wstring last_played;    // CS2 上次启动时间（占位 — 真值在 vdf 里）
    std::wstring playtime_label; // CS2 总时长（占位）
    bool resolved = false;
} g_steam;

void readSteamInfo() {
    if (g_steam.resolved) return;
    g_steam.resolved = true;
    HKEY hk;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", 0, KEY_READ, &hk) == ERROR_SUCCESS) {
        wchar_t buf[256]; DWORD cb;
        // AutoLoginUser = Steam 登录账号名（可读，例如 "dwgx1337"）
        cb = sizeof(buf);
        if (RegQueryValueExW(hk, L"AutoLoginUser", nullptr, nullptr, (LPBYTE)buf, &cb) == ERROR_SUCCESS) {
            g_steam.persona = buf;
        }
        // LastGameNameUsed = 上次玩的游戏名（可读，例如 "Counter-Strike 2"）
        cb = sizeof(buf);
        if (RegQueryValueExW(hk, L"LastGameNameUsed", nullptr, nullptr, (LPBYTE)buf, &cb) == ERROR_SUCCESS) {
            g_steam.last_game = buf;
        }
        RegCloseKey(hk);
    }
    if (g_steam.persona.empty()) g_steam.persona = L"未登录";
    // 上次启动时间 / 总时长 — Steam 不在 reg 暴露（在 vdf 文件里），
    // 显示成"上次玩的游戏"信息更直接：
    if (g_steam.last_played.empty()) {
        g_steam.last_played = g_steam.last_game.empty() ? L"—" : g_steam.last_game;
    }
    if (g_steam.playtime_label.empty()) g_steam.playtime_label = L"—";
}

enum class UserStatus { Online, Busy, Away, Sleep, Offline };
UserStatus g_status = UserStatus::Online;
bool g_status_fold_open = false;
Tween g_status_fold_t;

// 托盘 ID + 自定义 message
constexpr UINT kTrayCallbackMsg = WM_APP + 100;
constexpr UINT kTrayUid = 1;
NOTIFYICONDATAW g_nid{};
bool g_tray_added = false;
HMENU g_tray_menu = nullptr;
const wchar_t* statusKey(UserStatus s) {
    switch (s) {
    case UserStatus::Online:  return L"online";
    case UserStatus::Busy:    return L"busy";
    case UserStatus::Away:    return L"away";
    case UserStatus::Sleep:   return L"sleep";
    default:                  return L"offline";
    }
}
const wchar_t* statusLabel(UserStatus s, Lang lang) {
    static const wchar_t* zh[] = { L"在线", L"繁忙", L"离开", L"睡眠", L"离线" };
    static const wchar_t* en[] = { L"Online", L"Busy", L"Away", L"Sleeping", L"Offline" };
    static const wchar_t* ja[] = { L"オンライン", L"取り込み中", L"離席中", L"おやすみ", L"オフライン" };
    int i = (int)s;
    if (lang == Lang::En) return en[i];
    if (lang == Lang::JaJP) return ja[i];
    return zh[i];
}

// ====================================================================
// 自绘 Input (替代 Win32 EDIT)
// ====================================================================
struct InputBox {
    std::wstring text;
    int  cursor = 0;
    int  sel_anchor = -1;   // -1 = 无选区
    bool password = false;
    RectF bounds{};
    Tween float_t;

    bool hasSelection() const { return sel_anchor >= 0 && sel_anchor != cursor; }
    int  selStart() const { return std::min(sel_anchor, cursor); }
    int  selEnd()   const { return std::max(sel_anchor, cursor); }
    void clearSel() { sel_anchor = -1; }
    void selectAll() { sel_anchor = 0; cursor = (int)text.size(); }

    void deleteSelection() {
        if (!hasSelection()) return;
        int s = selStart(), e = selEnd();
        text.erase(s, e - s);
        cursor = s;
        clearSel();
    }
    void replaceSelection(const std::wstring& with) {
        deleteSelection();
        text.insert(cursor, with);
        cursor += (int)with.size();
    }

    void copyToClipboard(HWND hwnd) {
        if (!hasSelection() || password) return;
        std::wstring s = text.substr(selStart(), selEnd() - selStart());
        if (!OpenClipboard(hwnd)) return;
        EmptyClipboard();
        size_t bytes = (s.size() + 1) * sizeof(wchar_t);
        HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (h) {
            memcpy(GlobalLock(h), s.c_str(), bytes);
            GlobalUnlock(h);
            SetClipboardData(CF_UNICODETEXT, h);
        }
        CloseClipboard();
    }
    void pasteFromClipboard(HWND hwnd) {
        if (!OpenClipboard(hwnd)) return;
        HANDLE h = GetClipboardData(CF_UNICODETEXT);
        if (h) {
            const wchar_t* p = (const wchar_t*)GlobalLock(h);
            if (p) {
                std::wstring in = p;
                // 剥掉 \r 让换行统一
                std::wstring filt;
                filt.reserve(in.size());
                for (wchar_t c : in) if (c != L'\r') filt.push_back(c);
                replaceSelection(filt);
                GlobalUnlock(h);
            }
        }
        CloseClipboard();
    }
    void cutToClipboard(HWND hwnd) {
        if (!hasSelection() || password) return;
        copyToClipboard(hwnd);
        deleteSelection();
    }

    // c 是 WM_CHAR 字符；ctrl 表示 Ctrl 当前按下
    // 返回 true = 已处理（调用方不要再 PostMessage）
    bool onChar(wchar_t c, bool ctrl, HWND hwnd) {
        if (ctrl) {
            if (c == 0x01) { selectAll(); return true; }                  // Ctrl+A
            if (c == 0x03) { copyToClipboard(hwnd); return true; }        // Ctrl+C
            if (c == 0x16) { pasteFromClipboard(hwnd); return true; }     // Ctrl+V
            if (c == 0x18) { cutToClipboard(hwnd); return true; }         // Ctrl+X
            if (c == 0x1A) return true;                                    // Ctrl+Z (TODO)
            return true;   // 其他 Ctrl 组合吃掉
        }
        if (c == 0x08) { // backspace
            if (hasSelection()) deleteSelection();
            else if (cursor > 0) { text.erase(cursor - 1, 1); cursor--; }
            return true;
        }
        if (c == 0x09 || c == 0x1B) return false;   // Tab/Esc 让上层处理
        if (c == 0x0A || c == 0x0D) return false;   // Enter 上层
        if (c >= 0x20) {
            replaceSelection(std::wstring(1, c));
            return true;
        }
        return false;
    }
    void onKey(int vk, bool shift, bool ctrl) {
        bool moved = false;
        if (vk == VK_LEFT) {
            if (shift && sel_anchor < 0) sel_anchor = cursor;
            if (cursor > 0) { cursor--; moved = true; }
            if (!shift) clearSel();
        } else if (vk == VK_RIGHT) {
            if (shift && sel_anchor < 0) sel_anchor = cursor;
            if (cursor < (int)text.size()) { cursor++; moved = true; }
            if (!shift) clearSel();
        } else if (vk == VK_HOME) {
            if (shift && sel_anchor < 0) sel_anchor = cursor;
            cursor = 0; moved = true;
            if (!shift) clearSel();
        } else if (vk == VK_END) {
            if (shift && sel_anchor < 0) sel_anchor = cursor;
            cursor = (int)text.size(); moved = true;
            if (!shift) clearSel();
        } else if (vk == VK_DELETE) {
            if (hasSelection()) deleteSelection();
            else if (cursor < (int)text.size()) text.erase(cursor, 1);
        }
        if (moved && shift) {
            // sel_anchor 已设；cursor 已动；选区自动 = (anchor, cursor)
        } else if (moved && !shift) {
            // 已 clearSel 了
        }
        (void)ctrl;
    }
    bool hit(POINT p) const {
        return p.x >= bounds.X && p.x <= bounds.X + bounds.Width
            && p.y >= bounds.Y && p.y <= bounds.Y + bounds.Height;
    }
    std::wstring display() const {
        if (!password) return text;
        return std::wstring(text.size(), L'•');
    }
    std::wstring displaySlice(int from, int to) const {
        from = std::max(0, std::min(from, (int)text.size()));
        to   = std::max(0, std::min(to,   (int)text.size()));
        if (from >= to) return L"";
        if (!password) return text.substr(from, to - from);
        return std::wstring(to - from, L'•');
    }
};

struct AuthForm {
    InputBox username, password, invite;
    int  focus = 0;     // 0=username 1=password 2=invite
    bool busy = false;
    std::wstring error_msg;
} g_auth_form;

// ====================================================================
// 工具
// ====================================================================
// Font cache — 关键性能优化。GDI+ 每次创建 Font 会触发 GDI 字体匹配，
// 每帧重建会卡到飞起。按 (size, style) 缓存指针，运行期不释放。
namespace fontcache {
struct Key {
    int size_q;
    int style;
    bool operator==(const Key& o) const { return size_q == o.size_q && style == o.style; }
};
struct KeyHash { size_t operator()(const Key& k) const {
    return std::hash<int>()(k.size_q) ^ (std::hash<int>()(k.style) << 1); } };
inline std::unordered_map<Key, Font*, KeyHash>& cacheMap() {
    static std::unordered_map<Key, Font*, KeyHash> g; return g;
}
inline Font* get(float size_pt, FontStyle style = FontStyleRegular) {
    Key k{ (int)(size_pt * 4.0f + 0.5f), (int)style };
    auto& m = cacheMap();
    auto it = m.find(k);
    if (it != m.end()) return it->second;
    Font* f = new Font(kFontFace, size_pt, style, UnitPoint);
    m.emplace(k, f);
    return f;
}
}  // namespace fontcache

void buildRoundRect(GraphicsPath& p, REAL x, REAL y, REAL w, REAL h, REAL r) {
    p.Reset();
    p.AddArc(x, y, r*2, r*2, 180, 90);
    p.AddArc(x+w-r*2, y, r*2, r*2, 270, 90);
    p.AddArc(x+w-r*2, y+h-r*2, r*2, r*2, 0, 90);
    p.AddArc(x, y+h-r*2, r*2, r*2, 90, 90);
    p.CloseFigure();
}
void fillRR(Graphics& g, REAL x, REAL y, REAL w, REAL h, REAL r, Color c) {
    GraphicsPath p; buildRoundRect(p, x, y, w, h, r);
    SolidBrush b(c); g.FillPath(&b, &p);
}
void strokeRR(Graphics& g, REAL x, REAL y, REAL w, REAL h, REAL r, Color c, REAL stroke=1.0f) {
    GraphicsPath p; buildRoundRect(p, x, y, w, h, r);
    Pen pen(c, stroke); g.DrawPath(&pen, &p);
}
void drawShadow(Graphics& g, REAL x, REAL y, REAL w, REAL h, REAL r,
                Color base, REAL dy, int spread) {
    for (int i = spread; i > 0; --i) {
        BYTE a = (BYTE)(base.GetA() / i);
        Color c(a, base.GetR(), base.GetG(), base.GetB());
        GraphicsPath p; buildRoundRect(p, x-i, y+dy+i*0.4f, w+i*2, h+i*2, r+i);
        SolidBrush b(c); g.FillPath(&b, &p);
    }
}
void drawText_(Graphics& g, const wchar_t* text, REAL x, REAL y, REAL w,
               float size, Color color,
               StringAlignment ha = StringAlignmentNear, FontStyle fs = FontStyleRegular) {
    SolidBrush b(color);
    StringFormat fmt; fmt.SetAlignment(ha);
    RectF r(x, y, w, size * 3);
    g.DrawString(text, -1, fontcache::get(size, fs), r, &fmt, &b);
}
RectF measureText(Graphics& g, const wchar_t* text, float size, FontStyle fs = FontStyleRegular) {
    RectF bbox;
    g.MeasureString(text, -1, fontcache::get(size, fs), PointF(0, 0), &bbox);
    return bbox;
}
std::wstring W(const char* utf8) {
    if (!utf8) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    std::wstring w(n - 1, 0);
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w.data(), n);
    return w;
}
struct HitArea { RectF rect; std::function<void()> on_click; bool draggable_off = true; };
std::vector<HitArea> g_hits;
void hit(RectF r, std::function<void()> fn, bool drag_off = true) {
    g_hits.push_back({r, std::move(fn), drag_off});
}
bool inRect(POINT p, RectF r) {
    return p.x >= r.X && p.x <= r.X + r.Width && p.y >= r.Y && p.y <= r.Y + r.Height;
}
std::wstring replaceNick(const wchar_t* tmpl, const wchar_t* v) {
    std::wstring s(tmpl);
    auto pos = s.find(L"{nickname}");
    if (pos != std::wstring::npos) s.replace(pos, 10, v);
    return s;
}
void switchView(View v) {
    if (v == g_view) return;
    g_view = v;
    g_view_fade.start(0.0f, 1.0f, 0.25f, 0.0f, curve::easeOutQuint);
}

// ====================================================================
// Loading
// ====================================================================
void paintLoading(Graphics& g, int Wpx, int Hpx) {
    const Palette& pal = palette();
    SolidBrush bg(pal.bg);
    g.FillRectangle(&bg, 0, 0, Wpx, Hpx);

    float scale = g_card_scale.value();
    float op    = g_card_opacity.value();
    float exit  = g_card_fade_out.value();
    float opacity = op * (1.0f - exit);
    if (opacity <= 0.001f) return;

    const float cw = 168.0f * scale, ch = 168.0f * scale;
    const float cx = (Wpx - cw) / 2.0f;
    const float cy = (Hpx - ch) / 2.0f - 14.0f * exit;

    Color cardC((BYTE)(opacity * 255), pal.card.GetR(), pal.card.GetG(), pal.card.GetB());
    Color shC((BYTE)(pal.shadow_card_hover.GetA() * opacity), 0, 0, 0);
    drawShadow(g, cx, cy, cw, ch, 12.0f, shC, 4.0f, 4);
    fillRR(g, cx, cy, cw, ch, 12.0f, cardC);

    const float spinR = 22.0f * scale;
    const float scx = cx + cw * 0.5f, scy = cy + ch * 0.5f - 10.0f * scale;
    Color spinC((BYTE)(opacity * 255), pal.primary.GetR(), pal.primary.GetG(), pal.primary.GetB());
    Pen pen(spinC, 3.0f);
    pen.SetStartCap(LineCapRound); pen.SetEndCap(LineCapRound);
    g.DrawArc(&pen, scx-spinR, scy-spinR, spinR*2, spinR*2, g_spin_angle, 80.0f);

    Color tc((BYTE)(opacity * 255), pal.text_muted.GetR(), pal.text_muted.GetG(), pal.text_muted.GetB());
    drawText_(g, W(tr("loading.connecting")).c_str(),
              cx, cy + ch - 42.0f, cw, 9.0f, tc, StringAlignmentCenter);
}

// ====================================================================
// Topbar / Sidebar (Claude Design 规格: 200 + 48)
// ====================================================================
const float kSidebarW = 200.0f;
const float kTopbarH  = 48.0f;

// 包含子模块（依赖 palette / Color / Tween / drawText_ / fillRR / hit / g_mouse）
#include "icons.inl"
#include "modals.inl"
#include "market_view.inl"
#include "chat_view.inl"

struct MenuEntry { View view; const char* key; icons::Name icon; };
const MenuEntry kMenu[] = {
    { View::Home,     "menu.home",     icons::Name::Home },
    { View::Lunching, "menu.lunching", icons::Name::Library },
    { View::Chat,     "menu.chat",     icons::Name::Chat },
    { View::Cloud,    "menu.cloud",    icons::Name::Cloud },
    { View::Settings, "menu.settings", icons::Name::Settings },
};

void paintTopbar(Graphics& g, int Wpx) {
    const Palette& pal = palette();
    float ty = -kTopbarH * (1.0f - g_topbar_y.value());

    SolidBrush bg(pal.bg);
    g.FillRectangle(&bg, 0.0f, ty, (REAL)Wpx, kTopbarH);
    Pen sep(pal.divider, 1.0f);
    g.DrawLine(&sep, 0.0f, ty + kTopbarH, (REAL)Wpx, ty + kTopbarH);

    // 标题 (design: 13px 500 weight, padding-left 20)
    drawText_(g, W(tr("app.name")).c_str(), 20.0f, ty + 14.0f, 200.0f, 10.0f,
              pal.text, StringAlignmentNear, FontStyleRegular);

    // 右上 user-trigger pill: padding 4 6 4 10, gap 10, radius 999
    // 内容: nickname (13px muted) + avatar 24x24
    float pill_h = 32.0f;
    float ar = 12.0f;   // avatar radius (24x24)
    float pill_pad_l = 12.0f, pill_pad_r = 4.0f, pill_gap = 10.0f;
    // 估算 nickname 宽度
    float name_w = measureText(g, g_user.nickname, 10.0f).Width + 4.0f;
    float pill_w = pill_pad_l + name_w + pill_gap + ar*2 + pill_pad_r;
    float pill_x = (REAL)Wpx - 16.0f - pill_w;
    float pill_y = ty + (kTopbarH - pill_h) / 2.0f;
    bool pill_hover = inRect(g_mouse, RectF(pill_x, pill_y, pill_w, pill_h));
    if (pill_hover) {
        Color hc(g_dark ? 12 : 10, pal.text.GetR(), pal.text.GetG(), pal.text.GetB());
        fillRR(g, pill_x, pill_y, pill_w, pill_h, 999.0f, hc);
    }
    drawText_(g, g_user.nickname, pill_x + pill_pad_l, pill_y + 10.0f, name_w, 10.0f,
              pal.text_muted, StringAlignmentNear);
    float ax = pill_x + pill_pad_l + name_w + pill_gap;
    float ay = pill_y + (pill_h - ar*2) / 2.0f;
    SolidBrush avbg(pal.primary);
    g.FillEllipse(&avbg, ax, ay, ar*2, ar*2);
    Font af(kFontFace, 8.5f, FontStyleBold, UnitPoint);
    SolidBrush avf(Color(255, 255, 255, 255));
    StringFormat fmt; fmt.SetAlignment(StringAlignmentCenter); fmt.SetLineAlignment(StringAlignmentCenter);
    RectF avrect(ax, ay, ar*2, ar*2);
    wchar_t initial[2] = { (wchar_t)towupper(g_user.nickname[0]), 0 };
    g.DrawString(initial, -1, &af, avrect, &fmt, &avf);
    // 在线徽章
    Color stC = pal.status_online;
    SolidBrush stB(stC);
    g.FillEllipse(&stB, ax + ar*2 - 7.0f, ay + ar*2 - 7.0f, 7.0f, 7.0f);
    Pen ring(pal.bg, 2.0f);
    g.DrawEllipse(&ring, ax + ar*2 - 7.0f, ay + ar*2 - 7.0f, 7.0f, 7.0f);

    // 点击 trigger 切换 dropdown（不再 hover 自动展开 — 之前会"收不回去"）
    RectF avHit(pill_x, ty, pill_w + 16.0f, kTopbarH);
    hit(avHit, [](){
        g_account_dropdown = !g_account_dropdown;
        g_dropdown_t.start(g_dropdown_t.value(),
                           g_account_dropdown ? 1.0f : 0.0f,
                           0.18f, 0, curve::easeOutBack);
    }, true);
}

void paintAccountDropdown(Graphics& g, int Wpx) {
    if (!g_account_dropdown && g_dropdown_t.value() < 0.001f) return;
    const Palette& pal = palette();

    float t = g_dropdown_t.value();
    float dw = 240.0f;
    float fold_extra = g_status_fold_t.value() * 124.0f;   // 5 * 24 + 4
    float dh = 240.0f + fold_extra;
    float dx = Wpx - 6.0f - dw;
    float dy = kTopbarH + 4.0f - 6.0f * (1.0f - t);
    float scale = 0.94f + 0.06f * t;
    BYTE a = (BYTE)(255 * t);
    if (a == 0) return;

    auto fade = [&](Color c) { return Color((BYTE)(c.GetA() * t), c.GetR(), c.GetG(), c.GetB()); };

    float ox = dx + dw, oy = dy;
    g.TranslateTransform(ox, oy);
    g.ScaleTransform(scale, scale);
    g.TranslateTransform(-ox, -oy);

    Color cardC(a, pal.card.GetR(), pal.card.GetG(), pal.card.GetB());
    drawShadow(g, dx, dy, dw, dh, 12.0f, fade(pal.shadow_card_hover), 6.0f, 4);
    fillRR(g, dx, dy, dw, dh, 12.0f, cardC);
    strokeRR(g, dx, dy, dw, dh, 12.0f, fade(pal.divider));

    // 顶部 header — avatar + nickname + email
    float ar = 14;
    SolidBrush avbg(fade(pal.primary));
    g.FillEllipse(&avbg, dx + 12, dy + 12, ar*2, ar*2);
    Font af(kFontFace, 9.5f, FontStyleBold, UnitPoint);
    SolidBrush avf(Color((BYTE)(255 * t), 255, 255, 255));
    StringFormat avfmt; avfmt.SetAlignment(StringAlignmentCenter); avfmt.SetLineAlignment(StringAlignmentCenter);
    wchar_t init[2] = { (wchar_t)towupper(g_user.nickname[0]), 0 };
    g.DrawString(init, -1, &af, RectF(dx + 12, dy + 12, ar*2, ar*2), &avfmt, &avf);
    drawText_(g, g_user.nickname, dx + 50, dy + 12, dw - 60,
              10.5f, fade(pal.text), StringAlignmentNear, FontStyleBold);
    drawText_(g, g_user.email, dx + 50, dy + 28, dw - 60,
              7.5f, fade(pal.text_muted));

    Pen sep(fade(pal.divider), 1.0f);
    g.DrawLine(&sep, dx + 8, dy + 50, dx + dw - 8, dy + 50);

    // status fold trigger
    float iy = dy + 58;
    RectF strigger(dx + 6, iy, dw - 12, 30);
    bool shov = inRect(g_mouse, strigger);
    if (shov) {
        Color hc((BYTE)(20 * t), pal.text.GetR(), pal.text.GetG(), pal.text.GetB());
        fillRR(g, strigger.X, strigger.Y, strigger.Width, strigger.Height, 6, hc);
    }
    // status dot
    SolidBrush sd(fade(chatv::statusColor(pal, statusKey(g_status))));
    g.FillEllipse(&sd, dx + 14.0f, iy + 12.0f, 8.0f, 8.0f);
    drawText_(g, statusLabel(g_status, g_lang), dx + 30, iy + 8, dw - 60,
              9.0f, fade(pal.text));
    drawText_(g, g_status_fold_open ? L"▴" : L"▾", dx + dw - 22, iy + 8, 14,
              8.0f, fade(pal.text_muted));
    hit(strigger, [](){
        g_status_fold_open = !g_status_fold_open;
        g_status_fold_t.start(g_status_fold_t.value(), g_status_fold_open ? 1.0f : 0.0f,
                              0.22f, 0, curve::easeOutCubic);
    }, true);
    iy += 32;

    // status fold body
    if (g_status_fold_t.value() > 0.001f) {
        float ft = g_status_fold_t.value();
        UserStatus statuses[] = { UserStatus::Online, UserStatus::Busy, UserStatus::Away, UserStatus::Sleep, UserStatus::Offline };
        for (auto s : statuses) {
            if (s == g_status) continue;
            float row_y = iy;
            RectF r(dx + 14, row_y, dw - 28, 24 * ft);
            if (r.Height < 4) continue;
            bool hov = inRect(g_mouse, r);
            if (hov) {
                Color hc((BYTE)(20 * t * ft), pal.text.GetR(), pal.text.GetG(), pal.text.GetB());
                fillRR(g, r.X, r.Y, r.Width, r.Height, 4, hc);
            }
            SolidBrush dot(Color((BYTE)(t * ft * 255),
                                 chatv::statusColor(pal, statusKey(s)).GetR(),
                                 chatv::statusColor(pal, statusKey(s)).GetG(),
                                 chatv::statusColor(pal, statusKey(s)).GetB()));
            g.FillEllipse(&dot, dx + 18.0f, row_y + 8.0f * ft, 6.0f, 6.0f);
            Color tx_c((BYTE)(t * ft * pal.text.GetA()), pal.text.GetR(), pal.text.GetG(), pal.text.GetB());
            drawText_(g, statusLabel(s, g_lang), dx + 32, row_y + 4 * ft, dw - 60,
                      8.5f, tx_c);
            UserStatus target = s;
            hit(r, [target](){
                g_status = target;
                g_status_fold_open = false;
                g_status_fold_t.start(g_status_fold_t.value(), 0, 0.18f, 0, curve::easeOutCubic);
            }, true);
            iy += 24 * ft;
        }
        // 底部 sep
        if (ft > 0.5f) {
            Pen ssep(fade(pal.divider), 1.0f);
            g.DrawLine(&ssep, dx + 8, iy + 4, dx + dw - 8, iy + 4);
        }
        iy += 8;
    }

    // 菜单条目
    struct Item { const char* key; icons::Name icon; std::function<void()> click; bool danger; };
    Item items[] = {
        { "acc.profile",  icons::Name::User, [](){
              switchView(View::Profile); g_account_dropdown=false;
              g_dropdown_t.start(g_dropdown_t.value(),0,0.15f,0,curve::easeOutCubic); }, false },
        { "acc.history",  icons::Name::History, [](){
              g_overlay = Overlay::History;
              g_overlay_t.start(0,1,0.25f,0,curve::easeOutCubic);
              g_account_dropdown=false;
              g_dropdown_t.start(g_dropdown_t.value(),0,0.15f,0,curve::easeOutCubic); }, false },
        { "acc.password", icons::Name::Shield, [](){ }, false },
        { "acc.signout",  icons::Name::Logout, [](){
              // 清除存储的凭据，回到 Auth
              persist::clearCreds();
              g_account_dropdown = false;
              g_dropdown_t.start(g_dropdown_t.value(),0,0.15f,0,curve::easeOutCubic);
              // 回到 Auth：重置表单 + 切 stage
              g_auth_form.username.text.clear(); g_auth_form.username.cursor = 0; g_auth_form.username.clearSel();
              g_auth_form.password.text.clear(); g_auth_form.password.cursor = 0; g_auth_form.password.clearSel();
              g_auth_form.invite.text.clear();   g_auth_form.invite.cursor   = 0; g_auth_form.invite.clearSel();
              g_auth_form.focus = 0;
              g_auth_form.error_msg.clear();
              g_stage = Stage::Auth;
              // 缩窗到 Auth 卡片大小
              int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
              SetWindowPos(g_hwnd, nullptr, (sw - 480) / 2, (sh - 540) / 2, 480, 540, SWP_NOZORDER);
              g_auth_card_op.start(0, 1, 0.40f, 0.05f, curve::easeOutCubic);
              g_auth_card_y.start(12, 0, 0.45f, 0.05f, curve::easeOutQuint);
        }, true },
    };
    for (auto& it : items) {
        RectF r(dx + 6, iy, dw - 12, 30);
        bool hover = inRect(g_mouse, r);
        if (hover) {
            Color hc(it.danger ? (BYTE)(28 * t) : (BYTE)(20 * t),
                     it.danger ? 0xE3 : pal.text.GetR(),
                     it.danger ? 0x4B : pal.text.GetG(),
                     it.danger ? 0x4B : pal.text.GetB());
            fillRR(g, r.X, r.Y, r.Width, r.Height, 6, hc);
        }
        Color tc = it.danger ? fade(Color(255, 0xE3, 0x4B, 0x4B)) : fade(pal.text);
        icons::drawSvg(g, it.icon, r.X + 10, r.Y + 7, 16, tc);
        drawText_(g, W(tr(it.key)).c_str(), r.X + 32, r.Y + 8, r.Width - 40, 9.0f, tc);
        hit(r, it.click, true);
        iy += 32;
    }

    g.ResetTransform();
}

// 点击 dropdown 之外（且不在 trigger）→ 关闭
void registerDropdownDismissHits(int Wpx, int Hpx) {
    if (!g_account_dropdown && g_dropdown_t.value() < 0.001f) return;
    float dw = 240.0f;
    float dh = 240.0f + g_status_fold_t.value() * 124.0f;
    float dx = Wpx - 6.0f - dw;
    float dy = kTopbarH;
    auto dismiss = [](){
        g_account_dropdown = false;
        g_status_fold_open = false;
        g_dropdown_t.start(g_dropdown_t.value(), 0, 0.15f, 0, curve::easeOutCubic);
        g_status_fold_t.start(g_status_fold_t.value(), 0, 0.15f, 0, curve::easeOutCubic);
    };
    // 4 环形 hit 避开 dropdown 本身 + topbar trigger 区
    hit(RectF(0, 0, dx, kTopbarH), dismiss, true);                          // topbar 左侧
    hit(RectF(0, kTopbarH, dx, Hpx - kTopbarH), dismiss, true);             // dropdown 左侧 + 下
    hit(RectF(dx + dw, kTopbarH, Wpx - (dx + dw), Hpx - kTopbarH), dismiss, true);  // dropdown 右侧
    hit(RectF(dx, dy + dh, dw, Hpx - (dy + dh)), dismiss, true);            // dropdown 下方
}
// 兼容旧名字
void updateDropdownHover(int Wpx) { (void)Wpx; }

void paintSidebar(Graphics& g, int Hpx) {
    const Palette& pal = palette();
    float sx = -kSidebarW * (1.0f - g_sidebar_x.value());
    SolidBrush bg(pal.sidebar_bg);
    g.FillRectangle(&bg, sx, kTopbarH, kSidebarW, (REAL)Hpx - kTopbarH);
    Pen sep(pal.divider, 1.0f);
    g.DrawLine(&sep, sx + kSidebarW, kTopbarH, sx + kSidebarW, (REAL)Hpx);

    // design: padding 14px 10px, gap 4px, item h 38px, padding 0 12px, radius 8
    float my = kTopbarH + 14.0f;
    for (auto& m : kMenu) {
        bool active = (m.view == g_view);
        RectF item(sx + 10.0f, my, kSidebarW - 20.0f, 38.0f);
        bool hover = inRect(g_mouse, item);
        if (hover && !active) {
            // hover bg rgba(217,119,87,.08)
            Color hc(20, pal.primary.GetR(), pal.primary.GetG(), pal.primary.GetB());
            fillRR(g, item.X, item.Y, item.Width, item.Height, 8.0f, hc);
        }
        if (active) {
            // active 左侧 3px 主色指示条 (.menu-item.active::before)
            fillRR(g, item.X - 10.0f, item.Y + 9.0f, 3.0f, item.Height - 18.0f, 1.5f, pal.primary);
        }
        Color tc = active ? pal.primary : (hover ? pal.text : pal.text_muted);

        // SVG 图标 — 不依赖系统字体，避免 Yahei UI 渲染不出 ☁ ⚙ 这类 unicode
        icons::drawSvg(g, m.icon, item.X + 12.0f, item.Y + 9.0f, 20.0f, tc);
        drawText_(g, W(tr(m.key)).c_str(), item.X + 44.0f, item.Y + 11.0f, item.Width - 50.0f,
                  10.5f, tc, StringAlignmentNear, FontStyleRegular);

        View target = m.view;
        hit(item, [target]() { switchView(target); }, true);
        my += 42.0f;   // 38 height + 4 gap
    }
}

// ====================================================================
// Views (640x400 紧凑 layout)
// ====================================================================
void paintHomeView(Graphics& g, RectF area) {
    // design: greet 28px bold tracking -.5; sub 13px muted mt 4
    // profile-card: mt 22, padding 22 26 24, grid 86 / 1fr; avatar large 72x72
    // meta grid 96/1fr/auto, font 13.5, padding-top 16
    const Palette& pal = palette();
    float op = g_view_fade.started ? g_view_fade.value() : 1.0f;
    op = (g_main_opacity.value()) * op;
    if (op <= 0.001f) return;
    auto fade = [&](Color c) { return Color((BYTE)(c.GetA() * op), c.GetR(), c.GetG(), c.GetB()); };

    auto greet = replaceNick(W(tr("home.greet")).c_str(), g_user.nickname);

    // padding 28 32 32 (design .view)
    float vx = area.X + 32, vy = area.Y + 28;
    float ty = vy + (1.0f - op) * 8;
    drawText_(g, greet.c_str(), vx, ty, area.Width - 64,
              22.0f, fade(pal.text), StringAlignmentNear, FontStyleBold);
    drawText_(g, W(tr("home.subtitle")).c_str(), vx, ty + 36, area.Width - 64,
              10.0f, fade(pal.text_muted));

    // profile-card
    float cx = vx, cy = vy + 64;
    float cw = area.Width - 64, ch = 240;
    drawShadow(g, cx, cy, cw, ch, 12.0f, fade(pal.shadow_card), 1.0f, 2);
    drawShadow(g, cx, cy, cw, ch, 12.0f, fade(pal.shadow_card), 8.0f, 3);
    fillRR(g, cx, cy, cw, ch, 12.0f, fade(pal.card));

    // 大头像 72x72 (design .avatar.large)
    float avR = 36.0f, avx = cx + 26, avy = cy + 26;
    SolidBrush avBg(fade(pal.primary));
    g.FillEllipse(&avBg, avx, avy, avR*2, avR*2);
    Font af(kFontFace, 22.0f, FontStyleBold, UnitPoint);
    SolidBrush avF(Color((BYTE)(255 * op), 255, 255, 255));
    StringFormat fmt; fmt.SetAlignment(StringAlignmentCenter); fmt.SetLineAlignment(StringAlignmentCenter);
    RectF avr(avx, avy, avR*2, avR*2);
    wchar_t initial[2] = { (wchar_t)towupper(g_user.nickname[0]), 0 };
    g.DrawString(initial, -1, &af, avr, &fmt, &avF);
    // online dot 14x14, right 2 bottom 2, ring 3px card
    float dotR = 7.0f;
    float dx = avx + avR*2 - dotR*2 - 2.0f;
    float dy = avy + avR*2 - dotR*2 - 2.0f;
    SolidBrush stB(fade(pal.status_online));
    g.FillEllipse(&stB, dx, dy, dotR*2, dotR*2);
    Pen ring(fade(pal.card), 3.0f);
    g.DrawEllipse(&ring, dx, dy, dotR*2, dotR*2);

    // profile-id (右上方)
    float idX = cx + 26 + avR*2 + 18;
    drawText_(g, g_user.nickname, idX, cy + 28, cw - (idX - cx) - 26,
              14.0f, fade(pal.text), StringAlignmentNear, FontStyleBold);
    drawText_(g, g_user.email, idX, cy + 56, cw - (idX - cx) - 26,
              10.0f, fade(pal.text_muted));
    SolidBrush gn(fade(pal.status_online));
    g.FillEllipse(&gn, idX, cy + 84.0f, 6.0f, 6.0f);
    drawText_(g, L"Online", idX + 12, cy + 80, 80, 8.5f, fade(pal.text_muted));

    // meta divider
    Pen sep(fade(pal.divider), 1.0f);
    g.DrawLine(&sep, cx + 26, cy + 130, cx + cw - 26, cy + 130);

    // 4 行 meta (design 13.5px, key/val/auto)
    float ry = cy + 146;
    struct R { const char* lab; const wchar_t* val; bool mono; };
    std::wstring tier_w = W(tr("tier.1week"));
    R rows[] = {
        { "home.tier",       tier_w.c_str(),    false },
        { "home.expires",    g_user.expires,    false },
        { "home.device_id",  g_user.device_id,  true  },
        { "home.last_login", g_user.last_login, true  },
    };
    for (int i = 0; i < 4; ++i) {
        drawText_(g, W(tr(rows[i].lab)).c_str(), cx + 26, ry, 100,
                  9.0f, fade(pal.text_muted));
        drawText_(g, rows[i].val, cx + 130, ry, cw - 200,
                  9.5f, fade(pal.text), StringAlignmentNear, FontStyleBold);
        if (i == 3) {
            std::wstring lab = W(tr("home.history"));
            float lw = measureText(g, lab.c_str(), 9.0f, FontStyleBold).Width + 16;
            RectF link(cx + cw - 26 - lw, ry - 4, lw, 22);
            bool hov = inRect(g_mouse, link);
            if (hov) {
                Color hc((BYTE)(30 * op), pal.primary.GetR(), pal.primary.GetG(), pal.primary.GetB());
                fillRR(g, link.X, link.Y, link.Width, link.Height, 6.0f, hc);
            }
            drawText_(g, lab.c_str(), link.X, link.Y + 4, link.Width,
                      9.0f, hov ? fade(pal.primary_hover) : fade(pal.primary),
                      StringAlignmentCenter, FontStyleBold);
            hit(link, [](){
                g_overlay = Overlay::History;
                g_overlay_t.start(0, 1, 0.25f, 0, curve::easeOutCubic);
            }, true);
        }
        ry += 22.0f;
    }
}

void paintLunchingView(Graphics& g, RectF area) {
    // design: lib-grid 4 cols 240px gap 18, mt 22; game-card 240x140
    // bg linear-gradient 135 #2c2825 -> #1f1c19; cover radial primary 30%/20%
    // hover translateY(-2) + shadow; meta abs left 14 right 14 bottom 12; name 14 bold; ver 11.5 muted
    const Palette& pal = palette();
    float op = g_view_fade.started ? g_view_fade.value() : 1.0f;
    op = (g_main_opacity.value()) * op;
    if (op <= 0.001f) return;
    auto fade = [&](Color c) { return Color((BYTE)(c.GetA() * op), c.GetR(), c.GetG(), c.GetB()); };

    float vx = area.X + 32, vy = area.Y + 28;
    drawText_(g, W(tr("menu.library")).c_str(), vx, vy + (1.0f - op) * 8, 400,
              22.0f, fade(pal.text), StringAlignmentNear, FontStyleBold);
    wchar_t cnt[16]; swprintf_s(cnt, 16, L"1 %ls", W(tr("library.count")).c_str());
    drawText_(g, cnt, vx, vy + 36, 200, 9.0f, fade(pal.text_muted));

    // game-card 240x140, single CS
    float gx = vx, gy = vy + 78;
    bool hover = inRect(g_mouse, RectF(gx, gy, 240, 140));
    float lift = hover ? 2.0f : 0.0f;

    drawShadow(g, gx, gy - lift, 240, 140, 12.0f,
               hover ? fade(pal.shadow_card_hover) : fade(pal.shadow_card),
               hover ? 8.0f : 2.0f, hover ? 4 : 3);

    GraphicsPath card; buildRoundRect(card, gx, gy - lift, 240, 140, 12.0f);
    modal::ensureCS2Thumb();
    if (modal::g_cs2_thumb) {
        // 真缩略图 cover (Steam fastly)
        g.SetClip(&card);
        float iw = (float)modal::g_cs2_thumb->GetWidth();
        float ih = (float)modal::g_cs2_thumb->GetHeight();
        float scale = std::max(240.0f / iw, 140.0f / ih);
        float dw = iw * scale, dh = ih * scale;
        float dx = gx + (240 - dw) / 2;
        float dy = gy - lift + (140 - dh) / 2;
        ImageAttributes attr;
        ColorMatrix mat = {
            1,0,0,0,0,
            0,1,0,0,0,
            0,0,1,0,0,
            0,0,0, op,0,
            0,0,0,0,1
        };
        attr.SetColorMatrix(&mat, ColorMatrixFlagsDefault, ColorAdjustTypeBitmap);
        g.DrawImage(modal::g_cs2_thumb, RectF(dx, dy, dw, dh), 0, 0, iw, ih, UnitPixel, &attr);
        g.ResetClip();
        // 底部黑色渐变蒙版让文字可读
        g.SetClip(&card);
        LinearGradientBrush vmask(PointF(gx, gy - lift + 70), PointF(gx, gy - lift + 140),
                                  Color(0, 0, 0, 0),
                                  Color((BYTE)(180 * op), 0, 0, 0));
        g.FillRectangle(&vmask, gx, gy - lift + 70, 240.0f, 70.0f);
        g.ResetClip();
    } else {
        // fallback：渐变 + 主色 radial
        LinearGradientBrush base(
            PointF(gx, gy - lift), PointF(gx + 240, gy - lift + 140),
            Color((BYTE)(255 * op), 0x2C, 0x28, 0x25),
            Color((BYTE)(255 * op), 0x1F, 0x1C, 0x19));
        g.FillPath(&base, &card);
        GraphicsPath cover; cover.AddEllipse(gx - 80.0f, gy - lift - 100.0f, 280.0f, 280.0f);
        PathGradientBrush rad(&cover);
        Color radCenter((BYTE)(140 * op), pal.primary.GetR(), pal.primary.GetG(), pal.primary.GetB());
        Color radEdge(0, 0, 0, 0);
        rad.SetCenterColor(radCenter);
        int n = 1;
        rad.SetSurroundColors(&radEdge, &n);
        g.SetClip(&card);
        g.FillPath(&rad, &cover);
        g.ResetClip();
        Font cf(kFontFace, 22.0f, FontStyleBold, UnitPoint);
        SolidBrush csB(Color((BYTE)(220 * op), 0xF5, 0xC4, 0x4C));
        StringFormat csF; csF.SetAlignment(StringAlignmentNear);
        g.DrawString(L"CS", -1, &cf, RectF(gx + 14, gy - lift + 12, 60, 30), &csF, &csB);
    }

    // meta abs bottom-left
    drawText_(g, L"Counter-Strike 2", gx + 14, gy - lift + 96, 240 - 28,
              10.5f, Color((BYTE)(255 * op), 255, 255, 255),
              StringAlignmentNear, FontStyleBold);
    drawText_(g, L"v1.40.1.5", gx + 14, gy - lift + 116, 240 - 28,
              8.5f, fade(pal.text_muted));

    hit(RectF(gx, gy - lift, 240, 140), [](){ modal::openCS2(); }, true);

    // Launch hint on hover
    if (hover) {
        drawText_(g, W(tr("library.launch")).c_str(),
                  gx + 240 - 64, gy - lift + 116, 50, 8.5f,
                  fade(pal.primary), StringAlignmentFar, FontStyleBold);
    }
}

void paintCloudView(Graphics& g, RectF area) {
    const Palette& pal = palette();
    float op = g_view_fade.started ? g_view_fade.value() : 1.0f;
    op = (g_main_opacity.value()) * op;
    if (op <= 0.001f) return;
    auto fade = [&](Color c) { return Color((BYTE)(c.GetA() * op), c.GetR(), c.GetG(), c.GetB()); };

    drawText_(g, W(tr("cloud.title")).c_str(), area.X + 18, area.Y + 16, 200,
              16.0f, fade(pal.text), StringAlignmentNear, FontStyleBold);

    float ph_y = area.Y + 60;
    float cw = area.Width - 36, ch = area.Height - 80;
    drawShadow(g, area.X + 18, ph_y, cw, ch, 12.0f, fade(pal.shadow_card), 2.0f, 3);
    fillRR(g, area.X + 18, ph_y, cw, ch, 12.0f, fade(pal.card));
    drawText_(g, L"☁", area.X + 18, ph_y + ch / 2 - 30, cw,
              28.0f, fade(pal.text_faint), StringAlignmentCenter);
    drawText_(g, W(tr("cloud.no_data")).c_str(), area.X + 18, ph_y + ch / 2 + 6, cw,
              9.0f, fade(pal.text_muted), StringAlignmentCenter);
}

void paintSettingsView(Graphics& g, RectF area) {
    // design: max-width 720; h2 13 uppercase tracking 1.2 muted, mt 18 mb 10
    // seg card bg padding 4 radius 10 fit-content; button padding 7 14 radius 7 on=bg
    const Palette& pal = palette();
    float op = g_view_fade.started ? g_view_fade.value() : 1.0f;
    op = (g_main_opacity.value()) * op;
    if (op <= 0.001f) return;
    auto fade = [&](Color c) { return Color((BYTE)(c.GetA() * op), c.GetR(), c.GetG(), c.GetB()); };

    float vx = area.X + 32, vy = area.Y + 28;
    drawText_(g, W(tr("menu.settings")).c_str(), vx, vy + (1.0f - op) * 8, 400,
              22.0f, fade(pal.text), StringAlignmentNear, FontStyleBold);

    // Section helper: render h2 + seg pills
    auto draw_seg_section = [&](float& sy, const char* h2_key, auto& items, auto active_test, auto on_click) {
        // h2 13px uppercase muted tracking
        drawText_(g, W(tr(h2_key)).c_str(), vx, sy, 200,
                  9.0f, fade(pal.text_muted), StringAlignmentNear, FontStyleBold);
        sy += 26;
        // seg container
        const float btn_h = 30.0f, btn_pad = 4.0f;
        float total_w = btn_pad * 2;
        for (auto& it : items) {
            float w = measureText(g, it.label, 9.5f, FontStyleBold).Width + 28;
            total_w += w + 4;
        }
        total_w -= 4;  // last gap
        // seg bg
        fillRR(g, vx, sy, total_w, btn_h + btn_pad * 2, 10.0f, fade(pal.card));
        float bx = vx + btn_pad;
        for (auto& it : items) {
            float w = measureText(g, it.label, 9.5f, FontStyleBold).Width + 28;
            bool active = active_test(it);
            bool hover  = inRect(g_mouse, RectF(bx, sy + btn_pad, w, btn_h));
            if (active) {
                fillRR(g, bx, sy + btn_pad, w, btn_h, 7.0f, fade(pal.bg));
            }
            Color fgc = active ? fade(pal.text) : (hover ? fade(pal.text) : fade(pal.text_muted));
            drawText_(g, it.label, bx, sy + btn_pad + 9.0f, w, 9.5f, fgc,
                      StringAlignmentCenter, FontStyleBold);
            hit(RectF(bx, sy + btn_pad, w, btn_h), [it, on_click]() { on_click(it); }, true);
            bx += w + 4;
        }
        sy += btn_h + btn_pad * 2 + 24;
    };

    float sy = vy + 64;
    struct LBItem { Lang l; const wchar_t* label; };
    LBItem langs[] = { {Lang::En, L"EN"}, {Lang::ZhCN, L"中文"}, {Lang::JaJP, L"日本語"} };
    draw_seg_section(sy, "settings.language", langs,
        [](const LBItem& i){ return g_lang == i.l; },
        [](const LBItem& i){
            if (g_lang != i.l) {
                g_lang = i.l;
                persist::saveLang((int)g_lang);
                g_view_fade.start(0.5f, 1.0f, 0.20f, 0, curve::easeOutCubic);
            }
        });

    struct TBItem { bool dark; const wchar_t* label; };
    static std::wstring s_lab_l = W(tr("settings.theme_light"));
    static std::wstring s_lab_d = W(tr("settings.theme_dark"));
    s_lab_l = W(tr("settings.theme_light"));
    s_lab_d = W(tr("settings.theme_dark"));
    TBItem themes[] = { {false, s_lab_l.c_str()}, {true, s_lab_d.c_str()} };
    draw_seg_section(sy, "settings.theme", themes,
        [](const TBItem& i){ return g_dark == i.dark; },
        [](const TBItem& i){
            if (g_dark != i.dark) {
                g_dark = i.dark;
                persist::saveTheme(g_dark);
                g_view_fade.start(0.6f, 1.0f, 0.25f, 0, curve::easeOutCubic);
            }
        });

    drawText_(g, W(tr("settings.about")).c_str(), vx, sy, 200,
              9.0f, fade(pal.text_muted), StringAlignmentNear, FontStyleBold);
    sy += 26;
    drawText_(g, L"Launcher  v0.1.0", vx, sy, 300, 9.5f, fade(pal.text), StringAlignmentNear, FontStyleBold);
    sy += 22;
    drawText_(g, L"© 2026 dwgx  ·  Skia + Clay + GLFW + libcurl + axum",
              vx, sy, 480, 8.5f, fade(pal.text_muted));
}

void paintProfileView(Graphics& g, RectF area) {
    const Palette& pal = palette();
    float op = g_view_fade.started ? g_view_fade.value() : 1.0f;
    op = (g_main_opacity.value()) * op;
    if (op <= 0.001f) return;
    auto fade = [&](Color c) { return Color((BYTE)(c.GetA() * op), c.GetR(), c.GetG(), c.GetB()); };

    drawText_(g, W(tr("profile.title")).c_str(), area.X + 18, area.Y + 16, 200,
              16.0f, fade(pal.text), StringAlignmentNear, FontStyleBold);

    float cx = area.X + 18, cy = area.Y + 50;
    float cw = area.Width - 36, ch = area.Height - 70;
    drawShadow(g, cx, cy, cw, ch, 12.0f, fade(pal.shadow_card), 2.0f, 3);
    fillRR(g, cx, cy, cw, ch, 12.0f, fade(pal.card));

    auto field = [&](float fy, const char* labelKey, const wchar_t* val, bool editable) {
        drawText_(g, W(tr(labelKey)).c_str(), cx + 16, fy, 100, 7.5f, fade(pal.text_muted));
        RectF box(cx + 90, fy - 4, cw - 110, 26);
        Color bxbg = editable ? fade(pal.surface) : fade(pal.bg);
        fillRR(g, box.X, box.Y, box.Width, box.Height, 5.0f, bxbg);
        drawText_(g, val, box.X + 8, box.Y + 6, box.Width - 16, 9.0f,
                  fade(editable ? pal.text : pal.text_faint),
                  StringAlignmentNear, editable ? FontStyleBold : FontStyleRegular);
        if (!editable) drawText_(g, L"🔒", box.X + box.Width - 18, box.Y + 4, 14, 8.0f, fade(pal.text_faint));
    };
    field(cy + 22, "profile.uid",      g_user.uid,      false);
    field(cy + 56, "profile.username", g_user.username, false);
    field(cy + 90, "profile.nickname", g_user.nickname, true);

    // 按钮
    float by = cy + ch - 40;
    RectF up(cx + 16, by, 130, 28);
    fillRR(g, up.X, up.Y, up.Width, up.Height, 6.0f, fade(pal.primary));
    drawText_(g, W(tr("profile.upload_avatar")).c_str(), up.X, up.Y + 8, up.Width, 9.0f,
              Color((BYTE)(255 * op), 255, 255, 255), StringAlignmentCenter, FontStyleBold);
    RectF pw(cx + 156, by, 130, 28);
    fillRR(g, pw.X, pw.Y, pw.Width, pw.Height, 6.0f, fade(pal.card));
    strokeRR(g, pw.X, pw.Y, pw.Width, pw.Height, 6.0f, fade(pal.divider));
    drawText_(g, W(tr("profile.change_pw")).c_str(), pw.X, pw.Y + 8, pw.Width, 9.0f,
              fade(pal.text), StringAlignmentCenter, FontStyleBold);
}

// ====================================================================
// History overlay (适配 640x400)
// ====================================================================
void paintHistoryOverlay(Graphics& g, int Wpx, int Hpx) {
    const Palette& pal = palette();
    float t = g_overlay_t.value();
    if (t < 0.001f) return;

    Color dim((BYTE)(pal.overlay_dim.GetA() * t), 0, 0, 0);
    SolidBrush bg(dim);
    g.FillRectangle(&bg, 0, 0, Wpx, Hpx);

    float cw = (float)Wpx - 80;
    float ch = (float)Hpx - 80;
    float cx = (Wpx - cw) / 2;
    float cy = (Hpx - ch) / 2 + 8 * (1.0f - t);
    auto fade = [&](Color c) { return Color((BYTE)(c.GetA() * t), c.GetR(), c.GetG(), c.GetB()); };
    Color cardC((BYTE)(255 * t), pal.card.GetR(), pal.card.GetG(), pal.card.GetB());
    drawShadow(g, cx, cy, cw, ch, 12.0f, Color((BYTE)(80 * t), 0, 0, 0), 6.0f, 5);
    fillRR(g, cx, cy, cw, ch, 12.0f, cardC);

    drawText_(g, W(tr("hist.title")).c_str(), cx + 18, cy + 14, cw - 36,
              13.0f, fade(pal.text), StringAlignmentNear, FontStyleBold);
    RectF x(cx + cw - 32, cy + 10, 22, 22);
    bool xh = inRect(g_mouse, x);
    if (xh) fillRR(g, x.X, x.Y, x.Width, x.Height, 5.0f, fade(pal.surface));
    drawText_(g, L"✕", x.X, x.Y + 4, x.Width, 10.0f, fade(pal.text), StringAlignmentCenter);
    hit(x, [](){ g_overlay = Overlay::None; g_overlay_t.start(g_overlay_t.value(), 0, 0.18f, 0, curve::easeOutCubic); }, true);

    Pen sep(fade(pal.divider), 1.0f);
    g.DrawLine(&sep, cx + 18, cy + 42, cx + cw - 18, cy + 42);

    struct R { const wchar_t* time; bool ok; const wchar_t* ip; };
    R rows[] = {
        { L"2026-05-02 10:32",  true,  L"114.215.182.41" },
        { L"2026-05-01 22:14",  true,  L"114.215.182.41" },
        { L"2026-05-01 08:02",  false, L"45.32.198.7"    },
        { L"2026-04-30 18:51",  true,  L"114.215.182.41" },
        { L"2026-04-30 09:08",  true,  L"114.215.182.41" },
    };
    float ry = cy + 56;
    for (auto& r : rows) {
        drawText_(g, r.time, cx + 18, ry, 150, 8.5f, fade(pal.text));
        Color statusC = r.ok ? fade(Color(255, 0x4C, 0xAF, 0x50))
                             : fade(Color(255, 0xE3, 0x4B, 0x4B));
        drawText_(g, r.ok ? W(tr("hist.success")).c_str() : W(tr("hist.failed")).c_str(),
                  cx + 170, ry, 60, 8.5f, statusC, StringAlignmentNear, FontStyleBold);
        drawText_(g, r.ip, cx + 240, ry, cw - 280, 8.5f, fade(pal.text_muted));
        ry += 26;
    }

    // 点击外部关闭
    RectF outer(0, 0, (REAL)Wpx, (REAL)Hpx);
    RectF inner(cx, cy, cw, ch);
    hit(outer, [inner](){
        if (!inRect(g_mouse, inner)) {
            g_overlay = Overlay::None;
            g_overlay_t.start(g_overlay_t.value(), 0, 0.18f, 0, curve::easeOutCubic);
        }
    }, true);
}

// ====================================================================
// Auth view (640x400)
// ====================================================================
void paintAuthView(Graphics& g, int Wpx, int Hpx) {
    // design: card 380, padding 32 30 28, radius 16
    // h1 22 bold tracking -.3, tagline 13 muted mt 4
    // form mt 22 gap 14; field h 48 radius 10 bg=bg border=divider
    // floating label: empty/blur -> 14 muted center; focus/filled -> 11 primary bold top
    g_hits.clear();
    const Palette& pal = palette();
    SolidBrush bg(pal.bg);
    g.FillRectangle(&bg, 0, 0, Wpx, Hpx);

    bool reg = (g_auth_mode == AuthMode::Register);
    const float cw = 380.0f;
    const float ch = reg ? 460.0f : 380.0f;
    const float cx = (Wpx - cw) / 2.0f;
    const float cy = (Hpx - ch) / 2.0f + g_auth_card_y.value();
    float op = g_auth_card_op.value();
    if (op <= 0.001f) return;

    auto fade = [&](Color c) { return Color((BYTE)(c.GetA() * op), c.GetR(), c.GetG(), c.GetB()); };

    // 阴影减弱：之前 5/6 spread 在 light theme 下太重
    drawShadow(g, cx, cy, cw, ch, 16.0f, fade(pal.shadow_card), 4.0f, 3);
    fillRR(g, cx, cy, cw, ch, 16.0f, fade(pal.card));

    // h1 22px bold + logo 26x26
    float lr = 14.0f, lx = cx + 30, ly = cy + 32;
    SolidBrush lbg(fade(pal.primary));
    g.FillEllipse(&lbg, lx, ly, lr*2, lr*2);
    Font lf(kFontFace, 12.0f, FontStyleBold, UnitPoint);
    SolidBrush lf_b(Color((BYTE)(255 * op), 255, 255, 255));
    StringFormat lfmt; lfmt.SetAlignment(StringAlignmentCenter); lfmt.SetLineAlignment(StringAlignmentCenter);
    RectF lr_rect(lx, ly, lr*2, lr*2);
    g.DrawString(L"L", -1, &lf, lr_rect, &lfmt, &lf_b);

    drawText_(g, W(tr(reg ? "auth.register.title" : "auth.login.title")).c_str(),
              lx + lr*2 + 10, cy + 30, 220,
              16.0f, fade(pal.text), StringAlignmentNear, FontStyleBold);
    drawText_(g, W(tr(reg ? "auth.register.sub" : "auth.login.sub")).c_str(),
              cx + 30, cy + 60, 320, 9.5f, fade(pal.text_muted));

    // floating-label field 48px high — 真用 tween 平滑过渡
    auto drawField = [&](InputBox& box, float ix, float iy, float iw, float ih,
                          const char* labelKey, int idx, float* /*unused*/) {
        box.bounds = RectF(ix, iy, iw, ih);
        bool focused = (g_auth_form.focus == idx);
        bool filled = !box.text.empty();
        bool floating = focused || filled;
        // 启动 tween（仅在状态切换时）
        float target = floating ? 1.0f : 0.0f;
        if (!box.float_t.started) {
            box.float_t.start(target, target, 0.001f, 0, curve::easeOutCubic);
        } else if (std::abs(box.float_t.to - target) > 0.001f) {
            box.float_t.start(box.float_t.value(), target, 0.22f, 0, curve::easeOutCubic);
        }
        float anim_v = box.float_t.value();

        // bg = bg(page-bg), border = divider/primary
        fillRR(g, ix, iy, iw, ih, 10.0f, fade(pal.bg));
        Color bd = focused ? fade(pal.primary) : fade(pal.divider);
        strokeRR(g, ix, iy, iw, ih, 10.0f, bd, focused ? 1.5f : 1.0f);
        // focus halo
        if (focused) {
            Color halo((BYTE)(38 * op), pal.primary.GetR(), pal.primary.GetG(), pal.primary.GetB());
            strokeRR(g, ix - 2, iy - 2, iw + 4, ih + 4, 12.0f, halo, 4.0f);
        }

        // floating label — 用 anim_v 在 0..1 平滑插值
        // floating: top 13 / 11px primary bold
        // resting: vert center / 14px muted
        std::wstring labelStr = W(tr(labelKey));
        float lab_size = 11.5f - 3.5f * (1.0f - anim_v);
        float lab_y = iy + 5.0f + (ih * 0.5f - 5.0f - 5.0f) * (1.0f - anim_v);
        // 颜色平滑插值（muted → primary）
        BYTE lab_r = (BYTE)(pal.text_muted.GetR() + (pal.primary.GetR() - pal.text_muted.GetR()) * anim_v);
        BYTE lab_g = (BYTE)(pal.text_muted.GetG() + (pal.primary.GetG() - pal.text_muted.GetG()) * anim_v);
        BYTE lab_b = (BYTE)(pal.text_muted.GetB() + (pal.primary.GetB() - pal.text_muted.GetB()) * anim_v);
        Color lab_c((BYTE)(255 * op), lab_r, lab_g, lab_b);
        drawText_(g, labelStr.c_str(), ix + 14.0f, lab_y, iw - 28.0f, lab_size, lab_c,
                  StringAlignmentNear, anim_v > 0.5f ? FontStyleBold : FontStyleRegular);

        // 选区高亮 + 文本
        Font* tf = fontcache::get(10.5f);
        if (focused && box.hasSelection()) {
            RectF bb_pre, bb_in;
            g.MeasureString(box.displaySlice(0, box.selStart()).c_str(), -1, tf,
                            PointF(0, 0), &bb_pre);
            g.MeasureString(box.displaySlice(box.selStart(), box.selEnd()).c_str(), -1, tf,
                            PointF(0, 0), &bb_in);
            Color sel_bg(96, pal.primary.GetR(), pal.primary.GetG(), pal.primary.GetB());
            SolidBrush sel_b(sel_bg);
            g.FillRectangle(&sel_b, ix + 14.0f + bb_pre.Width, iy + 21.0f,
                            bb_in.Width, 18.0f);
        }
        std::wstring txt = box.display();
        if (!txt.empty()) {
            drawText_(g, txt.c_str(), ix + 14.0f, iy + 22.0f, iw - 28.0f,
                      10.5f, fade(pal.text));
        }
        // caret
        if (focused && !box.hasSelection()) {
            std::wstring sub = box.displaySlice(0, box.cursor);
            RectF bbox; g.MeasureString(sub.c_str(), -1, tf, PointF(0, 0), &bbox);
            float cur_x = ix + 14.0f + bbox.Width;
            int phase = (int)(g_time_in_stage * 1000) % 1000;
            if (phase < 500) {
                Pen p(fade(pal.primary), 1.5f);
                g.DrawLine(&p, cur_x, iy + 22.0f, cur_x, iy + ih - 8.0f);
            }
        }
        hit(box.bounds, [idx](){
            g_auth_form.focus = idx;
            // 切换 focus 时清除选区，避免视觉残留
            if (idx != 0) g_auth_form.username.clearSel();
            if (idx != 1) g_auth_form.password.clearSel();
            if (idx != 2) g_auth_form.invite.clearSel();
        }, true);
    };

    static float anim_u = 0, anim_p = 0, anim_i = 0;
    float fy = cy + 90;
    drawField(g_auth_form.username, cx + 30, fy, cw - 60, 48, "auth.username", 0, &anim_u);
    fy += 62;
    g_auth_form.password.password = true;
    drawField(g_auth_form.password, cx + 30, fy, cw - 60, 48, "auth.password", 1, &anim_p);
    fy += 62;
    if (reg) {
        drawField(g_auth_form.invite, cx + 30, fy, cw - 60, 48, "auth.invite", 2, &anim_i);
        fy += 62;
    }

    // Submit btn-primary 44 high radius 10 + gradient shadow
    RectF btn(cx + 30, fy + 6, cw - 60, 44);
    bool bhov = inRect(g_mouse, btn);
    Color bbg = g_auth_form.busy
        ? fade(Color(255, 0x6B, 0x6A, 0x67))
        : (bhov ? fade(pal.primary_hover) : fade(pal.primary));
    // 阴影 0 6px 16px -6 rgba(217,119,87,.6)
    Color glow((BYTE)(70 * op), pal.primary.GetR(), pal.primary.GetG(), pal.primary.GetB());
    drawShadow(g, btn.X, btn.Y, btn.Width, btn.Height, 10.0f, glow, 4.0f, 3);
    fillRR(g, btn.X, btn.Y, btn.Width, btn.Height, 10.0f, bbg);
    drawText_(g, W(tr(g_auth_form.busy ? "auth.busy" : (reg ? "auth.register" : "auth.login"))).c_str(),
              btn.X, btn.Y + 14, btn.Width, 11.0f,
              Color((BYTE)(255 * op), 255, 255, 255),
              StringAlignmentCenter, FontStyleBold);
    if (!g_auth_form.busy) {
        hit(btn, [](){ PostMessageW(g_hwnd, WM_APP + 1, 0, 0); }, true);
    }

    // Error: rgba(227,75,75,.1) bg, padding 8 10, radius 8
    if (!g_auth_form.error_msg.empty()) {
        Color err_bg((BYTE)(28 * op), 0xE3, 0x4B, 0x4B);
        Color err_fg((BYTE)(255 * op), 0xFF, 0x8A, 0x80);
        RectF errR(btn.X, btn.Y + 56, btn.Width, 26);
        fillRR(g, errR.X, errR.Y, errR.Width, errR.Height, 8.0f, err_bg);
        drawText_(g, g_auth_form.error_msg.c_str(),
                  errR.X + 10, errR.Y + 7, errR.Width - 20, 9.0f, err_fg);
    }

    // Switch link
    drawText_(g, W(tr(reg ? "auth.to_login" : "auth.to_register")).c_str(),
              cx + 30, cy + ch - 36, 160, 9.0f, fade(pal.text_muted));
    std::wstring link_label = W(tr(reg ? "auth.go_login" : "auth.go_register"));
    float link_w = measureText(g, link_label.c_str(), 9.0f, FontStyleBold).Width + 6;
    RectF link(cx + 30 + 130, cy + ch - 36, link_w, 16);
    bool lhov = inRect(g_mouse, link);
    drawText_(g, link_label.c_str(),
              link.X, link.Y + 1, link.Width, 9.0f,
              lhov ? fade(pal.primary_hover) : fade(pal.primary),
              StringAlignmentNear, FontStyleBold);
    hit(link, [](){
        g_auth_mode = (g_auth_mode == AuthMode::Login) ? AuthMode::Register : AuthMode::Login;
        g_auth_form.error_msg.clear();
        g_auth_form.focus = 0;
        g_auth_card_op.start(0.6f, 1.0f, 0.18f, 0.0f, curve::easeOutCubic);
    }, true);
}

// ====================================================================
// Main paint
// ====================================================================
void paintMain(Graphics& g, int Wpx, int Hpx) {
    g_hits.clear();
    const Palette& pal = palette();
    SolidBrush bg(pal.bg);
    g.FillRectangle(&bg, 0, 0, Wpx, Hpx);

    paintTopbar(g, Wpx);
    paintSidebar(g, Hpx);

    RectF area(kSidebarW, kTopbarH, (REAL)Wpx - kSidebarW, (REAL)Hpx - kTopbarH);
    switch (g_view) {
        case View::Home:     paintHomeView(g, area);     break;
        case View::Lunching: paintLunchingView(g, area); break;
        case View::Chat:     chatv::paintChatViewTop(g, area); break;
        case View::Cloud:    paintCloudView(g, area);    break;
        case View::Settings: paintSettingsView(g, area); break;
        case View::Profile:  paintProfileView(g, area);  break;
    }

    if (g_overlay == Overlay::History || g_overlay_t.value() > 0.001f) {
        modal::paintHistoryModalNew(g, Wpx, Hpx);
    }
    paintAccountDropdown(g, Wpx);
    registerDropdownDismissHits(Wpx, Hpx);
    modal::paintCS2Modal(g, Wpx, Hpx);
}

// ====================================================================
// Dot 阶段：起始一个小点 (40x40 窗口里画 2→14px 主色圆)
// ====================================================================
void paintDot(Graphics& g, int Wpx, int Hpx) {
    const Palette& pal = palette();
    SolidBrush bg(pal.bg);
    g.FillRectangle(&bg, 0, 0, Wpx, Hpx);
    float sz = g_dot_size.value();
    float a  = g_dot_alpha.value();
    if (a <= 0.001f) return;
    Color c((BYTE)(255 * a), pal.primary.GetR(), pal.primary.GetG(), pal.primary.GetB());
    SolidBrush b(c);
    g.FillEllipse(&b, (Wpx - sz) / 2.0f, (Hpx - sz) / 2.0f, sz, sz);
}

// ====================================================================
// Stage 切换
// ====================================================================
void enterDotStage() {
    g_stage = Stage::Dot;
    g_time_in_stage = 0.0f;
    g_dot_size.start(2, 14, 0.30f, 0.0f, curve::easeOutCubic);
    g_dot_alpha.start(0, 1, 0.25f, 0.0f, curve::easeOutCubic);
}
// 40x40 → 200x200 (展开成加载卡片)
void enterExpandLoadingStage() {
    g_stage = Stage::ExpandLoading;
    g_time_in_stage = 0.0f;
    g_window_w.start(40, 200, 0.40f, 0.0f, curve::easeOutBack);
    g_window_h.start(40, 200, 0.40f, 0.0f, curve::easeOutBack);
}
void enterLoadingStage() {
    g_stage = Stage::Loading;
    g_time_in_stage = 0.0f;
    g_card_scale.start(0.85f, 1.0f, 0.30f, 0.0f, curve::easeOutBack);
    g_card_opacity.start(0.0f, 1.0f, 0.25f, 0.0f, curve::easeOutCubic);
}
// 200x200 → 480x540 (loading 完了向外扩展到 Auth, 容纳 380 卡片)
void enterExpandAuthStage() {
    g_stage = Stage::ExpandAuth;
    g_time_in_stage = 0.0f;
    g_card_fade_out.start(0, 1, 0.25f, 0.0f, curve::easeOutCubic);
    g_window_w.start(200, 480, 0.50f, 0.05f, curve::easeOutQuint);
    g_window_h.start(200, 540, 0.50f, 0.05f, curve::easeOutQuint);
}
void enterAuthStage() {
    g_stage = Stage::Auth;
    g_time_in_stage = 0.0f;
    g_auth_card_op.start(0, 1, 0.40f, 0.05f, curve::easeOutCubic);
    g_auth_card_y.start(12, 0, 0.45f, 0.05f, curve::easeOutQuint);
}
// 480x540 → 1100x720 (Auth submit 后扩到桌面级主界面)
void enterExpandMainStage() {
    g_stage = Stage::ExpandMain;
    g_time_in_stage = 0.0f;
    g_auth_card_op.start(g_auth_card_op.value(), 0, 0.20f, 0.0f, curve::easeOutCubic);
    g_window_w.start(480, 1100, 0.50f, 0.05f, curve::easeOutQuint);
    g_window_h.start(540, 720,  0.50f, 0.05f, curve::easeOutQuint);
}
void enterMainStage() {
    g_stage = Stage::Main;
    g_time_in_stage = 0.0f;
    g_sidebar_x.start(0, 1, 0.40f, 0.05f, curve::easeOutQuint);
    g_topbar_y.start(0, 1, 0.35f, 0.10f, curve::easeOutCubic);
    g_main_opacity.start(0, 1, 0.45f, 0.15f, curve::easeOutQuint);
}
// 兼容旧调用名
void enterExpandingStage() { enterExpandLoadingStage(); }

// ====================================================================
// 托盘 — Shell_NotifyIcon + 右键弹出菜单（显示主窗口 / 退出）
// ====================================================================
void trayAdd(HWND hwnd) {
    if (g_tray_added) return;
    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = hwnd;
    g_nid.uID = kTrayUid;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = kTrayCallbackMsg;
    g_nid.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wcscpy_s(g_nid.szTip, L"Launcher");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
    g_tray_added = true;
}
void trayRemove() {
    if (!g_tray_added) return;
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    g_tray_added = false;
}
void hideToTray(HWND hwnd) {
    trayAdd(hwnd);
    ShowWindow(hwnd, SW_HIDE);
    // Win11 默认把第三方托盘 icon 收进 ↑ 弹出层，弹一个 balloon 让用户找得到
    static bool first = true;
    if (first) {
        first = false;
        NOTIFYICONDATAW info = g_nid;
        info.uFlags = NIF_INFO;
        wcscpy_s(info.szInfoTitle, L"Launcher");
        wcscpy_s(info.szInfo, L"已最小化到托盘 — 双击图标恢复 / 右键退出（Win11 在 ↑ 内可找到）");
        info.dwInfoFlags = NIIF_INFO;
        info.uTimeout = 4000;
        Shell_NotifyIconW(NIM_MODIFY, &info);
    }
}
void showFromTray(HWND hwnd) {
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
}
void showTrayMenu(HWND hwnd) {
    if (!g_tray_menu) {
        g_tray_menu = CreatePopupMenu();
        AppendMenuW(g_tray_menu, MF_STRING, 1001, L"显示主窗口");
        AppendMenuW(g_tray_menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(g_tray_menu, MF_STRING, 1002, L"退出");
    }
    POINT pt; GetCursorPos(&pt);
    SetForegroundWindow(hwnd);
    UINT cmd = TrackPopupMenu(g_tray_menu,
        TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
        pt.x, pt.y, 0, hwnd, nullptr);
    if (cmd == 1001) showFromTray(hwnd);
    else if (cmd == 1002) { trayRemove(); PostQuitMessage(0); }
}

// ====================================================================
// WndProc
// ====================================================================
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_NCHITTEST: {
            POINT p { LOWORD(lp), HIWORD(lp) };
            ScreenToClient(hwnd, &p);
            // 每帧的 g_hits 区域 不可拖；其他区域全部 HTCAPTION 整窗拖
            for (auto& h : g_hits) {
                if (h.draggable_off && inRect(p, h.rect)) return HTCLIENT;
            }
            return HTCAPTION;
        }
        case WM_MOUSEMOVE: {
            int nx = LOWORD(lp), ny = HIWORD(lp);
            if (nx == g_mouse.x && ny == g_mouse.y) break;
            g_mouse.x = nx; g_mouse.y = ny;
            // hover 状态需要实时更新；GDI+ dirty region 会 batch invalidate
            InvalidateRect(hwnd, nullptr, FALSE);
            break;
        }
        case WM_LBUTTONUP: {
            POINT p { LOWORD(lp), HIWORD(lp) };
            // 任何点击都会重置 chat composer focus；hit 处理时如果落在 textarea 会再 set true
            chatv::g_focus_composer = false;
            for (auto it = g_hits.rbegin(); it != g_hits.rend(); ++it) {
                if (inRect(p, it->rect)) {
                    if (it->on_click) it->on_click();
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
            }
            break;
        }
        case kTrayCallbackMsg: {
            UINT ev = LOWORD(lp);
            if (ev == WM_LBUTTONUP) showFromTray(hwnd);
            else if (ev == WM_RBUTTONUP || ev == WM_CONTEXTMENU) showTrayMenu(hwnd);
            return 0;
        }
        case WM_CHAR: {
            wchar_t c = (wchar_t)wp;
            bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            if (g_stage == Stage::Auth) {
                InputBox* box = nullptr;
                if (g_auth_form.focus == 0)      box = &g_auth_form.username;
                else if (g_auth_form.focus == 1) box = &g_auth_form.password;
                else if (g_auth_form.focus == 2) box = &g_auth_form.invite;
                if (box) {
                    if (!box->onChar(c, ctrl, hwnd)) {
                        // 上层响应：Enter 提交
                        if (c == L'\r' || c == L'\n') PostMessageW(hwnd, WM_APP + 1, 0, 0);
                    }
                }
                InvalidateRect(hwnd, nullptr, FALSE);
            } else if (g_stage == Stage::Main && g_view == View::Chat && chatv::g_focus_composer) {
                // Ctrl+V 优先尝试粘贴媒体（图片 / 文件 drop）
                if (ctrl && c == 0x16) {
                    if (chatv::tryPasteMedia(hwnd)) {
                        InvalidateRect(hwnd, nullptr, FALSE);
                        return 0;
                    }
                }
                if (!chatv::g_composer.onChar(c, ctrl, hwnd)) {
                    if (c == L'\r' || c == L'\n') {
                        if (!chatv::g_composer.text.empty()) {
                            auto& s = chatv::streamFor(chatv::g_active);
                            chatv::Msg m; m.kind = chatv::MsgKind::Text;
                            m.from = L"me"; m.author = L""; m.status = L"online";
                            m.read = false; m.time = L"now";
                            static std::vector<std::wstring> g_my_txts;
                            g_my_txts.push_back(chatv::g_composer.text);
                            m.body = g_my_txts.back().c_str();
                            s.push_back(m);
                            chatv::g_composer.text.clear();
                            chatv::g_composer.cursor = 0;
                            chatv::g_composer.clearSel();
                        }
                    }
                }
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            break;
        }
        case WM_KEYDOWN: {
            bool shift_dn = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            bool ctrl_dn  = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            // Auth 表单 Tab 切焦点 + 方向键编辑
            if (g_stage == Stage::Auth) {
                if (wp == VK_TAB) {
                    g_auth_form.focus = (g_auth_form.focus + 1) %
                        (g_auth_mode == AuthMode::Register ? 3 : 2);
                    return 0;
                }
                if (g_auth_form.focus == 0)      g_auth_form.username.onKey((int)wp, shift_dn, ctrl_dn);
                else if (g_auth_form.focus == 1) g_auth_form.password.onKey((int)wp, shift_dn, ctrl_dn);
                else if (g_auth_form.focus == 2) g_auth_form.invite.onKey((int)wp, shift_dn, ctrl_dn);
            } else if (g_stage == Stage::Main && g_view == View::Chat && chatv::g_focus_composer) {
                chatv::g_composer.onKey((int)wp, shift_dn, ctrl_dn);
            }
            // ESC：关 modal / overlay / picker；都没开 → 最小化到系统托盘
            if (wp == VK_ESCAPE) {
                if (modal::g_cs2_open) {
                    modal::closeCS2();
                } else if (g_overlay != Overlay::None) {
                    g_overlay_t.start(g_overlay_t.value(), 0, 0.18f, 0, curve::easeOutCubic);
                    g_overlay = Overlay::None;
                } else if (chatv::g_picker_open) {
                    chatv::g_picker_open = false;
                    chatv::g_picker_t.start(chatv::g_picker_t.value(), 0, 0.18f, 0, curve::easeOutCubic);
                } else {
                    hideToTray(hwnd);
                }
            }
            // 不再绑定 1-5 / D / H / S / P — 全部用鼠标 / sidebar
            break;
        }
        case WM_APP + 1: {
            // Auth submit
            std::wstring user = g_auth_form.username.text;
            std::wstring pass = g_auth_form.password.text;
            std::wstring inv  = g_auth_form.invite.text;
            g_auth_form.error_msg.clear();
            if (user.size() < 3) g_auth_form.error_msg = L"用户名至少 3 字";
            else if (pass.size() < 8) g_auth_form.error_msg = L"密码至少 8 字";
            else if (g_auth_mode == AuthMode::Register && inv.empty())
                g_auth_form.error_msg = L"注册需要邀请码";
            else {
                g_auth_form.busy = true;
                // 持久化凭据到隐秘注册表（DPAPI 加密）
                persist::saveCreds(user, pass);
                SetTimer(hwnd, 0xA1, 600, nullptr);
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_TIMER:
            if (wp == 0xA1) {
                KillTimer(hwnd, 0xA1);
                g_auth_form.busy = false;
                enterExpandMainStage();
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps; HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc; GetClientRect(hwnd, &rc);
            int Wpx = rc.right - rc.left, Hpx = rc.bottom - rc.top;
            // Backbuffer 缓存：避免每帧 CreateCompatibleBitmap/DC（GDI 资源昂贵）。
            // 窗口尺寸变化时才重建。
            static HDC s_mem = nullptr;
            static HBITMAP s_bmp = nullptr;
            static HBITMAP s_old = nullptr;
            static int s_w = 0, s_h = 0;
            if (!s_mem || s_w != Wpx || s_h != Hpx) {
                if (s_mem) {
                    SelectObject(s_mem, s_old);
                    DeleteObject(s_bmp);
                    DeleteDC(s_mem);
                }
                s_mem = CreateCompatibleDC(hdc);
                s_bmp = CreateCompatibleBitmap(hdc, Wpx, Hpx);
                s_old = (HBITMAP)SelectObject(s_mem, s_bmp);
                s_w = Wpx; s_h = Hpx;
            }
            Graphics g(s_mem);
            g.SetSmoothingMode(SmoothingModeAntiAlias);
            g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
            g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
            g.SetPixelOffsetMode(PixelOffsetModeHighQuality);
            if (g_stage == Stage::Dot)             paintDot(g, Wpx, Hpx);
            else if (g_stage == Stage::ExpandLoading) paintLoading(g, Wpx, Hpx);
            else if (g_stage == Stage::Loading)    paintLoading(g, Wpx, Hpx);
            else if (g_stage == Stage::Expanding)  paintLoading(g, Wpx, Hpx);
            else if (g_stage == Stage::ExpandAuth) paintLoading(g, Wpx, Hpx);
            else if (g_stage == Stage::Auth)       paintAuthView(g, Wpx, Hpx);
            else if (g_stage == Stage::ExpandMain) {
                // auto-login: Loading 卡片 fade out + 窗口扩张；非 auto-login 走 Auth 卡片 fade
                if (g_skip_auth_after_loading) paintLoading(g, Wpx, Hpx);
                else paintAuthView(g, Wpx, Hpx);
            }
            else                                    paintMain(g, Wpx, Hpx);
            BitBlt(hdc, 0, 0, Wpx, Hpx, s_mem, 0, 0, SRCCOPY);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_DROPFILES: {
            // 拖入文件 → chat 当前频道发媒体
            if (g_stage == Stage::Main && g_view == View::Chat) {
                HDROP drop = (HDROP)wp;
                UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
                for (UINT i = 0; i < n; ++i) {
                    wchar_t path[MAX_PATH];
                    if (DragQueryFileW(drop, i, path, MAX_PATH)) {
                        chatv::appendMedia(path);
                    }
                }
                DragFinish(drop);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_RBUTTONUP:  hideToTray(hwnd); return 0;
        case WM_DESTROY:    trayRemove(); PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

ULONG_PTR g_gdiplus_token = 0;

int APIENTRY wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR cmdline, int) {
    detectSystemLanguage();

    // 持久化 — 从隐秘注册表加载 lang / theme（覆盖系统默认）
    persist::ensure();
    g_lang = (Lang)persist::loadLang((int)g_lang);
    g_dark = persist::loadTheme(g_dark);

    wchar_t buf[16] = {0};
    if (GetEnvironmentVariableW(L"LAUNCHER_DARK", buf, 16) > 0 && buf[0] == L'1') g_dark = true;
    if (GetEnvironmentVariableW(L"LAUNCHER_LANG", buf, 16) > 0) {
        if (buf[0] == L'e') g_lang = Lang::En;
        else if (buf[0] == L'z') g_lang = Lang::ZhCN;
        else if (buf[0] == L'j') g_lang = Lang::JaJP;
    }
    if (GetEnvironmentVariableW(L"LAUNCHER_VIEW", buf, 16) > 0) {
        if (buf[0] == L'h') g_view = View::Home;
        else if (buf[0] == L'l') g_view = View::Lunching;
        else if (buf[0] == L'c') g_view = View::Cloud;
        else if (buf[0] == L's') g_view = View::Settings;
        else if (buf[0] == L'p') g_view = View::Profile;
        else if (buf[0] == L't') g_view = View::Chat;
        else if (buf[0] == L'm') {
            g_view = View::Chat;
            chatv::g_active = L"market";
        }
    }
    bool overlayHistory = false;
    if (GetEnvironmentVariableW(L"LAUNCHER_OVERLAY", buf, 16) > 0 && buf[0] == L'h') overlayHistory = true;
    bool skip_loading = wcsstr(cmdline, L"--main") != nullptr ||
        (GetEnvironmentVariableW(L"LAUNCHER_SKIP_LOADING", buf, 16) > 0 && buf[0] == L'1');
    bool skip_auth = (GetEnvironmentVariableW(L"LAUNCHER_SKIP_AUTH", buf, 16) > 0 && buf[0] == L'1');
    if (GetEnvironmentVariableW(L"LAUNCHER_AUTH_REGISTER", buf, 16) > 0 && buf[0] == L'1') {
        g_auth_mode = AuthMode::Register;
    }

    GdiplusStartupInput gsi;
    GdiplusStartup(&g_gdiplus_token, &gsi, nullptr);

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"LauncherPreview";
    RegisterClassExW(&wc);

    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    // 入场：Dot 40x40 → Loading 200x200 → Auth 480x540 → Main 1100x720
    int initW = skip_loading ? 1100 : 40;
    int initH = skip_loading ? 720  : 40;

    g_hwnd = CreateWindowExW(
        WS_EX_LAYERED | (skip_loading ? 0 : WS_EX_TOPMOST),
        wc.lpszClassName, L"Launcher",
        WS_POPUP,
        (sw - initW) / 2, (sh - initH) / 2, initW, initH,
        nullptr, nullptr, inst, nullptr);
    SetLayeredWindowAttributes(g_hwnd, 0, 255, LWA_ALPHA);
    DWM_WINDOW_CORNER_PREFERENCE pref = DWMWCP_ROUND;
    DwmSetWindowAttribute(g_hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &pref, sizeof(pref));
    DragAcceptFiles(g_hwnd, TRUE);   // 接收文件拖拽
    ShowWindow(g_hwnd, SW_SHOW); UpdateWindow(g_hwnd);

    // 自动登录 — 注册表里有凭据就直接进 main，跳过 Auth
    std::wstring saved_user, saved_pass;
    bool auto_login = persist::loadCreds(saved_user, saved_pass)
                      && !saved_user.empty() && !saved_pass.empty();
    if (auto_login) {
        g_auth_form.username.text = saved_user;
        g_auth_form.password.text = saved_pass;
        g_auth_form.username.cursor = (int)saved_user.size();
        g_auth_form.password.cursor = (int)saved_pass.size();
    }

    if (skip_loading && !skip_auth) {
        enterAuthStage();
    } else if (skip_loading) {
        // 调试快进
        SetWindowPos(g_hwnd, nullptr, (sw - 1100) / 2, (sh - 720) / 2, 1100, 720, SWP_NOZORDER);
        enterMainStage();
        g_main_opacity.elapsed = 999;
        g_sidebar_x.elapsed = 999;
        g_topbar_y.elapsed = 999;
        if (overlayHistory) {
            g_overlay = Overlay::History;
            g_overlay_t.start(0, 1, 0.25f, 0, curve::easeOutCubic);
            g_overlay_t.elapsed = 999;
        }
    } else {
        // 完整入场动画：Dot → Loading → ...（auto_login 时走 g_skip_auth_after_loading 标记跳过 Auth）
        enterDotStage();
    }
    // 主循环里用此标记决定 Loading 完后是去 Auth 还是直接 ExpandMain
    extern bool g_skip_auth_after_loading;
    g_skip_auth_after_loading = auto_login && !skip_loading;

    auto last = std::chrono::steady_clock::now();
    MSG msg{};
    while (true) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) goto end;
            TranslateMessage(&msg); DispatchMessageW(&msg);
        }
        auto now = std::chrono::steady_clock::now();
        float dt = std::chrono::duration<float>(now - last).count(); last = now;
        if (dt > 0.1f) dt = 0.1f;
        g_spin_angle += 320.0f * dt;
        if (g_spin_angle >= 360.0f) g_spin_angle -= 360.0f;
        g_time_in_stage += dt;

        g_card_scale.tick(dt); g_card_opacity.tick(dt); g_card_fade_out.tick(dt);
        g_window_w.tick(dt);   g_window_h.tick(dt);
        g_sidebar_x.tick(dt);  g_topbar_y.tick(dt);   g_main_opacity.tick(dt);
        g_view_fade.tick(dt);
        g_dropdown_t.tick(dt);
        g_overlay_t.tick(dt);
        g_auth_card_op.tick(dt); g_auth_card_y.tick(dt);
        g_dot_size.tick(dt);   g_dot_alpha.tick(dt);
        g_status_fold_t.tick(dt);
        modal::g_cs2_t.tick(dt);
        chatv::g_picker_t.tick(dt);
        chatv::g_typing_t += dt;
        g_auth_form.username.float_t.tick(dt);
        g_auth_form.password.float_t.tick(dt);
        g_auth_form.invite.float_t.tick(dt);

        // 入场流程驱动
        auto resize_to_tween = [&]() {
            int w = (int)g_window_w.value();
            int h = (int)g_window_h.value();
            int x = (sw - w) / 2, y = (sh - h) / 2;
            SetWindowPos(g_hwnd, nullptr, x, y, w, h, SWP_NOZORDER);
        };
        if (g_stage == Stage::Dot && g_time_in_stage > 0.45f) {
            enterExpandLoadingStage();
        } else if (g_stage == Stage::ExpandLoading) {
            resize_to_tween();
            if (g_window_w.done()) enterLoadingStage();
        } else if (g_stage == Stage::Loading && g_time_in_stage > 1.4f) {
            if (g_skip_auth_after_loading) {
                // 自动登录：Loading 完后直接展开到 Main 1100x720（跳 Auth）
                g_stage = Stage::ExpandMain;
                g_time_in_stage = 0.0f;
                g_card_fade_out.start(0, 1, 0.30f, 0.0f, curve::easeOutCubic);
                g_window_w.start(200, 1100, 0.55f, 0.05f, curve::easeOutQuint);
                g_window_h.start(200, 720,  0.55f, 0.05f, curve::easeOutQuint);
            } else {
                enterExpandAuthStage();
            }
        } else if (g_stage == Stage::ExpandAuth) {
            resize_to_tween();
            if (g_window_w.done()) enterAuthStage();
        } else if (g_stage == Stage::ExpandMain) {
            resize_to_tween();
            if (g_window_w.done()) enterMainStage();
        }

        auto active = [](const Tween& t){ return t.started && !t.done(); };
        bool any_anim = active(g_card_scale) || active(g_card_opacity) || active(g_card_fade_out)
            || active(g_window_w) || active(g_window_h)
            || active(g_sidebar_x) || active(g_topbar_y) || active(g_main_opacity)
            || active(g_view_fade) || active(g_dropdown_t) || active(g_overlay_t)
            || active(g_auth_card_op) || active(g_auth_card_y)
            || active(g_dot_size) || active(g_dot_alpha)
            || active(g_status_fold_t) || active(modal::g_cs2_t)
            || active(chatv::g_picker_t)
            || active(g_auth_form.username.float_t)
            || active(g_auth_form.password.float_t)
            || active(g_auth_form.invite.float_t);
        // 入场阶段 + chat (typing dots / caret) 一直要画
        bool always_anim = (g_stage != Stage::Main)
            || (g_stage == Stage::Main && g_view == View::Chat && chatv::g_focus_composer);
        bool needs_paint = any_anim || always_anim;

        if (needs_paint) {
            InvalidateRect(g_hwnd, nullptr, FALSE);
            Sleep(16);   // 60 FPS
        } else {
            // 真静止：完全不主动 invalidate，等鼠标 / 键盘事件触发。
            // WaitMessage 阻塞到下一个消息，CPU = 0
            WaitMessage();
        }
    }
end:
    GdiplusShutdown(g_gdiplus_token);
    return 0;
}
