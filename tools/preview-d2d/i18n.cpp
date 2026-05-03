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
        {"home.time",           "Now",              u8"当前时间",          u8"現在時刻"},
        {"home.host",           "Host",             u8"本机名",            u8"ホスト"},
        {"home.subscription",   "Subscription",     u8"订阅",              u8"購読"},
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
        {"acc.add_status",      "Add status...",    u8"添加状态消息…",     u8"ステータスを追加…"},
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
        {"auth.empty",          "Username/password required",
                                                    u8"用户名 / 密码不能为空",
                                                                          u8"ユーザー名 / パスワード必須"},
        {"auth.empty_invite",   "Invite code required",
                                                    u8"邀请码不能为空",    u8"招待コード必須"},
        {"profile.title",       "Profile",          u8"个人资料",          u8"プロフィール"},
        {"profile.uid",         "UID",              u8"UID",               u8"UID"},
        {"profile.username",    "Username",         u8"用户名",            u8"ユーザー名"},
        {"profile.nickname",    "Nickname",         u8"昵称",              u8"ニックネーム"},
        {"profile.email",       "Email",            u8"邮箱",              u8"メール"},
        {"profile.expires",     "Subscription",     u8"订阅到期",          u8"購読期限"},
        {"profile.bio",         "Bio",              u8"个人签名",          u8"自己紹介"},
        {"profile.bio_empty",   "No bio - click to edit", u8"还没设置签名 — 点击编辑",
                                                                          u8"自己紹介未設定 — クリックして編集"},
        {"profile.upload_avatar","Upload avatar",   u8"上传头像",          u8"アバター変更"},
        {"profile.change_pw",   "Change password",  u8"修改密码",          u8"パスワード変更"},
        {"chat.placeholder",    "Type something...",u8"写点什么…",         u8"何か書く…"},
        {"chat.empty",          "No messages yet. Say hi!",
                                                    u8"还没消息。说点什么吧～",
                                                                          u8"まだメッセージがない"},
        {"chat.market_redirect","Market channel - switch to Market tab",
                                                    u8"市场频道 — 切到 Market 标签查看商品",
                                                                          u8"マーケットチャネル"},
        {"chat.official",       "Official channel", u8"官方频道",          u8"公式チャネル"},
        {"chat.market_desc",    "Community trades - sell .cfg / sensitivity configs",
                                                    u8"社区交易市场（出售 .cfg / 灵敏度配置）",
                                                                          u8"コミュニティトレード"},
        {"picker.emoji",        "Emoji",            u8"表情",              u8"絵文字"},
        {"picker.packs",        "Packs",            u8"表情包",            u8"スタンプ"},
        {"picker.new",          "+ New",            u8"+ 新建",            u8"+ 新規"},
        {"picker.import",       "↥ Import",         u8"↥ 导入",            u8"↥ 取り込み"},
        {"picker.empty",        "Drop files / upload / install",
                                                    u8"还没贴纸 — 拖文件 / 上传 / 安装",
                                                                          u8"スタンプなし"},
        {"picker.rename",       "Rename",           u8"重命名",            u8"名前変更"},
        {"picker.share",        "Share",            u8"分享",              u8"共有"},
        {"picker.shared",       "Shared ✓",         u8"已分享 ✓",          u8"共有中 ✓"},
        {"picker.delete",       "Delete",           u8"删除",              u8"削除"},
        {"toast.copied_link",   "Share link copied",u8"分享链接已复制",    u8"共有リンクをコピー"},
        {"toast.imported",      "Imported {n}",     u8"已导入 {n} 张表情 ✓",  u8"{n} 個取り込み完了"},
        {"toast.over_limit",    "Quota reached (50/user)",
                                                    u8"已达上限（50/用户）",  u8"上限到達（50/ユーザー）"},
        {"market.title",        "Market",           u8"市场",              u8"マーケット"},
        {"market.sub",          "CS2 .cfg / configs / templates",
                                                    u8"CS2 .cfg / 配置 / 模板",
                                                                          u8"CS2 .cfg / 設定"},
        {"market.loading",      "Loading...",       u8"商品加载中…",       u8"読み込み中…"},
        {"common.cancel",       "Cancel",           u8"取消",              u8"キャンセル"},
        {"common.save",         "Save",             u8"保存",              u8"保存"},
        {"common.close",        "Close",            u8"关闭",              u8"閉じる"},
        {"common.confirm",      "Confirm",          u8"确定",              u8"確認"},
        {"common.loading",      "Loading...",       u8"加载中…",           u8"読み込み中…"},
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
