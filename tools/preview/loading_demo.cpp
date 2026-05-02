// Launcher 完整预览 demo (GDI+ 实现, 不依赖 Skia/Clay/vcpkg)
//
// 视觉令牌跟 src/ui/theme + src/ui/anim 一致。真实工程在 src/ 下用 Skia + Clay。
//
// 升级要点 (vs 上一版)：
//   * 顶部应用名 "启动器" / "Launcher" / "ランチャー"
//   * 左侧菜单：主页 / Lunching（原"游戏库"）/ 云端 / 设置；不再有左下 Logout
//   * 右上头像点击 → 下拉菜单（个人信息 / 历史登录 / 修改昵称 / 修改头像 / 退出登录）
//   * ProfileView：UID（不可改）/ Username（不可改）/ Nickname（受限频率）/
//                  头像上传 / 修改密码
//   * LoginHistoryView：modal overlay（半透明遮罩 + 居中卡片，点击外部关闭）
//   * 切换主题/语言/view 时 view_fade tween（120ms 淡出 + 180ms 淡入）
//
// 快捷键：D 主题 / 1-4 切 view / S 跳过 loading / H 打开历史登录 overlay /
//        Esc/右键 退出

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include <dwmapi.h>
#include <vector>
#include <string>
#include <functional>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "dwmapi.lib")

using namespace Gdiplus;

// =====================================================================
// Design tokens
// =====================================================================
struct Palette {
    Color bg, surface, card, divider;
    Color primary, primary_hover;
    Color text, text_muted, text_faint;
    Color sidebar_bg, sidebar_active;
    Color shadow_card, shadow_card_hover;
    Color overlay_dim;       // modal 背景遮罩
};

const Palette kLight = {
    Color(255, 0xFA, 0xF7, 0xF2), Color(255, 0xF3, 0xEF, 0xE8),
    Color(255, 0xFF, 0xFF, 0xFF), Color(255, 0xED, 0xE9, 0xE1),
    Color(255, 0xC9, 0x64, 0x42), Color(255, 0xD9, 0x77, 0x57),
    Color(255, 0x1F, 0x1E, 0x1D), Color(255, 0x6B, 0x6A, 0x67),
    Color(255, 0xA8, 0xA3, 0x9A),
    Color(255, 0xF3, 0xEF, 0xE8), Color(255, 0xE9, 0xE1, 0xD3),
    Color( 14, 0, 0, 0),          Color( 30, 0, 0, 0),
    Color(140, 0, 0, 0)
};
const Palette kDark = {
    Color(255, 0x1A, 0x18, 0x16), Color(255, 0x20, 0x1E, 0x1B),
    Color(255, 0x24, 0x22, 0x20), Color(255, 0x36, 0x32, 0x2D),
    Color(255, 0xD9, 0x77, 0x57), Color(255, 0xE5, 0x86, 0x66),
    Color(255, 0xF5, 0xF1, 0xEA), Color(255, 0xA8, 0xA3, 0x9A),
    Color(255, 0x6B, 0x6A, 0x67),
    Color(255, 0x1E, 0x1B, 0x18), Color(255, 0x36, 0x30, 0x29),
    Color( 80, 0, 0, 0),          Color(140, 0, 0, 0),
    Color(180, 0, 0, 0)
};

bool g_dark = false;
const Palette& palette() { return g_dark ? kDark : kLight; }

// =====================================================================
// Curves
// =====================================================================
namespace curve {
inline float easeOutQuint(float t) { float i=1-t; return 1-i*i*i*i*i; }
inline float easeOutCubic(float t) { float i=1-t; return 1-i*i*i; }
inline float easeOutBack(float t)  {
    const float c1=1.70158f, c3=c1+1; float i=t-1;
    return 1 + c3*i*i*i + c1*i*i;
}
inline float easeInOutCubic(float t) {
    return t < 0.5f ? 4*t*t*t : 1 - std::pow(-2*t+2, 3) * 0.5f;
}
}

// =====================================================================
// I18N
// =====================================================================
enum class Lang { En, ZhCN, JaJP };
Lang g_lang = Lang::ZhCN;

const char* tr(const char* key) {
    struct E { const char* k; const char* en; const char* cn; const char* ja; };
    static const E T[] = {
        // 应用整体
        {"app.name",            "Launcher",         u8"启动器",            u8"ランチャー"},
        {"app.tagline",         "Private launcher", u8"私人启动器",        u8"プライベート"},
        {"loading.connecting",  "Connecting...",    u8"连接中…",           u8"接続中…"},
        // Greet
        {"home.greet",          "Hello, {nickname}",u8"你好，{nickname}",  u8"こんにちは、{nickname}"},
        {"home.subtitle",       "Welcome back",     u8"欢迎回来",          u8"おかえりなさい"},
        {"home.tier",           "Plan",             u8"订阅档位",          u8"プラン"},
        {"home.expires",        "Expires",          u8"到期时间",          u8"期限"},
        {"home.device_id",      "Device ID",        u8"设备 ID",           u8"デバイス ID"},
        {"home.last_login",     "Last sign in",     u8"上次登录",          u8"前回のサインイン"},
        {"home.history",        "View history",     u8"查看历史",          u8"履歴を表示"},
        // 菜单
        {"menu.home",           "Home",             u8"主页",              u8"ホーム"},
        {"menu.lunching",       "Lunching",         u8"Lunching",          u8"Lunching"},
        {"menu.cloud",          "Cloud",            u8"云端",              u8"クラウド"},
        {"menu.settings",       "Settings",         u8"设置",              u8"設定"},
        // 头像下拉
        {"acc.profile",         "My Profile",       u8"个人信息",          u8"プロフィール"},
        {"acc.history",         "Login history",    u8"历史登录",          u8"ログイン履歴"},
        {"acc.nickname",        "Edit nickname",    u8"修改昵称",          u8"ニックネーム変更"},
        {"acc.avatar",          "Change avatar",    u8"修改头像",          u8"アバター変更"},
        {"acc.password",        "Change password",  u8"修改密码",          u8"パスワード変更"},
        {"acc.signout",         "Sign out",         u8"退出登录",          u8"サインアウト"},
        // Lunching
        {"lunching.launch",     "Launch",           u8"发射",              u8"起動"},
        {"lunching.preparing",  "Preparing...",     u8"准备中…",           u8"準備中…"},
        // Profile
        {"profile.title",       "My Profile",       u8"个人信息",          u8"プロフィール"},
        {"profile.uid",         "UID (immutable)",  u8"UID（不可修改）",    u8"UID（変更不可）"},
        {"profile.username",    "Username (immutable)", u8"用户名（不可修改）", u8"ユーザー名（変更不可）"},
        {"profile.nickname",    "Nickname",         u8"昵称",              u8"ニックネーム"},
        {"profile.nickname_hint","You can change every 7 days", u8"每 7 天可改一次", u8"7日に1回変更可能"},
        {"profile.change_pw",   "Change password",  u8"修改密码",          u8"パスワード変更"},
        {"profile.upload_avatar","Upload avatar",   u8"上传头像",          u8"アバターアップロード"},
        {"profile.upload_hint", "PNG / JPEG / GIF, up to 5MB", u8"PNG / JPEG / GIF，最大 5MB", u8"PNG / JPEG / GIF、最大 5MB"},
        {"profile.save",        "Save",             u8"保存",              u8"保存"},
        {"profile.cancel",      "Cancel",           u8"取消",              u8"キャンセル"},
        // History overlay
        {"hist.title",          "Login history",    u8"历史登录",          u8"ログイン履歴"},
        {"hist.col_time",       "Time",             u8"时间",              u8"時刻"},
        {"hist.col_status",     "Status",           u8"状态",              u8"状態"},
        {"hist.col_ip",         "IP",               u8"IP",                u8"IP"},
        {"hist.col_device",     "Device",           u8"设备",              u8"デバイス"},
        {"hist.success",        "OK",               u8"成功",              u8"成功"},
        {"hist.failed",         "Failed",           u8"失败",              u8"失敗"},
        {"hist.close",          "Close",            u8"关闭",              u8"閉じる"},
        // Settings
        {"settings.language",   "Language",         u8"语言",              u8"言語"},
        {"settings.theme",      "Theme",            u8"主题",              u8"テーマ"},
        {"settings.theme_light","Light",            u8"亮色",              u8"ライト"},
        {"settings.theme_dark", "Dark",             u8"暗色",              u8"ダーク"},
        {"settings.about",      "About",            u8"关于",              u8"情報"},
        // Cloud
        {"cloud.title",         "Cloud",            u8"云端管理",          u8"クラウド"},
        {"cloud.no_data",       "Nothing synced",   u8"尚未同步",          u8"未同期"},
        // Tier
        {"tier.1week",          "1 week",           u8"1 周",              u8"1週間"},
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

// =====================================================================
// Tween
// =====================================================================
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

// =====================================================================
// 状态
// =====================================================================
enum class Stage { Loading, Expanding, Auth, Main };
enum class AuthMode { Login, Register };
enum class View  { Home, Lunching, Cloud, Settings, Profile };
enum class Overlay { None, History, EditNickname, ChangePassword };

Stage    g_stage    = Stage::Loading;
AuthMode g_auth_mode = AuthMode::Login;
View    g_view  = View::Home;
View    g_view_prev = View::Home;
Overlay g_overlay = Overlay::None;

// Auth 表单状态
struct AuthForm {
    HWND  edit_username = nullptr;
    HWND  edit_password = nullptr;
    HWND  edit_invite   = nullptr;
    HFONT font          = nullptr;
    std::wstring error_msg;
    bool  busy = false;        // 提交中（disable 按钮）
} g_auth;
Tween g_auth_card_y, g_auth_card_op;   // Auth 入场动画
bool    g_account_dropdown = false;     // 头像下拉是否打开

Tween g_card_scale, g_card_opacity, g_card_fade_out;
Tween g_window_w, g_window_h;
Tween g_sidebar_x, g_topbar_y, g_main_opacity;
Tween g_view_fade;          // 0→1 淡入新 view
Tween g_dropdown_t;         // 0→1 下拉展开
Tween g_overlay_t;          // 0→1 遮罩淡入

float g_time_in_stage = 0.0f;
float g_spin_angle = 0.0f;
HWND  g_hwnd = nullptr;
POINT g_mouse{-1, -1};

struct UserInfo {
    const wchar_t* uid       = L"K8RX2QZP";
    const wchar_t* username  = L"dwgx";
    const wchar_t* nickname  = L"dwgx";
    const wchar_t* email     = L"dwgx1337@outlook.com";
    const wchar_t* device_id = L"f8a1c2d4...e5b6 (TPM bound)";
    const wchar_t* expires   = L"2026-05-09 14:32 UTC";
    const wchar_t* last_login= L"2026-05-02 10:32  ·  beijing.cn";
} g_user;

// =====================================================================
// 工具
// =====================================================================
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
    Font font(L"Segoe UI", size, fs, UnitPoint);
    SolidBrush b(color);
    StringFormat fmt; fmt.SetAlignment(ha);
    RectF r(x, y, w, size * 3);
    g.DrawString(text, -1, &font, r, &fmt, &b);
}
std::wstring W(const char* utf8) {
    if (!utf8) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    std::wstring w(n - 1, 0);
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w.data(), n);
    return w;
}
struct HitArea { RectF rect; std::function<void()> on_click; };
std::vector<HitArea> g_hits;
void hit(RectF r, std::function<void()> fn) { g_hits.push_back({r, std::move(fn)}); }
bool inRect(POINT p, RectF r) {
    return p.x >= r.X && p.x <= r.X + r.Width && p.y >= r.Y && p.y <= r.Y + r.Height;
}

