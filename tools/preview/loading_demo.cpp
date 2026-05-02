// Launcher 完整预览 demo (GDI+ 实现, 不依赖 Skia/Clay/vcpkg)
// 演示流程：
//   1. 200x200 加载小卡片，弧形 spinner + 入场缩放/淡入 (300ms)
//   2. 1.6s 后窗口扩张到 1100x720，加载卡片淡出
//   3. 主界面：左侧 200px 侧栏 + 顶部 48px + 主区
//      - HomeView：Hello {user} + 头像 + 订阅档位 + 设备 ID
//      - GameLibraryView：4×3 卡片网格
//      - SettingsView：语言切换 (en/zh-CN/ja-JP) + 主题切换
//      - CloudView：占位
//
// 入场动画层级（每个元素 stagger 50-200ms）：
//   背景 fade → 侧栏 slide-in 从左 → topbar 滑下 → 主区卡片错位浮入
//
// 这是预览，真实工程在 src/ 下用 Skia + Clay。视觉令牌跟 src/ui/theme + src/ui/anim 一致。
//
// 快捷键：
//   D  切换亮/暗
//   1-4  切换 Home/Library/Cloud/Settings
//   S  跳过加载直接进主界面（截屏用）
//   Esc/右键  退出

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
// Design tokens (镜像 src/ui/theme/color_tokens.h)
// =====================================================================
struct Palette {
    Color bg, surface, card, divider;
    Color primary, primary_hover;
    Color text, text_muted, text_faint;
    Color sidebar_bg, sidebar_active;
    Color shadow_card, shadow_card_hover;
};

const Palette kLight = {
    Color(255, 0xFA, 0xF7, 0xF2), Color(255, 0xF3, 0xEF, 0xE8),
    Color(255, 0xFF, 0xFF, 0xFF), Color(255, 0xED, 0xE9, 0xE1),
    Color(255, 0xC9, 0x64, 0x42), Color(255, 0xD9, 0x77, 0x57),
    Color(255, 0x1F, 0x1E, 0x1D), Color(255, 0x6B, 0x6A, 0x67),
    Color(255, 0xA8, 0xA3, 0x9A),
    Color(255, 0xF3, 0xEF, 0xE8), Color(255, 0xE9, 0xE1, 0xD3),
    Color( 14, 0, 0, 0),          Color( 30, 0, 0, 0)
};
const Palette kDark = {
    Color(255, 0x1A, 0x18, 0x16), Color(255, 0x20, 0x1E, 0x1B),
    Color(255, 0x24, 0x22, 0x20), Color(255, 0x36, 0x32, 0x2D),
    Color(255, 0xD9, 0x77, 0x57), Color(255, 0xE5, 0x86, 0x66),
    Color(255, 0xF5, 0xF1, 0xEA), Color(255, 0xA8, 0xA3, 0x9A),
    Color(255, 0x6B, 0x6A, 0x67),
    Color(255, 0x1E, 0x1B, 0x18), Color(255, 0x36, 0x30, 0x29),
    Color( 80, 0, 0, 0),          Color(140, 0, 0, 0)
};

bool g_dark = false;
const Palette& palette() { return g_dark ? kDark : kLight; }

// =====================================================================
// Curves (镜像 src/ui/anim/curve.h)
// =====================================================================
namespace curve {
inline float easeOutQuint(float t) { float i=1-t; return 1-i*i*i*i*i; }
inline float easeOutCubic(float t) { float i=1-t; return 1-i*i*i; }
inline float easeOutBack(float t)  {
    const float c1=1.70158f, c3=c1+1; float i=t-1;
    return 1 + c3*i*i*i + c1*i*i;
}
}

// =====================================================================
// I18N (内嵌; 真实工程用 src/ui/i18n + assets/i18n/*.json)
// =====================================================================
enum class Lang { En, ZhCN, JaJP };
Lang g_lang = Lang::ZhCN;

