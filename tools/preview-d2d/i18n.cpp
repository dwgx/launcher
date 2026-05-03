#include "i18n.h"
#include <cstring>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

namespace launcher::d2d {

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
        {"home.device",         "Device",           u8"设备",              u8"デバイス"},
        {"home.tags",           "My tags",          u8"我的标签",          u8"マイタグ"},
        {"home.add_tag",        "+ Add tag",        u8"+ 添加",            u8"+ 追加"},
        {"menu.home",           "Home",             u8"主页",              u8"ホーム"},
        {"menu.lunching",       "Launch",           u8"启动",              u8"起動"},
        {"menu.chat",           "Chat",             u8"聊天",              u8"チャット"},
        {"menu.market",         "Market",           u8"市场",              u8"マーケット"},
        {"menu.cloud",          "Cloud",            u8"云端",              u8"クラウド"},
        {"menu.settings",       "Settings",         u8"设置",              u8"設定"},
        {"acc.profile",         "Profile",          u8"个人资料",          u8"プロフィール"},
        {"acc.history",         "Login history",    u8"登录历史",          u8"ログイン履歴"},
        {"acc.password",        "Change password",  u8"修改密码",          u8"パスワード変更"},
        {"acc.signout",         "Sign out",         u8"退出登录",          u8"ログアウト"},
        {"settings.theme",      "Theme",            u8"主题",              u8"テーマ"},
        {"settings.theme_light","Light",            u8"亮",                u8"ライト"},
        {"settings.theme_dark", "Dark",             u8"暗",                u8"ダーク"},
        {"settings.language",   "Language",         u8"语言",              u8"言語"},
        {"settings.about",      "About",            u8"关于",              u8"について"},
        {"auth.login.title",    "Sign in",          u8"登录",              u8"サインイン"},
        {"auth.login.sub",      "Welcome back",     u8"欢迎回来",          u8"おかえり"},
        {"auth.register.title", "Sign up",          u8"注册",              u8"サインアップ"},
        {"auth.register.sub",   "Invite-only",      u8"邀请码注册",        u8"招待コード登録"},
        {"auth.username",       "Username",         u8"用户名",            u8"ユーザー名"},
        {"auth.password",       "Password",         u8"密码",              u8"パスワード"},
        {"auth.invite",         "Invite code",      u8"邀请码",            u8"招待コード"},
        {"auth.login",          "Sign in",          u8"登录",              u8"サインイン"},
        {"auth.register",       "Sign up",          u8"注册",              u8"登録"},
        {"auth.busy",           "Working...",       u8"处理中…",            u8"処理中…"},
        {"auth.to_login",       "Have account?",    u8"已经有账号？",      u8"既にアカウントがある？"},
        {"auth.to_register",    "No account?",      u8"还没账号？",        u8"アカウントがない？"},
        {"auth.go_login",       "Sign in",          u8"立即登录",          u8"今すぐログイン"},
        {"auth.go_register",    "Sign up",          u8"立即注册",          u8"今すぐ登録"},
        {"profile.title",       "Profile",          u8"个人资料",          u8"プロフィール"},
        {"profile.uid",         "UID",              u8"UID",               u8"UID"},
        {"profile.username",    "Username",         u8"用户名",            u8"ユーザー名"},
        {"profile.nickname",    "Nickname",         u8"昵称",              u8"ニックネーム"},
        {"profile.email",       "Email",            u8"邮箱",              u8"メール"},
        {"profile.expires",     "Subscription",     u8"订阅到期",          u8"購読期限"},
        {"profile.upload_avatar","Upload avatar",   u8"上传头像",          u8"アバター変更"},
        {"profile.change_pw",   "Change password",  u8"修改密码",          u8"パスワード変更"},
    };
    static const char* k_app_name = "Launcher";
    if (!key) return k_app_name;
    for (auto& e : T) {
        if (strcmp(e.k, key) == 0) {
            switch (g_lang) {
                case Lang::En:   return e.en;
                case Lang::ZhCN: return e.cn;
                case Lang::JaJP: return e.ja;
            }
        }
    }
    return key;
}

std::wstring trW(const char* key) {
    const char* s = tr(key);
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(n - 1, 0);
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), n);
    return w;
}

}  // namespace launcher::d2d