// 替换字符串中的 {nickname} 占位
std::wstring replaceNick(const wchar_t* tmpl, const wchar_t* v) {
    std::wstring s(tmpl);
    auto pos = s.find(L"{nickname}");
    if (pos != std::wstring::npos) s.replace(pos, 10, v);
    return s;
}

void switchView(View v) {
    if (v == g_view) return;
    g_view_prev = g_view;
    g_view = v;
    g_view_fade.start(0.0f, 1.0f, 0.30f, 0.0f, curve::easeOutQuint);
}

// =====================================================================
// Loading
// =====================================================================
void paintLoading(Graphics& g, int Wpx, int Hpx) {
    const Palette& pal = palette();
    SolidBrush bg(pal.bg);
    g.FillRectangle(&bg, 0, 0, Wpx, Hpx);

    float scale = g_card_scale.value();
    float op    = g_card_opacity.value();
    float exit  = g_card_fade_out.value();
    float opacity = op * (1.0f - exit);

    const float cw = 168.0f * scale, ch = 168.0f * scale;
    const float cx = (Wpx - cw) / 2.0f;
    const float cy = (Hpx - ch) / 2.0f - 14.0f * exit;

    if (opacity <= 0.001f) return;

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

// =====================================================================
// Topbar / Avatar dropdown
// =====================================================================
const float kSidebarW = 200.0f;
const float kTopbarH  = 48.0f;

struct MenuEntry { View view; const char* key; const wchar_t* glyph; };
const MenuEntry kMenu[] = {
    { View::Home,     "menu.home",     L"◉" },
    { View::Lunching, "menu.lunching", L"⚡" },
    { View::Cloud,    "menu.cloud",    L"☁" },
    { View::Settings, "menu.settings", L"⚙" },
};

void paintAccountDropdown(Graphics& g, int Wpx) {
    if (!g_account_dropdown && g_dropdown_t.value() < 0.001f) return;
    const Palette& pal = palette();

    float t = g_dropdown_t.value();
    float dw = 220.0f, dh = 252.0f;
    float dx = Wpx - 16.0f - dw;
    float dy = kTopbarH + 4.0f - 6.0f * (1.0f - t);

    BYTE a = (BYTE)(255 * t);
    if (a == 0) return;

    Color cardC(a, pal.card.GetR(), pal.card.GetG(), pal.card.GetB());
    Color shC((BYTE)(pal.shadow_card_hover.GetA() * t), 0, 0, 0);
    drawShadow(g, dx, dy, dw, dh, 12.0f, shC, 6.0f, 4);
    fillRR(g, dx, dy, dw, dh, 12.0f, cardC);

    auto fade = [&](Color c) { return Color((BYTE)(c.GetA() * t), c.GetR(), c.GetG(), c.GetB()); };

    // 顶部用户简介
    drawText_(g, g_user.nickname, dx + 16, dy + 16, dw - 32,
              13.0f, fade(pal.text), StringAlignmentNear, FontStyleBold);
    wchar_t uid_line[64];
    swprintf_s(uid_line, 64, L"UID  %ls", g_user.uid);
    drawText_(g, uid_line, dx + 16, dy + 38, dw - 32,
              8.5f, fade(pal.text_muted));

    // 分割线
    Pen sep(fade(pal.divider), 1.0f);
    g.DrawLine(&sep, dx + 12, dy + 64, dx + dw - 12, dy + 64);

    // 菜单条目
    struct Item { const char* key; const wchar_t* glyph; std::function<void()> click; };
    Item items[] = {
        { "acc.profile",  L"◐", [](){ switchView(View::Profile);  g_account_dropdown = false; } },
        { "acc.history",  L"⏱", [](){ g_overlay = Overlay::History; g_overlay_t.start(0,1,0.25f,0,curve::easeOutCubic); g_account_dropdown = false; } },
        { "acc.nickname", L"✎", [](){ g_overlay = Overlay::EditNickname; g_overlay_t.start(0,1,0.25f,0,curve::easeOutCubic); g_account_dropdown = false; } },
        { "acc.avatar",   L"☷", [](){ /* TODO file picker */ g_account_dropdown = false; } },
        { "acc.password", L"⚿", [](){ g_overlay = Overlay::ChangePassword; g_overlay_t.start(0,1,0.25f,0,curve::easeOutCubic); g_account_dropdown = false; } },
        { "acc.signout",  L"⏻", [](){ /* TODO logout */ g_account_dropdown = false; } },
    };
    float iy = dy + 76;
    for (auto& it : items) {
        RectF r(dx + 8, iy, dw - 16, 30);
        bool hover = inRect(g_mouse, r);
        if (hover) {
            Color hc((BYTE)(40 * t), pal.text.GetR(), pal.text.GetG(), pal.text.GetB());
            fillRR(g, r.X, r.Y, r.Width, r.Height, 6.0f, hc);
        }
        Color tc = fade(pal.text);
        if (strcmp(it.key, "acc.signout") == 0) {
            tc = fade(Color(255, 0xE3, 0x4B, 0x4B));   // 退出登录染红
        }
        drawText_(g, it.glyph, r.X + 12, r.Y + 8, 16, 11.0f, tc);
        drawText_(g, W(tr(it.key)).c_str(), r.X + 36, r.Y + 8, r.Width - 50,
                  9.0f, tc);
        hit(r, it.click);
        iy += 30;
    }
}

void paintTopbar(Graphics& g, int Wpx) {
    const Palette& pal = palette();
    float ty = -kTopbarH * (1.0f - g_topbar_y.value());

    SolidBrush bg(pal.bg);
    g.FillRectangle(&bg, 0.0f, ty, (REAL)Wpx, kTopbarH);
    Pen sep(pal.divider, 1.0f);
    g.DrawLine(&sep, 0.0f, ty + kTopbarH, (REAL)Wpx, ty + kTopbarH);

    drawText_(g, W(tr("app.name")).c_str(), 16.0f, ty + 14.0f, 200.0f, 11.0f,
              pal.text, StringAlignmentNear, FontStyleBold);
    drawText_(g, W(tr("app.tagline")).c_str(), 84.0f, ty + 18.0f, 200.0f, 8.0f,
              pal.text_faint);

    // 右上：nickname + 头像 (clickable)
    drawText_(g, g_user.nickname, (REAL)Wpx - 96.0f, ty + 16.0f, 50.0f,
              10.0f, pal.text_muted, StringAlignmentFar);

    float ar = 14.0f, ax = (REAL)Wpx - 16.0f - ar*2, ay = ty + (kTopbarH - ar*2)/2;
    SolidBrush avbg(pal.primary);
    g.FillEllipse(&avbg, ax, ay, ar*2, ar*2);
    Font af(L"Segoe UI", 10.0f, FontStyleBold, UnitPoint);
    SolidBrush avf(Color(255, 255, 255, 255));
    StringFormat fmt; fmt.SetAlignment(StringAlignmentCenter); fmt.SetLineAlignment(StringAlignmentCenter);
    RectF avrect(ax, ay, ar*2, ar*2);
    wchar_t initial[2] = { (wchar_t)towupper(g_user.nickname[0]), 0 };
    g.DrawString(initial, -1, &af, avrect, &fmt, &avf);

    // 头像 hit area —— 包括 nickname 文字 + 头像（点哪个都展开）
    RectF avHit((REAL)Wpx - 110.0f, ty + 6.0f, 100.0f, 36.0f);
    hit(avHit, [](){
        g_account_dropdown = !g_account_dropdown;
        if (g_account_dropdown) g_dropdown_t.start(0, 1, 0.18f, 0, curve::easeOutCubic);
        else                     g_dropdown_t.start(g_dropdown_t.value(), 0, 0.15f, 0, curve::easeOutCubic);
    });
}

// =====================================================================
// Sidebar
// =====================================================================
void paintSidebar(Graphics& g, int Hpx) {
    const Palette& pal = palette();
    float sx = -kSidebarW * (1.0f - g_sidebar_x.value());
    SolidBrush bg(pal.sidebar_bg);
    g.FillRectangle(&bg, sx, kTopbarH, kSidebarW, (REAL)Hpx - kTopbarH);
    Pen sep(pal.divider, 1.0f);
    g.DrawLine(&sep, sx + kSidebarW, kTopbarH, sx + kSidebarW, (REAL)Hpx);

    drawText_(g, W(tr("app.name")).c_str(), sx + 20, kTopbarH + 24, 160,
              14.0f, pal.text, StringAlignmentNear, FontStyleBold);
    drawText_(g, W(tr("app.tagline")).c_str(), sx + 20, kTopbarH + 50, 160,
              8.5f, pal.text_faint);

    float my = kTopbarH + 96.0f;
    for (auto& m : kMenu) {
        bool active = (m.view == g_view);
        RectF item(sx + 12.0f, my, kSidebarW - 24.0f, 36.0f);
        bool hover = inRect(g_mouse, item);
        if (active) {
            fillRR(g, item.X, item.Y, item.Width, item.Height, 8.0f, pal.sidebar_active);
            // 左侧 3px 主色指示条
            fillRR(g, item.X, item.Y + 4.0f, 3.0f, item.Height - 8.0f, 1.5f, pal.primary);
        } else if (hover) {
            Color hc(40, pal.text.GetR(), pal.text.GetG(), pal.text.GetB());
            fillRR(g, item.X, item.Y, item.Width, item.Height, 8.0f, hc);
        }
        Color tc = active ? pal.text : pal.text_muted;
        drawText_(g, m.glyph, item.X + 14.0f, item.Y + 11.0f, 16.0f,
                  11.0f, active ? pal.primary : tc);
        drawText_(g, W(tr(m.key)).c_str(), item.X + 38.0f, item.Y + 11.0f, item.Width - 50.0f,
                  9.5f, tc, StringAlignmentNear, active ? FontStyleBold : FontStyleRegular);
        View target = m.view;
        hit(item, [target]() { switchView(target); });
        my += 42.0f;
    }
}

// =====================================================================
// Views
// =====================================================================
void paintHomeView(Graphics& g, RectF area) {
    const Palette& pal = palette();
    float op = g_view_fade.started ? g_view_fade.value() : 1.0f;
    op = (g_main_opacity.value()) * op;
    auto fade = [&](Color c) { return Color((BYTE)(c.GetA() * op), c.GetR(), c.GetG(), c.GetB()); };

    auto greet = replaceNick(W(tr("home.greet")).c_str(), g_user.nickname);

    float ty = area.Y + 32.0f + (1.0f - op) * 12.0f;
    drawText_(g, greet.c_str(), area.X + 36, ty, area.Width - 72,
              22.0f, fade(pal.text), StringAlignmentNear, FontStyleBold);
    drawText_(g, W(tr("home.subtitle")).c_str(), area.X + 36, ty + 38, area.Width - 72,
              10.0f, fade(pal.text_muted));

    // 头像卡片
    float cardY = area.Y + 110 + (1.0f - op) * 16.0f;
    drawShadow(g, area.X + 36, cardY, 280, 280, 16.0f, fade(pal.shadow_card), 2.0f, 3);
    fillRR(g, area.X + 36, cardY, 280, 280, 16.0f, fade(pal.card));

    float avR = 56.0f;
    float avx = area.X + 36 + 140 - avR;
    float avy = cardY + 36;
    SolidBrush avBg(fade(pal.primary));
    g.FillEllipse(&avBg, avx, avy, avR*2, avR*2);
    Font af(L"Segoe UI", 32.0f, FontStyleBold, UnitPoint);
    SolidBrush avF(Color((BYTE)(255 * op), 255, 255, 255));
    StringFormat fmt; fmt.SetAlignment(StringAlignmentCenter); fmt.SetLineAlignment(StringAlignmentCenter);
    RectF avr(avx, avy, avR*2, avR*2);
    wchar_t initial[2] = { (wchar_t)towupper(g_user.nickname[0]), 0 };
    g.DrawString(initial, -1, &af, avr, &fmt, &avF);

    drawText_(g, g_user.nickname, area.X + 36, cardY + 168, 280,
              16.0f, fade(pal.text), StringAlignmentCenter, FontStyleBold);
    wchar_t uid_line[80];
    swprintf_s(uid_line, 80, L"UID  %ls", g_user.uid);
    drawText_(g, uid_line, area.X + 36, cardY + 200, 280,
              8.5f, fade(pal.text_muted), StringAlignmentCenter);

    SolidBrush green(Color((BYTE)(255 * op), 0x4C, 0xAF, 0x50));
    g.FillEllipse(&green, area.X + 52, cardY + 244, 8.0f, 8.0f);
    drawText_(g, L"Online", area.X + 68, cardY + 240, 100, 8.5f, fade(pal.text_muted));

    // 详情卡片
    float dx = area.X + 36 + 280 + 24;
    float dw = area.Width - 36 - 280 - 24 - 36;
    drawShadow(g, dx, cardY, dw, 280, 16.0f, fade(pal.shadow_card), 2.0f, 3);
    fillRR(g, dx, cardY, dw, 280, 16.0f, fade(pal.card));

    std::wstring tier_w = W(tr("tier.1week"));
    struct R { const char* lab; const wchar_t* val; };
    R rows[] = {
        { "home.tier",       tier_w.c_str()      },
        { "home.expires",    g_user.expires      },
        { "home.device_id",  g_user.device_id    },
        { "home.last_login", g_user.last_login   },
    };
    float ry = cardY + 28;
    for (int i = 0; i < 4; ++i) {
        drawText_(g, W(tr(rows[i].lab)).c_str(), dx + 28, ry, 250,
                  8.5f, fade(pal.text_muted));
        drawText_(g, rows[i].val, dx + 28, ry + 22, dw - 56,
                  11.0f, fade(pal.text), StringAlignmentNear, FontStyleBold);
        // last login 行加一个 "查看历史" link
        if (i == 3) {
            float lx = dx + 28;
            float ly = ry + 22;
            // 估算文字宽度后右侧加一个按钮
            RectF link(dx + dw - 28 - 90, ly, 90, 20);
            bool hov = inRect(g_mouse, link);
            drawText_(g, W(tr("home.history")).c_str(), link.X, link.Y + 4, link.Width,
                      9.0f, hov ? fade(pal.primary_hover) : fade(pal.primary),
                      StringAlignmentFar, FontStyleBold);
            hit(link, [](){
                g_overlay = Overlay::History;
                g_overlay_t.start(0, 1, 0.25f, 0, curve::easeOutCubic);
            });
        }
        if (i < 3) {
            Color sepC((BYTE)(pal.divider.GetA() * op), pal.divider.GetR(), pal.divider.GetG(), pal.divider.GetB());
            Pen p(sepC, 1.0f);
            g.DrawLine(&p, dx + 28.0f, ry + 56.0f, dx + dw - 28.0f, ry + 56.0f);
        }
        ry += 60.0f;
    }
}

void paintLunchingView(Graphics& g, RectF area) {
    const Palette& pal = palette();
    float op = g_view_fade.started ? g_view_fade.value() : 1.0f;
    op = (g_main_opacity.value()) * op;
    auto fade = [&](Color c) { return Color((BYTE)(c.GetA() * op), c.GetR(), c.GetG(), c.GetB()); };

    drawText_(g, W(tr("menu.lunching")).c_str(), area.X + 36, area.Y + 32, 400,
              22.0f, fade(pal.text), StringAlignmentNear, FontStyleBold);
    drawText_(g, L"Click any tile to launch.", area.X + 36, area.Y + 70, 400,
              10.0f, fade(pal.text_muted));

    const float gap = 16.0f;
    const float cardW = (area.Width - 72.0f - gap * 3) / 4.0f;
    const float cardH = 140.0f;

    const wchar_t* games[] = {
        L"Genshin Impact", L"Counter-Strike 2", L"Valorant", L"Apex Legends",
        L"Cyberpunk 2077", L"Elden Ring",       L"Helldivers 2", L"Palworld",
        L"League of Legends", L"Overwatch 2",   L"Fortnite",     L"Dota 2"
    };
    int idx = 0;
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 4 && idx < 12; ++col, ++idx) {
            float x = area.X + 36 + col * (cardW + gap);
            float y = area.Y + 110 + row * (cardH + gap) + (1.0f - op) * (8 + idx * 2);
            bool hover = inRect(g_mouse, RectF(x, y, cardW, cardH));
            float lift = hover ? 2.0f : 0.0f;

            drawShadow(g, x, y - lift, cardW, cardH, 12.0f,
                       hover ? fade(pal.shadow_card_hover) : fade(pal.shadow_card),
                       hover ? 4.0f : 1.0f, hover ? 4 : 2);
            fillRR(g, x, y - lift, cardW, cardH, 12.0f, fade(pal.card));

            fillRR(g, x, y - lift, cardW, 28.0f, 12.0f, fade(pal.primary));
            SolidBrush prb(fade(pal.primary));
            g.FillRectangle(&prb, x, y - lift + 14.0f, cardW, 14.0f);

            drawText_(g, games[idx], x + 14, y - lift + 38, cardW - 28,
                      11.0f, fade(pal.text), StringAlignmentNear, FontStyleBold);
            wchar_t ver[32];
            swprintf_s(ver, 32, L"v1.%d.%d", row + 2, col * 3);
            drawText_(g, ver, x + 14, y - lift + 62, cardW - 28,
                      8.5f, fade(pal.text_muted));

            if (hover) {
                drawText_(g, W(tr("lunching.launch")).c_str(),
                          x + 14, y - lift + cardH - 26, cardW - 28,
                          9.5f, fade(pal.primary), StringAlignmentNear, FontStyleBold);
            }
        }
    }
}