const char* tr(const char* key) {
    struct E { const char* k; const char* en; const char* cn; const char* ja; };
    static const E T[] = {
        {"app.name",            "Launcher",          "Launcher",            "Launcher"},
        {"app.tagline",         "Game launcher",    u8"游戏启动器",        u8"ゲームランチャー"},
        {"loading.connecting",  "Connecting...",    u8"连接中…",           u8"接続中…"},
        {"home.greet",          "Hello, dwgx",      u8"你好，dwgx",        u8"こんにちは、dwgx"},
        {"home.subtitle",       "Welcome back",     u8"欢迎回来",          u8"おかえりなさい"},
        {"home.tier",           "Plan",             u8"订阅档位",          u8"プラン"},
        {"home.expires",        "Expires",          u8"到期时间",          u8"期限"},
        {"home.device_id",      "Device ID",        u8"设备 ID",           u8"デバイス ID"},
        {"home.last_login",     "Last sign in",     u8"上次登录",          u8"前回のサインイン"},
        {"menu.home",           "Home",             u8"主页",              u8"ホーム"},
        {"menu.library",        "Library",          u8"游戏库",            u8"ライブラリ"},
        {"menu.cloud",          "Cloud",            u8"云端",              u8"クラウド"},
        {"menu.settings",       "Settings",         u8"设置",              u8"設定"},
        {"menu.logout",         "Sign out",         u8"退出登录",          u8"サインアウト"},
        {"library.launch",      "Launch",           u8"启动",              u8"起動"},
        {"settings.language",   "Language",         u8"语言",              u8"言語"},
        {"settings.theme",      "Theme",            u8"主题",              u8"テーマ"},
        {"settings.theme_light","Light",            u8"亮色",              u8"ライト"},
        {"settings.theme_dark", "Dark",             u8"暗色",              u8"ダーク"},
        {"settings.about",      "About",            u8"关于",              u8"情報"},
        {"cloud.title",         "Cloud",            u8"云端管理",          u8"クラウド"},
        {"cloud.no_data",       "Nothing synced",   u8"尚未同步",          u8"未同期"},
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
// Tween (简化版镜像 src/ui/anim/tween.h)
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
enum class Stage { Loading, Expanding, Main };
enum class View  { Home, Library, Cloud, Settings };

Stage g_stage = Stage::Loading;
View  g_view  = View::Home;

Tween g_card_scale, g_card_opacity, g_card_fade_out;
Tween g_window_w, g_window_h;
Tween g_sidebar_x, g_topbar_y, g_main_opacity;

float g_time_in_stage = 0.0f;
float g_spin_angle = 0.0f;
HWND  g_hwnd = nullptr;
POINT g_mouse{-1, -1};

struct UserInfo {
    const wchar_t* name = L"dwgx";
    const wchar_t* email = L"dwgx1337@outlook.com";
    const char*    tier_key = "tier.1week";
    const wchar_t* device_id = L"f8a1c2d4...e5b6 (TPM bound)";
    const wchar_t* expires = L"2026-05-09 14:32 UTC";
    const wchar_t* last_login = L"2026-05-02 10:32";
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

// =====================================================================
// Loading 阶段
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
// 主界面
// =====================================================================
const float kSidebarW = 200.0f;
const float kTopbarH  = 48.0f;

struct MenuEntry { View view; const char* key; const wchar_t* glyph; };
const MenuEntry kMenu[] = {
    { View::Home,     "menu.home",     L"◉" },
    { View::Library,  "menu.library",  L"▦" },
    { View::Cloud,    "menu.cloud",    L"☁" },
    { View::Settings, "menu.settings", L"⚙" },
};

void paintTopbar(Graphics& g, int Wpx) {
    const Palette& pal = palette();
    float ty = -kTopbarH * (1.0f - g_topbar_y.value());

    SolidBrush bg(pal.bg);
    g.FillRectangle(&bg, 0.0f, ty, (REAL)Wpx, kTopbarH);
    Pen sep(pal.divider, 1.0f);
    g.DrawLine(&sep, 0.0f, ty + kTopbarH, (REAL)Wpx, ty + kTopbarH);

    // 应用名 (左)
    drawText_(g, L"Launcher", 16.0f, ty + 14.0f, 200.0f, 11.0f,
              pal.text, StringAlignmentNear, FontStyleBold);
    drawText_(g, W(tr("app.tagline")).c_str(), 84.0f, ty + 18.0f, 200.0f, 8.0f,
              pal.text_faint);

    // 用户名 + 头像 (右)
    drawText_(g, g_user.name, (REAL)Wpx - 96.0f, ty + 16.0f, 50.0f,
              10.0f, pal.text_muted, StringAlignmentFar);
    float ar = 14.0f, ax = (REAL)Wpx - 16.0f - ar*2, ay = ty + (kTopbarH - ar*2)/2;
    SolidBrush avbg(pal.primary);
    g.FillEllipse(&avbg, ax, ay, ar*2, ar*2);
    Font af(L"Segoe UI", 10.0f, FontStyleBold, UnitPoint);
    SolidBrush avf(Color(255, 255, 255, 255));
    StringFormat fmt; fmt.SetAlignment(StringAlignmentCenter); fmt.SetLineAlignment(StringAlignmentCenter);
    RectF avrect(ax, ay, ar*2, ar*2);
    g.DrawString(L"D", -1, &af, avrect, &fmt, &avf);
}

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
        if (active) fillRR(g, item.X, item.Y, item.Width, item.Height, 8.0f, pal.sidebar_active);
        else if (hover) {
            Color hc(40, pal.text.GetR(), pal.text.GetG(), pal.text.GetB());
            fillRR(g, item.X, item.Y, item.Width, item.Height, 8.0f, hc);
        }
        Color tc = active ? pal.text : pal.text_muted;
        drawText_(g, m.glyph, item.X + 14.0f, item.Y + 11.0f, 16.0f,
                  11.0f, active ? pal.primary : tc);
        drawText_(g, W(tr(m.key)).c_str(), item.X + 38.0f, item.Y + 11.0f, item.Width - 50.0f,
                  9.5f, tc, StringAlignmentNear, active ? FontStyleBold : FontStyleRegular);
        View target = m.view;
        hit(item, [target]() { g_view = target; });
        my += 42.0f;
    }

    // 底部 logout
    RectF lo(sx + 12.0f, (REAL)Hpx - 56.0f, kSidebarW - 24.0f, 36.0f);
    drawText_(g, L"⏻", lo.X + 14.0f, lo.Y + 11.0f, 16.0f, 11.0f, pal.text_faint);
    drawText_(g, W(tr("menu.logout")).c_str(), lo.X + 38.0f, lo.Y + 11.0f, lo.Width - 50.0f,
              9.5f, pal.text_faint);
}

void paintHomeView(Graphics& g, RectF area) {
    const Palette& pal = palette();
    float op = g_main_opacity.value();
    auto fade = [&](Color c) { return Color((BYTE)(c.GetA() * op), c.GetR(), c.GetG(), c.GetB()); };

    float ty = area.Y + 32.0f + (1.0f - op) * 12.0f;
    drawText_(g, W(tr("home.greet")).c_str(), area.X + 36, ty, area.Width - 72,
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
    g.DrawString(L"D", -1, &af, avr, &fmt, &avF);

    drawText_(g, g_user.name, area.X + 36, cardY + 168, 280,
              16.0f, fade(pal.text), StringAlignmentCenter, FontStyleBold);
    drawText_(g, g_user.email, area.X + 36, cardY + 200, 280,
              8.5f, fade(pal.text_muted), StringAlignmentCenter);

    // 在线徽章
    float bx = area.X + 36 + 16, by = cardY + 240;
    SolidBrush green(Color((BYTE)(255 * op), 0x4C, 0xAF, 0x50));
    g.FillEllipse(&green, bx, by + 4.0f, 8.0f, 8.0f);
    drawText_(g, L"Online", bx + 16, by, 100, 8.5f, fade(pal.text_muted));

    // 详情卡片
    float dx = area.X + 36 + 280 + 24;
    float dw = area.Width - 36 - 280 - 24 - 36;
    drawShadow(g, dx, cardY, dw, 280, 16.0f, fade(pal.shadow_card), 2.0f, 3);
    fillRR(g, dx, cardY, dw, 280, 16.0f, fade(pal.card));

    struct Row { const char* label; const wchar_t* value; };
    Row rows[] = {
        { "home.tier",       W(tr("tier.1week")).c_str() },  // 注意：临时 wstring 生命周期问题，下面用 vector 缓存
    };
    // 重新用静态 wstring 数组规避临时对象生命周期问题
    std::wstring tier_w = W(tr("tier.1week"));
    struct Row2 { const char* label; const wchar_t* value; };
    Row2 r2[] = {
        { "home.tier",       tier_w.c_str()      },
        { "home.expires",    g_user.expires      },
        { "home.device_id",  g_user.device_id    },
        { "home.last_login", g_user.last_login   },
    };
    float ry = cardY + 28;
    for (int i = 0; i < 4; ++i) {
        drawText_(g, W(tr(r2[i].label)).c_str(), dx + 28, ry, 250,
                  8.5f, fade(pal.text_muted));
        drawText_(g, r2[i].value, dx + 28, ry + 22, dw - 56,
                  11.0f, fade(pal.text), StringAlignmentNear, FontStyleBold);
        if (i < 3) {
            Color sep((BYTE)(pal.divider.GetA() * op), pal.divider.GetR(), pal.divider.GetG(), pal.divider.GetB());
            Pen p(sep, 1.0f);
            g.DrawLine(&p, dx + 28.0f, ry + 56.0f, dx + dw - 28.0f, ry + 56.0f);
        }
        ry += 60.0f;
    }
}

void paintLibraryView(Graphics& g, RectF area) {
    const Palette& pal = palette();
    float op = g_main_opacity.value();
    auto fade = [&](Color c) { return Color((BYTE)(c.GetA() * op), c.GetR(), c.GetG(), c.GetB()); };

    drawText_(g, W(tr("menu.library")).c_str(), area.X + 36, area.Y + 32, 400,
              22.0f, fade(pal.text), StringAlignmentNear, FontStyleBold);

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
            float y = area.Y + 80 + row * (cardH + gap) + (1.0f - op) * (8 + idx * 2);
            bool hover = inRect(g_mouse, RectF(x, y, cardW, cardH));
            float lift = hover ? 2.0f : 0.0f;

            drawShadow(g, x, y - lift, cardW, cardH, 12.0f,
                       hover ? fade(pal.shadow_card_hover) : fade(pal.shadow_card),
                       hover ? 4.0f : 1.0f, hover ? 4 : 2);
            fillRR(g, x, y - lift, cardW, cardH, 12.0f, fade(pal.card));

            // 顶部 28px 主色条 + 圆角
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
                drawText_(g, W(tr("library.launch")).c_str(),
                          x + 14, y - lift + cardH - 26, cardW - 28,
                          9.5f, fade(pal.primary), StringAlignmentNear, FontStyleBold);
            }
        }
    }
}