void paintCloudView(Graphics& g, RectF area) {
    const Palette& pal = palette();
    float op = g_view_fade.started ? g_view_fade.value() : 1.0f;
    op = (g_main_opacity.value()) * op;
    auto fade = [&](Color c) { return Color((BYTE)(c.GetA() * op), c.GetR(), c.GetG(), c.GetB()); };

    drawText_(g, W(tr("cloud.title")).c_str(), area.X + 36, area.Y + 32, 400,
              22.0f, fade(pal.text), StringAlignmentNear, FontStyleBold);

    float ph_y = area.Y + 100;
    drawShadow(g, area.X + 36, ph_y, area.Width - 72, 320, 16.0f, fade(pal.shadow_card), 2.0f, 3);
    fillRR(g, area.X + 36, ph_y, area.Width - 72, 320, 16.0f, fade(pal.card));

    drawText_(g, L"☁", area.X + 36, ph_y + 80, area.Width - 72,
              48.0f, fade(pal.text_faint), StringAlignmentCenter);
    drawText_(g, W(tr("cloud.no_data")).c_str(), area.X + 36, ph_y + 200, area.Width - 72,
              11.0f, fade(pal.text_muted), StringAlignmentCenter);
}

void paintSettingsView(Graphics& g, RectF area) {
    const Palette& pal = palette();
    float op = g_view_fade.started ? g_view_fade.value() : 1.0f;
    op = (g_main_opacity.value()) * op;
    auto fade = [&](Color c) { return Color((BYTE)(c.GetA() * op), c.GetR(), c.GetG(), c.GetB()); };

    drawText_(g, W(tr("menu.settings")).c_str(), area.X + 36, area.Y + 32, 400,
              22.0f, fade(pal.text), StringAlignmentNear, FontStyleBold);

    // Language
    float sy = area.Y + 96;
    drawText_(g, W(tr("settings.language")).c_str(), area.X + 36, sy, 400,
              12.0f, fade(pal.text), StringAlignmentNear, FontStyleBold);
    sy += 32;
    struct LB { Lang l; const wchar_t* label; };
    LB langs[] = { {Lang::En, L"English"}, {Lang::ZhCN, L"简体中文"}, {Lang::JaJP, L"日本語"} };
    float bx = area.X + 36;
    for (auto& lb : langs) {
        float bw = 110, bh = 36;
        bool active = (g_lang == lb.l);
        bool hover  = inRect(g_mouse, RectF(bx, sy, bw, bh));
        Color bgc = active ? fade(pal.primary)
                           : (hover ? fade(pal.surface) : fade(pal.card));
        Color fgc = active ? Color((BYTE)(255 * op), 255, 255, 255) : fade(pal.text);
        fillRR(g, bx, sy, bw, bh, 8.0f, bgc);
        if (!active) strokeRR(g, bx, sy, bw, bh, 8.0f, fade(pal.divider));
        drawText_(g, lb.label, bx, sy + 11, bw, 9.5f, fgc,
                  StringAlignmentCenter, active ? FontStyleBold : FontStyleRegular);
        Lang t = lb.l;
        hit(RectF(bx, sy, bw, bh), [t]() {
            if (g_lang != t) {
                g_lang = t;
                g_view_fade.start(0.5f, 1.0f, 0.20f, 0, curve::easeOutCubic);
            }
        });
        bx += bw + 12;
    }

    // Theme
    sy += 64;
    drawText_(g, W(tr("settings.theme")).c_str(), area.X + 36, sy, 400,
              12.0f, fade(pal.text), StringAlignmentNear, FontStyleBold);
    sy += 32;
    struct TB { bool dark; const char* labelKey; };
    TB themes[] = { {false, "settings.theme_light"}, {true, "settings.theme_dark"} };
    bx = area.X + 36;
    for (auto& tb : themes) {
        float bw = 110, bh = 36;
        bool active = (g_dark == tb.dark);
        bool hover  = inRect(g_mouse, RectF(bx, sy, bw, bh));
        Color bgc = active ? fade(pal.primary)
                           : (hover ? fade(pal.surface) : fade(pal.card));
        Color fgc = active ? Color((BYTE)(255 * op), 255, 255, 255) : fade(pal.text);
        fillRR(g, bx, sy, bw, bh, 8.0f, bgc);
        if (!active) strokeRR(g, bx, sy, bw, bh, 8.0f, fade(pal.divider));
        drawText_(g, W(tr(tb.labelKey)).c_str(), bx, sy + 11, bw, 9.5f, fgc,
                  StringAlignmentCenter, active ? FontStyleBold : FontStyleRegular);
        bool t = tb.dark;
        hit(RectF(bx, sy, bw, bh), [t]() {
            if (g_dark != t) {
                g_dark = t;
                g_view_fade.start(0.6f, 1.0f, 0.25f, 0, curve::easeOutCubic);
            }
        });
        bx += bw + 12;
    }

    // About
    sy += 64;
    drawText_(g, W(tr("settings.about")).c_str(), area.X + 36, sy, 400,
              12.0f, fade(pal.text), StringAlignmentNear, FontStyleBold);
    sy += 28;
    drawText_(g, L"Launcher v0.1.0", area.X + 36, sy, 400, 9.5f, fade(pal.text_muted));
    sy += 20;
    drawText_(g, L"© 2026 dwgx", area.X + 36, sy, 400, 9.0f, fade(pal.text_faint));
    sy += 18;
    drawText_(g, L"Skia + Clay + GLFW + libcurl + SQLite", area.X + 36, sy, 400, 8.5f, fade(pal.text_faint));
}