void paintCloudView(Graphics& g, RectF area) {
    const Palette& pal = palette();
    float op = g_main_opacity.value();
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
    float op = g_main_opacity.value();
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
        if (!active) {
            GraphicsPath p; buildRoundRect(p, bx, sy, bw, bh, 8.0f);
            Pen pen(fade(pal.divider), 1.0f); g.DrawPath(&pen, &p);
        }
        drawText_(g, lb.label, bx, sy + 11, bw, 9.5f, fgc,
                  StringAlignmentCenter, active ? FontStyleBold : FontStyleRegular);
        Lang t = lb.l;
        hit(RectF(bx, sy, bw, bh), [t]() { g_lang = t; });
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
        if (!active) {
            GraphicsPath p; buildRoundRect(p, bx, sy, bw, bh, 8.0f);
            Pen pen(fade(pal.divider), 1.0f); g.DrawPath(&pen, &p);
        }
        drawText_(g, W(tr(tb.labelKey)).c_str(), bx, sy + 11, bw, 9.5f, fgc,
                  StringAlignmentCenter, active ? FontStyleBold : FontStyleRegular);
        bool t = tb.dark;
        hit(RectF(bx, sy, bw, bh), [t]() { g_dark = t; });
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
        case View::Library:  paintLibraryView(g, area);  break;
        case View::Cloud:    paintCloudView(g, area);    break;
        case View::Settings: paintSettingsView(g, area); break;
    }
}

// =====================================================================
// 阶段切换
// =====================================================================
void enterMainStage() {
    g_stage = Stage::Main;
    g_time_in_stage = 0.0f;
    g_sidebar_x.start(0, 1, 0.45f, 0.05f, curve::easeOutQuint);
    g_topbar_y.start(0, 1, 0.40f, 0.10f, curve::easeOutCubic);
    g_main_opacity.start(0, 1, 0.50f, 0.20f, curve::easeOutQuint);
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
            if (g_stage == Stage::Main && p.y < (int)kTopbarH && p.x < 1100 - 200) return HTCAPTION;
            return g_stage == Stage::Loading ? HTCAPTION : HTCLIENT;
        }
        case WM_MOUSEMOVE:
            g_mouse.x = LOWORD(lp); g_mouse.y = HIWORD(lp);
            InvalidateRect(hwnd, nullptr, FALSE);
            break;
        case WM_LBUTTONUP: {
            POINT p { LOWORD(lp), HIWORD(lp) };
            for (auto& h : g_hits) {
                if (inRect(p, h.rect)) { if (h.on_click) h.on_click(); InvalidateRect(hwnd, nullptr, FALSE); break; }
            }
            break;
        }
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) PostQuitMessage(0);
            else if (wp == 'D' || wp == 'd') { g_dark = !g_dark; InvalidateRect(hwnd, nullptr, FALSE); }
            else if (wp == 'S' || wp == 's') {
                if (g_stage != Stage::Main) {
                    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
                    SetWindowPos(hwnd, nullptr, (sw - 1100) / 2, (sh - 720) / 2, 1100, 720, SWP_NOZORDER);
                    enterMainStage();
                    g_main_opacity.elapsed = 999;
                    g_sidebar_x.elapsed = 999;
                    g_topbar_y.elapsed = 999;
                }
            } else if (wp >= '1' && wp <= '4') {
                static View vs[] = { View::Home, View::Library, View::Cloud, View::Settings };
                g_view = vs[wp - '1'];
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
            else                                   paintMain(g, Wpx, Hpx);
            BitBlt(hdc, 0, 0, Wpx, Hpx, mem, 0, 0, SRCCOPY);
            SelectObject(mem, old); DeleteObject(bmp); DeleteDC(mem);
            EndPaint(hwnd, &ps);
            return 0;
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
        else if (buf[0] == L'l') g_view = View::Library;
        else if (buf[0] == L'c') g_view = View::Cloud;
        else if (buf[0] == L's') g_view = View::Settings;
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
    ShowWindow(g_hwnd, SW_SHOW); UpdateWindow(g_hwnd);

    if (skip_loading) {
        enterMainStage();
        g_main_opacity.elapsed = 999;
        g_sidebar_x.elapsed = 999;
        g_topbar_y.elapsed = 999;
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

        if (g_stage == Stage::Loading && g_time_in_stage > 1.6f) {
            enterExpandingStage();
        } else if (g_stage == Stage::Expanding) {
            int w = (int)g_window_w.value();
            int h = (int)g_window_h.value();
            int x = (sw - w) / 2, y = (sh - h) / 2;
            SetWindowPos(g_hwnd, nullptr, x, y, w, h, SWP_NOZORDER);
            if (g_window_w.done()) enterMainStage();
        }

        InvalidateRect(g_hwnd, nullptr, FALSE);
        Sleep(8);
    }
end:
    GdiplusShutdown(g_gdiplus_token);
    return 0;
}