void paintProfileView(Graphics& g, RectF area) {
    const Palette& pal = palette();
    float op = g_view_fade.started ? g_view_fade.value() : 1.0f;
    op = (g_main_opacity.value()) * op;
    auto fade = [&](Color c) { return Color((BYTE)(c.GetA() * op), c.GetR(), c.GetG(), c.GetB()); };

    drawText_(g, W(tr("profile.title")).c_str(), area.X + 36, area.Y + 32, 400,
              22.0f, fade(pal.text), StringAlignmentNear, FontStyleBold);

    // 大头像 + 上传按钮
    float cardY = area.Y + 96;
    drawShadow(g, area.X + 36, cardY, 280, 320, 16.0f, fade(pal.shadow_card), 2.0f, 3);
    fillRR(g, area.X + 36, cardY, 280, 320, 16.0f, fade(pal.card));

    float avR = 56.0f;
    float avx = area.X + 36 + 140 - avR;
    float avy = cardY + 32;
    SolidBrush avBg(fade(pal.primary));
    g.FillEllipse(&avBg, avx, avy, avR*2, avR*2);
    Font af(L"Segoe UI", 32.0f, FontStyleBold, UnitPoint);
    SolidBrush avF(Color((BYTE)(255 * op), 255, 255, 255));
    StringFormat fmt; fmt.SetAlignment(StringAlignmentCenter); fmt.SetLineAlignment(StringAlignmentCenter);
    RectF avr(avx, avy, avR*2, avR*2);
    wchar_t initial[2] = { (wchar_t)towupper(g_user.nickname[0]), 0 };
    g.DrawString(initial, -1, &af, avr, &fmt, &avF);

    // 上传按钮
    RectF upBtn(area.X + 36 + 70, cardY + 168, 140, 36);
    bool hov = inRect(g_mouse, upBtn);
    Color upbg = hov ? fade(pal.primary_hover) : fade(pal.primary);
    fillRR(g, upBtn.X, upBtn.Y, upBtn.Width, upBtn.Height, 8.0f, upbg);
    drawText_(g, W(tr("profile.upload_avatar")).c_str(), upBtn.X, upBtn.Y + 11,
              upBtn.Width, 9.5f, Color((BYTE)(255 * op), 255, 255, 255),
              StringAlignmentCenter, FontStyleBold);
    drawText_(g, W(tr("profile.upload_hint")).c_str(), area.X + 36, cardY + 220, 280,
              8.5f, fade(pal.text_muted), StringAlignmentCenter);

    // 改密按钮
    RectF pwBtn(area.X + 36 + 70, cardY + 256, 140, 36);
    bool pwhov = inRect(g_mouse, pwBtn);
    fillRR(g, pwBtn.X, pwBtn.Y, pwBtn.Width, pwBtn.Height, 8.0f, fade(pal.card));
    strokeRR(g, pwBtn.X, pwBtn.Y, pwBtn.Width, pwBtn.Height, 8.0f,
             pwhov ? fade(pal.primary) : fade(pal.divider));
    drawText_(g, W(tr("profile.change_pw")).c_str(), pwBtn.X, pwBtn.Y + 11,
              pwBtn.Width, 9.5f, fade(pal.text),
              StringAlignmentCenter, FontStyleBold);

    // 详情卡片：UID（不可改）/ Username（不可改）/ Nickname（可改）
    float dx = area.X + 36 + 280 + 24;
    float dw = area.Width - 36 - 280 - 24 - 36;
    drawShadow(g, dx, cardY, dw, 320, 16.0f, fade(pal.shadow_card), 2.0f, 3);
    fillRR(g, dx, cardY, dw, 320, 16.0f, fade(pal.card));

    auto field = [&](float fy, const char* labelKey, const wchar_t* val,
                     bool editable, const wchar_t* hint = nullptr) {
        drawText_(g, W(tr(labelKey)).c_str(), dx + 28, fy, dw - 56,
                  8.5f, fade(pal.text_muted));
        // 输入框样式
        RectF box(dx + 28, fy + 22, dw - 56, 36);
        Color boxBg = editable ? fade(pal.surface) : fade(pal.bg);
        fillRR(g, box.X, box.Y, box.Width, box.Height, 8.0f, boxBg);
        strokeRR(g, box.X, box.Y, box.Width, box.Height, 8.0f, fade(pal.divider));
        drawText_(g, val, box.X + 12, box.Y + 11, box.Width - 24,
                  10.5f, fade(editable ? pal.text : pal.text_faint),
                  StringAlignmentNear, editable ? FontStyleBold : FontStyleRegular);
        if (!editable) {
            // 锁图标
            drawText_(g, L"🔒", box.X + box.Width - 28, box.Y + 9, 16, 10.0f,
                      fade(pal.text_faint));
        }
        if (hint) {
            drawText_(g, hint, dx + 28, fy + 64, dw - 56, 8.0f, fade(pal.text_faint));
        }
    };

    field(cardY + 28,  "profile.uid",      g_user.uid,      false);
    field(cardY + 110, "profile.username", g_user.username, false);
    field(cardY + 192, "profile.nickname", g_user.nickname, true,
          W(tr("profile.nickname_hint")).c_str());
}

// =====================================================================
// Overlays
// =====================================================================
void paintHistoryOverlay(Graphics& g, int Wpx, int Hpx) {
    const Palette& pal = palette();
    float t = g_overlay_t.value();
    if (t < 0.001f) return;

    // 半透明遮罩
    Color dim((BYTE)(pal.overlay_dim.GetA() * t), 0, 0, 0);
    SolidBrush bg(dim);
    g.FillRectangle(&bg, 0, 0, Wpx, Hpx);

    // 居中卡片
    float cw = 720, ch = 480;
    float cx = (Wpx - cw) / 2;
    float cy = (Hpx - ch) / 2 + 12 * (1.0f - t);
    auto fade = [&](Color c) { return Color((BYTE)(c.GetA() * t), c.GetR(), c.GetG(), c.GetB()); };
    Color cardC((BYTE)(255 * t), pal.card.GetR(), pal.card.GetG(), pal.card.GetB());
    drawShadow(g, cx, cy, cw, ch, 16.0f, Color((BYTE)(80 * t), 0, 0, 0), 8.0f, 6);
    fillRR(g, cx, cy, cw, ch, 16.0f, cardC);

    // 头部
    drawText_(g, W(tr("hist.title")).c_str(), cx + 32, cy + 24, cw - 64,
              18.0f, fade(pal.text), StringAlignmentNear, FontStyleBold);

    // 关闭 X
    RectF x(cx + cw - 44, cy + 16, 28, 28);
    bool xh = inRect(g_mouse, x);
    if (xh) fillRR(g, x.X, x.Y, x.Width, x.Height, 6.0f, fade(pal.surface));
    drawText_(g, L"✕", x.X, x.Y + 6, x.Width, 12.0f, fade(pal.text), StringAlignmentCenter);
    hit(x, [](){ g_overlay = Overlay::None; g_overlay_t.start(g_overlay_t.value(), 0, 0.18f, 0, curve::easeOutCubic); });

    // 表头
    Pen sep(fade(pal.divider), 1.0f);
    g.DrawLine(&sep, cx + 32, cy + 64, cx + cw - 32, cy + 64);
    float ry = cy + 76;
    struct Col { const char* k; float w; };
    Col cols[] = {
        { "hist.col_time",   200 },
        { "hist.col_status", 100 },
        { "hist.col_ip",     180 },
        { "hist.col_device", 160 },
    };
    float colx = cx + 32;
    for (auto& c : cols) {
        drawText_(g, W(tr(c.k)).c_str(), colx, ry, c.w, 8.5f, fade(pal.text_muted));
        colx += c.w;
    }
    g.DrawLine(&sep, cx + 32, ry + 22, cx + cw - 32, ry + 22);

    // 假数据 8 行
    struct R { const wchar_t* time; bool ok; const wchar_t* ip; const wchar_t* dev; };
    R rows[] = {
        { L"2026-05-02 10:32",  true,  L"114.215.182.41", L"f8a1c2d4...e5b6" },
        { L"2026-05-01 22:14",  true,  L"114.215.182.41", L"f8a1c2d4...e5b6" },
        { L"2026-05-01 08:02",  false, L"45.32.198.7",    L"unknown"          },
        { L"2026-04-30 18:51",  true,  L"114.215.182.41", L"f8a1c2d4...e5b6" },
        { L"2026-04-30 09:08",  true,  L"114.215.182.41", L"f8a1c2d4...e5b6" },
        { L"2026-04-29 19:43",  true,  L"114.215.182.41", L"f8a1c2d4...e5b6" },
        { L"2026-04-29 10:11",  true,  L"114.215.182.41", L"f8a1c2d4...e5b6" },
        { L"2026-04-28 22:30",  true,  L"114.215.182.41", L"f8a1c2d4...e5b6" },
    };
    ry += 36;
    for (auto& r : rows) {
        colx = cx + 32;
        drawText_(g, r.time, colx, ry, 200, 9.5f, fade(pal.text));
        colx += 200;
        Color statusC = r.ok ? fade(Color(255, 0x4C, 0xAF, 0x50))
                             : fade(Color(255, 0xE3, 0x4B, 0x4B));
        // 状态徽章
        const wchar_t* statusText = r.ok ? W(tr("hist.success")).c_str()
                                         : W(tr("hist.failed")).c_str();
        drawText_(g, statusText, colx, ry, 100, 9.5f, statusC,
                  StringAlignmentNear, FontStyleBold);
        colx += 100;
        drawText_(g, r.ip,  colx, ry, 180, 9.5f, fade(pal.text_muted));
        colx += 180;
        drawText_(g, r.dev, colx, ry, 160, 9.5f, fade(pal.text_muted));
        ry += 32;
    }

    // 点击外部关闭
    RectF outer(0, 0, (REAL)Wpx, (REAL)Hpx);
    RectF inner(cx, cy, cw, ch);
    hit(outer, [inner](){
        if (!inRect(g_mouse, inner)) {
            g_overlay = Overlay::None;
            g_overlay_t.start(g_overlay_t.value(), 0, 0.18f, 0, curve::easeOutCubic);
        }
    });
}

// =====================================================================
// Main paint
// =====================================================================
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
        case View::Cloud:    paintCloudView(g, area);    break;
        case View::Settings: paintSettingsView(g, area); break;
        case View::Profile:  paintProfileView(g, area);  break;
    }

    // overlay 在 view 之上
    if (g_overlay == Overlay::History || g_overlay_t.value() > 0.001f) {
        paintHistoryOverlay(g, Wpx, Hpx);
    }

    // 头像下拉在 overlay 之上（操作可达性）
    paintAccountDropdown(g, Wpx);
}

// =====================================================================
// Auth view (登录 / 注册)
// =====================================================================
const int IDC_USERNAME    = 1001;
const int IDC_PASSWORD    = 1002;
const int IDC_INVITE      = 1003;

void createAuthControls(HWND parent) {
    LOGFONTW lf{};
    lf.lfHeight = -16;
    wcscpy_s(lf.lfFaceName, L"Segoe UI");
    g_auth.font = CreateFontIndirectW(&lf);
    auto mk = [&](int id) {
        HWND h = CreateWindowExW(0, L"EDIT", L"",
            WS_CHILD | WS_BORDER | ES_AUTOHSCROLL,
            0, 0, 320, 36, parent, (HMENU)(INT_PTR)id, GetModuleHandle(nullptr), nullptr);
        SendMessageW(h, WM_SETFONT, (WPARAM)g_auth.font, TRUE);
        return h;
    };
    g_auth.edit_username = mk(IDC_USERNAME);
    g_auth.edit_password = mk(IDC_PASSWORD);
    SendMessageW(g_auth.edit_password, EM_SETPASSWORDCHAR, (WPARAM)L'*', 0);
    g_auth.edit_invite   = mk(IDC_INVITE);
}

void layoutAuthControls(int Wpx, int Hpx, bool register_mode) {
    // 卡片 380×（420 or 480），居中
    const int cw = 380;
    const int ch = register_mode ? 480 : 420;
    const int cx = (Wpx - cw) / 2;
    const int cy = (Hpx - ch) / 2;

    const int ix = cx + 32, iw = cw - 64, ih = 36;
    int row_y = cy + 130;          // 第一行 input 顶部 y
    SetWindowPos(g_auth.edit_username, nullptr, ix, row_y, iw, ih, SWP_NOZORDER);
    row_y += 76;
    SetWindowPos(g_auth.edit_password, nullptr, ix, row_y, iw, ih, SWP_NOZORDER);
    row_y += 76;
    if (register_mode) {
        SetWindowPos(g_auth.edit_invite, nullptr, ix, row_y, iw, ih, SWP_NOZORDER);
    }
}

void showAuthControls(bool register_mode) {
    ShowWindow(g_auth.edit_username, SW_SHOW);
    ShowWindow(g_auth.edit_password, SW_SHOW);
    ShowWindow(g_auth.edit_invite,   register_mode ? SW_SHOW : SW_HIDE);
    SetFocus(g_auth.edit_username);
}

void paintAuthView(Graphics& g, int Wpx, int Hpx) {
    g_hits.clear();
    const Palette& pal = palette();
    SolidBrush bg(pal.bg);
    g.FillRectangle(&bg, 0, 0, Wpx, Hpx);

    bool reg = (g_auth_mode == AuthMode::Register);
    const float cw = 380.0f;
    const float ch = reg ? 480.0f : 420.0f;
    const float cx = (Wpx - cw) / 2.0f;
    const float cy = (Hpx - ch) / 2.0f + g_auth_card_y.value();
    float op = g_auth_card_op.value();
    if (op <= 0.001f) return;

    auto fade = [&](Color c) { return Color((BYTE)(c.GetA() * op), c.GetR(), c.GetG(), c.GetB()); };

    drawShadow(g, cx, cy, cw, ch, 16.0f, fade(pal.shadow_card_hover), 8.0f, 5);
    fillRR(g, cx, cy, cw, ch, 16.0f, fade(pal.card));

    // logo + title
    float lr = 22.0f;
    float lx = cx + 32, ly = cy + 32;
    SolidBrush lbg(fade(pal.primary));
    g.FillEllipse(&lbg, lx, ly, lr*2, lr*2);
    Font lf(L"Segoe UI", 16.0f, FontStyleBold, UnitPoint);
    SolidBrush lf_b(Color((BYTE)(255 * op), 255, 255, 255));
    StringFormat lfmt; lfmt.SetAlignment(StringAlignmentCenter); lfmt.SetLineAlignment(StringAlignmentCenter);
    RectF lr_rect(lx, ly, lr*2, lr*2);
    g.DrawString(L"L", -1, &lf, lr_rect, &lfmt, &lf_b);

    drawText_(g, reg ? L"创建账号" : L"登录到 Launcher", lx + lr*2 + 14, cy + 32, 220,
              16.0f, fade(pal.text), StringAlignmentNear, FontStyleBold);
    drawText_(g, reg ? L"用邀请码注册新账号" : L"输入用户名和密码继续",
              lx + lr*2 + 14, cy + 56, 220, 9.0f, fade(pal.text_muted));

    // labels
    drawText_(g, L"用户名", cx + 32, cy + 110, 200, 8.5f, fade(pal.text_muted));
    drawText_(g, L"密码",   cx + 32, cy + 186, 200, 8.5f, fade(pal.text_muted));
    if (reg) drawText_(g, L"邀请码", cx + 32, cy + 262, 200, 8.5f, fade(pal.text_muted));

    // Submit button
    float by = reg ? cy + 350.0f : cy + 274.0f;
    RectF btn(cx + 32, by, cw - 64, 42);
    bool bhov = inRect(g_mouse, btn);
    Color bbg = g_auth.busy
        ? fade(Color(255, 0x6B, 0x6A, 0x67))
        : (bhov ? fade(pal.primary_hover) : fade(pal.primary));
    fillRR(g, btn.X, btn.Y, btn.Width, btn.Height, 8.0f, bbg);
    drawText_(g, g_auth.busy ? L"正在连接…"
                              : (reg ? L"创建账号" : L"登录"),
              btn.X, btn.Y + 13, btn.Width, 11.0f,
              Color((BYTE)(255 * op), 255, 255, 255),
              StringAlignmentCenter, FontStyleBold);

    // Error
    if (!g_auth.error_msg.empty()) {
        Color err((BYTE)(255 * op), 0xE0, 0x5A, 0x5A);
        drawText_(g, g_auth.error_msg.c_str(),
                  cx + 32, by + 50, cw - 64, 9.0f, err);
    }

    // Switch link
    drawText_(g, reg ? L"已有账号？" : L"还没有账号？",
              cx + 32, by + 78, 130, 9.0f, fade(pal.text_muted));
    RectF link(cx + 130, by + 78, 200, 18);
    bool lhov = inRect(g_mouse, link);
    drawText_(g, reg ? L"去登录" : L"去注册",
              link.X, link.Y, link.Width, 9.0f,
              lhov ? fade(pal.primary_hover) : fade(pal.primary),
              StringAlignmentNear, FontStyleBold);
    hit(link, [](){
        g_auth_mode = (g_auth_mode == AuthMode::Login) ? AuthMode::Register : AuthMode::Login;
        g_auth.error_msg.clear();
        bool reg2 = (g_auth_mode == AuthMode::Register);
        ShowWindow(g_auth.edit_invite, reg2 ? SW_SHOW : SW_HIDE);
        // 重新动画入场
        g_auth_card_op.start(0.6f, 1.0f, 0.18f, 0.0f, curve::easeOutCubic);
    });

    // 服务器标记
    drawText_(g, L"服务器: 154.40.36.22:1337 · TLS",
              cx + 32, cy + ch - 36.0f, cw - 64, 8.0f,
              fade(pal.text_faint), StringAlignmentCenter);

    // Submit hit area
    if (!g_auth.busy) {
        hit(btn, [](){ PostMessageW(g_hwnd, WM_APP + 1, 0, 0); });
    }
}

// =====================================================================
// Stage 切换
// =====================================================================
void hideAuthControls() {
    if (g_auth.edit_username) ShowWindow(g_auth.edit_username, SW_HIDE);
    if (g_auth.edit_password) ShowWindow(g_auth.edit_password, SW_HIDE);
    if (g_auth.edit_invite)   ShowWindow(g_auth.edit_invite,   SW_HIDE);
}

void layoutAuthControls(int Wpx, int Hpx, bool register_mode);
void showAuthControls(bool register_mode);

void enterMainStage() {
    g_stage = Stage::Main;
    g_time_in_stage = 0.0f;
    hideAuthControls();
    g_sidebar_x.start(0, 1, 0.45f, 0.05f, curve::easeOutQuint);
    g_topbar_y.start(0, 1, 0.40f, 0.10f, curve::easeOutCubic);
    g_main_opacity.start(0, 1, 0.50f, 0.20f, curve::easeOutQuint);
}
void enterAuthStage() {
    g_stage = Stage::Auth;
    g_time_in_stage = 0.0f;
    g_auth_card_op.start(0, 1, 0.40f, 0.05f, curve::easeOutCubic);
    g_auth_card_y.start(16, 0, 0.45f, 0.05f, curve::easeOutQuint);
    showAuthControls(g_auth_mode == AuthMode::Register);
}
void enterExpandingStage() {
    g_stage = Stage::Expanding;
    g_time_in_stage = 0.0f;
    g_card_fade_out.start(0, 1, 0.30f, 0.0f, curve::easeOutCubic);
    g_window_w.start(200, 1100, 0.55f, 0.10f, curve::easeOutQuint);
    g_window_h.start(200, 720,  0.55f, 0.10f, curve::easeOutQuint);
}

// =====================================================================
// WndProc
// =====================================================================
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_NCHITTEST: {
            POINT p { LOWORD(lp), HIWORD(lp) }; ScreenToClient(hwnd, &p);
            // 顶栏左半区允许拖；overlay 打开时禁拖
            if (g_overlay != Overlay::None) return HTCLIENT;
            if (g_stage == Stage::Main && p.y < (int)kTopbarH && p.x < 1100 - 200 - 110) return HTCAPTION;
            return g_stage == Stage::Loading ? HTCAPTION : HTCLIENT;
        }
        case WM_MOUSEMOVE:
            g_mouse.x = LOWORD(lp); g_mouse.y = HIWORD(lp);
            InvalidateRect(hwnd, nullptr, FALSE);
            break;
        case WM_LBUTTONUP: {
            POINT p { LOWORD(lp), HIWORD(lp) };
            // 倒序遍历：后画的（dropdown / overlay）优先吃事件
            for (auto it = g_hits.rbegin(); it != g_hits.rend(); ++it) {
                if (inRect(p, it->rect)) {
                    if (it->on_click) it->on_click();
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
            }
            // 点击空白：关下拉
            if (g_account_dropdown) {
                g_account_dropdown = false;
                g_dropdown_t.start(g_dropdown_t.value(), 0, 0.15f, 0, curve::easeOutCubic);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            break;
        }
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) {
                if (g_overlay != Overlay::None) {
                    g_overlay_t.start(g_overlay_t.value(), 0, 0.18f, 0, curve::easeOutCubic);
                    g_overlay = Overlay::None;
                } else PostQuitMessage(0);
            }
            else if (wp == 'D' || wp == 'd') {
                g_dark = !g_dark;
                g_view_fade.start(0.6f, 1.0f, 0.25f, 0, curve::easeOutCubic);
            }
            else if (wp == 'H' || wp == 'h') {
                g_overlay = Overlay::History;
                g_overlay_t.start(0, 1, 0.25f, 0, curve::easeOutCubic);
            }
            else if (wp == 'S' || wp == 's') {
                if (g_stage != Stage::Main) {
                    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
                    SetWindowPos(hwnd, nullptr, (sw - 1100) / 2, (sh - 720) / 2, 1100, 720, SWP_NOZORDER);
                    enterMainStage();
                    g_main_opacity.elapsed = 999;
                    g_sidebar_x.elapsed = 999;
                    g_topbar_y.elapsed = 999;
                }
            }
            else if (wp >= '1' && wp <= '4') {
                static View vs[] = { View::Home, View::Lunching, View::Cloud, View::Settings };
                switchView(vs[wp - '1']);
            }
            else if (wp == 'P' || wp == 'p') {
                switchView(View::Profile);
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            break;
        case WM_PAINT: {
            PAINTSTRUCT ps; HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc; GetClientRect(hwnd, &rc);
            int Wpx = rc.right - rc.left, Hpx = rc.bottom - rc.top;
            HDC mem = CreateCompatibleDC(hdc);
            HBITMAP bmp = CreateCompatibleBitmap(hdc, Wpx, Hpx);
            HBITMAP old = (HBITMAP)SelectObject(mem, bmp);
            Graphics g(mem);
            g.SetSmoothingMode(SmoothingModeAntiAlias);
            g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
            g.SetCompositingQuality(CompositingQualityHighQuality);
            if (g_stage == Stage::Loading)        paintLoading(g, Wpx, Hpx);
            else if (g_stage == Stage::Expanding) paintLoading(g, Wpx, Hpx);
            else if (g_stage == Stage::Auth)      paintAuthView(g, Wpx, Hpx);
            else                                   paintMain(g, Wpx, Hpx);
            BitBlt(hdc, 0, 0, Wpx, Hpx, mem, 0, 0, SRCCOPY);
            SelectObject(mem, old); DeleteObject(bmp); DeleteDC(mem);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_SIZE:
            if (g_stage == Stage::Auth) {
                layoutAuthControls(LOWORD(lp), HIWORD(lp), g_auth_mode == AuthMode::Register);
            }
            break;
        case WM_APP + 1: {
            // Submit auth form: 取 EDIT 文字; 简化版: 不做真 HTTP, 假装成功后切到 Main
            wchar_t un[128] = {0}, pw[128] = {0}, iv[32] = {0};
            GetWindowTextW(g_auth.edit_username, un, 128);
            GetWindowTextW(g_auth.edit_password, pw, 128);
            GetWindowTextW(g_auth.edit_invite,   iv, 32);
            std::wstring user(un), pass(pw), invite(iv);
            g_auth.error_msg.clear();
            if (user.size() < 3) { g_auth.error_msg = L"用户名至少 3 字"; }
            else if (pass.size() < 8) { g_auth.error_msg = L"密码至少 8 字"; }
            else if (g_auth_mode == AuthMode::Register && invite.empty()) {
                g_auth.error_msg = L"注册需要邀请码";
            } else {
                g_auth.busy = true;
                InvalidateRect(hwnd, nullptr, FALSE);
                // Demo: 600ms 后假装成功, 切到 Main; 真工程在 Skia client 走 WinHTTP/libcurl
                SetTimer(hwnd, 0xA1, 600, nullptr);
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_TIMER:
            if (wp == 0xA1) {
                KillTimer(hwnd, 0xA1);
                g_auth.busy = false;
                enterMainStage();
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_CTLCOLOREDIT: {
            // EDIT 控件配色：暗色卡片底, 亮色文字
            HDC h = (HDC)wp;
            const Palette& pal = palette();
            SetTextColor(h, RGB(pal.text.GetR(), pal.text.GetG(), pal.text.GetB()));
            SetBkColor(h, RGB(pal.surface.GetR(), pal.surface.GetG(), pal.surface.GetB()));
            static HBRUSH br = nullptr;
            if (br) DeleteObject(br);
            br = CreateSolidBrush(RGB(pal.surface.GetR(), pal.surface.GetG(), pal.surface.GetB()));
            return (LRESULT)br;
        }
        case WM_RBUTTONUP:  PostQuitMessage(0); return 0;
        case WM_DESTROY:    PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

ULONG_PTR g_gdiplus_token = 0;

int APIENTRY wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR cmdline, int) {
    detectSystemLanguage();

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
    }
    bool overlayHistory = false, dropdownOpen = false;
    if (GetEnvironmentVariableW(L"LAUNCHER_OVERLAY", buf, 16) > 0) {
        if (buf[0] == L'h') overlayHistory = true;
        if (buf[0] == L'd') dropdownOpen = true;
    }
    bool skip_loading = wcsstr(cmdline, L"--main") != nullptr ||
        (GetEnvironmentVariableW(L"LAUNCHER_SKIP_LOADING", buf, 16) > 0 && buf[0] == L'1');

    GdiplusStartupInput gsi;
    GdiplusStartup(&g_gdiplus_token, &gsi, nullptr);

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"LauncherPreview";
    RegisterClassExW(&wc);

    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    int initW = skip_loading ? 1100 : 200;
    int initH = skip_loading ? 720  : 200;

    g_hwnd = CreateWindowExW(
        WS_EX_LAYERED | (skip_loading ? 0 : WS_EX_TOPMOST),
        wc.lpszClassName, L"Launcher",
        WS_POPUP,
        (sw - initW) / 2, (sh - initH) / 2, initW, initH,
        nullptr, nullptr, inst, nullptr);
    SetLayeredWindowAttributes(g_hwnd, 0, 255, LWA_ALPHA);
    DWM_WINDOW_CORNER_PREFERENCE pref = DWMWCP_ROUND;
    DwmSetWindowAttribute(g_hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &pref, sizeof(pref));
    createAuthControls(g_hwnd);     // 提前建好，需要时 ShowWindow
    ShowWindow(g_hwnd, SW_SHOW); UpdateWindow(g_hwnd);

    // skip_loading 走老路（直接 main，跳过 auth）方便截图
    bool skip_auth = (GetEnvironmentVariableW(L"LAUNCHER_SKIP_AUTH", buf, 16) > 0 && buf[0] == L'1');
    if (GetEnvironmentVariableW(L"LAUNCHER_AUTH_REGISTER", buf, 16) > 0 && buf[0] == L'1') {
        g_auth_mode = AuthMode::Register;
    }
    if (skip_loading && !skip_auth) {
        // 直接进 Auth 阶段，1100x720 已就位
        enterAuthStage();
        layoutAuthControls(1100, 720, g_auth_mode == AuthMode::Register);
    } else if (skip_loading) {
        enterMainStage();
        g_main_opacity.elapsed = 999;
        g_sidebar_x.elapsed = 999;
        g_topbar_y.elapsed = 999;
        if (overlayHistory) {
            g_overlay = Overlay::History;
            g_overlay_t.start(0, 1, 0.25f, 0, curve::easeOutCubic);
            g_overlay_t.elapsed = 999;
        }
        if (dropdownOpen) {
            g_account_dropdown = true;
            g_dropdown_t.start(0, 1, 0.18f, 0, curve::easeOutCubic);
            g_dropdown_t.elapsed = 999;
        }
    } else {
        g_card_scale.start(0.85f, 1.0f, 0.40f, 0.0f, curve::easeOutBack);
        g_card_opacity.start(0.0f, 1.0f, 0.30f, 0.0f, curve::easeOutCubic);
    }

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

        if (g_stage == Stage::Loading && g_time_in_stage > 1.6f) {
            enterExpandingStage();
        } else if (g_stage == Stage::Expanding) {
            int w = (int)g_window_w.value();
            int h = (int)g_window_h.value();
            int x = (sw - w) / 2, y = (sh - h) / 2;
            SetWindowPos(g_hwnd, nullptr, x, y, w, h, SWP_NOZORDER);
            if (g_window_w.done()) {
                enterAuthStage();
                layoutAuthControls(w, h, g_auth_mode == AuthMode::Register);
            }
        }

        InvalidateRect(g_hwnd, nullptr, FALSE);
        Sleep(8);
    }
end:
    GdiplusShutdown(g_gdiplus_token);
    return 0;
}
