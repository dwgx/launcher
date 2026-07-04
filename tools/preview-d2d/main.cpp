// Launcher D2D Preview — Phase 2.1 主入口。
// 业务全部接通 GDI+ Preview 同款功能：autologin / WS / 托盘 / 拖文件 / Toast /
// 真后端登录-退出-改密 / status 同步 / login history / tags 增删 / 头像云同步。

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <windowsx.h>
#include <ShlObj.h>
#include <chrono>
#include <memory>
#include <string>

#pragma comment(lib, "shell32.lib")

#include "d2d_app.h"
#include "stages.h"
#include "palette.h"
#include "auth.h"
#include "hit.h"
#include "ui_main.h"
#include "chat.h"
#include "modals.h"
#include "fetch.h"
#include "persist.h"
#include "user_state.h"
#include "toast.h"
#include "ws_user.h"
#include "tray.h"
#include "i18n.h"
#include "steam.h"
#include "hwid.h"
#include "sticker.h"
#include "net.h"
#include "download_pool.h"
#include <memory>
#include <utility>

#ifdef LAUNCHER_VISUAL_SMOKE
#include "visual_smoke.h"
#endif

#pragma comment(lib, "user32.lib")

using namespace launcher::d2d;

static D2DApp g_app;

namespace {
constexpr UINT kMsgAutoLoginResult = WM_APP + 56;

static std::string envStringA(const char* name) {
    char buf[4096]{};
    DWORD n = GetEnvironmentVariableA(name, buf, (DWORD)_countof(buf));
    if (n == 0 || n >= _countof(buf)) return {};
    return std::string(buf, n);
}

// 单实例锁。产品不允许多开：第二个实例检测到锁后弹窗提醒，前置已有窗口并退出。
// 测试期可设环境变量 LAUNCHER_ALLOW_MULTI=1 绕过（仅开发用，发布版不要设）。
// 返回值契约：nullptr = 已有实例在跑、本进程应退出；其它（含 INVALID_HANDLE_VALUE
// 与有效 HANDLE）= 应继续运行。持有的 HANDLE 留到进程退出由 OS 回收。
static HANDLE acquireSingleInstance() {
    if (!envStringA("LAUNCHER_ALLOW_MULTI").empty()) return INVALID_HANDLE_VALUE;  // 显式放行多开
    // Local\ 前缀：锁限定在当前登录会话内，不跨用户/远程会话，符合“同一用户不重复开”的语义
    HANDLE m = CreateMutexW(nullptr, FALSE, L"Local\\LauncherD2D.SingleInstance.v1");
    if (!m) return INVALID_HANDLE_VALUE;  // 创建失败：宁可放行也不误锁死用户
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        // 已有实例：先把它的窗口拉到前台，再弹窗告知用户已在运行
        HWND prev = FindWindowW(L"LauncherD2DPreview", nullptr);
        if (prev) {
            if (IsIconic(prev)) ShowWindow(prev, SW_RESTORE);
            SetForegroundWindow(prev);
        }
        // 弹窗文案本地化：守卫早于 g_lang 加载，这里独立取语言（用户偏好优先，回退系统语言）
        Lang lang = (Lang)persist::loadLang((int)detectSystemLang());
        const wchar_t* title;
        const wchar_t* body;
        switch (lang) {
            case Lang::ZhCN:
                title = L"Launcher 已在运行";
                body  = L"Launcher 已经打开了，不能重复启动。\n已为你切换到正在运行的窗口。";
                break;
            case Lang::JaJP:
                title = L"Launcher は既に起動しています";
                body  = L"Launcher はすでに開いています。多重起動はできません。\n実行中のウィンドウに切り替えました。";
                break;
            default:
                title = L"Launcher is already running";
                body  = L"Launcher is already open and cannot be launched twice.\nSwitched you to the running window.";
                break;
        }
        MessageBoxW(prev, body, title, MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
        CloseHandle(m);
        return nullptr;
    }
    return m;  // 首个实例，拿到锁
}

static std::wstring utf8ToWMain(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(n - 1, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

static void applyEnvIdentity() {
    std::string uid = envStringA("LAUNCHER_UID");
    std::string username = envStringA("LAUNCHER_USERNAME");
    std::string nickname = envStringA("LAUNCHER_NICKNAME");
    if (!uid.empty()) g_user.uid = utf8ToWMain(uid);
    if (!username.empty()) g_user.username = utf8ToWMain(username);
    if (!nickname.empty()) g_user.nickname = utf8ToWMain(nickname);
    else if (!username.empty()) g_user.nickname = g_user.username;
}
}

static POINT physToDip(POINT phys) {
    float scale = g_app.dpi() / 96.0f;
    if (scale < 0.001f) scale = 1.0f;
    return { (LONG)(phys.x / scale + 0.5f), (LONG)(phys.y / scale + 0.5f) };
}

static bool inAuthOrMain() {
    return stages::g_stage == stages::Stage::Auth
        || stages::g_stage == stages::Stage::Main;
}

// 启动后异步：tags + 头像云同步 + sticker packs/stickers + market + 个人 profile
static void afterLogin(HWND hwnd) {
    persist::saveSession(g_session_token, g_user_id);
    ws::start(hwnd);
    chat::fetchOfficialChannels(hwnd);
    fetch::myProfile(hwnd);          // ← status / status_text / bio 持久化拉回
    fetch::userTags(hwnd);
    fetch::remoteAvatar(hwnd);
    sticker::fetchMyPacks(hwnd);
    sticker::fetchMyStickers(hwnd);
    fetch::marketListings(hwnd);
}

static void validateSavedSession(HWND hwnd, const std::string& token) {
    struct A { HWND h; std::string tok; };
    auto* a = new A{ hwnd, token };
    CreateThread(nullptr, 0, [](LPVOID lp) -> DWORD {
        std::unique_ptr<A> a((A*)lp);
        std::string body = "{\"session_token\":\"" + a->tok
            + "\",\"hwid_hex\":\"" + hwidHex() + "\"}";
        auto r = net::postJson(L"/api/heartbeat", body);
        PostMessageW(a->h, kMsgAutoLoginResult, r.ok() ? 1 : 0, (LPARAM)r.status);
        return 0;
    }, a, 0, nullptr);
}

static void leaveInvalidSession(HWND hwnd, bool clear_persisted, bool show_toast) {
    ws::stop();
    if (clear_persisted) persist::clearSession();
    g_session_token.clear();
    g_user_id.clear();
    g_avatar_path.clear();
    g_app.images().invalidate();
    stages::g_skip_auth_after_loading = false;
    stages::g_auth_validation_pending = false;
    if (stages::g_stage == stages::Stage::Main
        || stages::g_stage == stages::Stage::ExpandMain) {
        stages::enterAuthFromLogout();
    }
    if (show_toast) {
        toast::show(clear_persisted
            ? trW("session.expired")
            : trW("session.cannot_verify"));
    }
    InvalidateRect(hwnd, nullptr, FALSE);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_SIZE: {
            int w = LOWORD(lp), h = HIWORD(lp);
            if (w > 0 && h > 0) g_app.requestResize(w, h);
            return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: { PAINTSTRUCT ps; BeginPaint(hwnd, &ps); EndPaint(hwnd, &ps); return 0; }
        case WM_NCHITTEST: {
            if (!inAuthOrMain()) return HTCAPTION;
            POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            ScreenToClient(hwnd, &pt);
            POINT dip = physToDip(pt);
            if (stages::g_stage == stages::Stage::Main
                && stages::g_view == stages::View::Chat) {
                return HTCLIENT;
            }
            return anyHover(dip) ? HTCLIENT : HTCAPTION;
        }
        case WM_MOUSEMOVE: {
            POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            g_mouse = physToDip(pt);
            if (stages::g_stage == stages::Stage::Main
                && stages::g_view == stages::View::Chat
                && chat::onMouseMove(hwnd, g_mouse)) {
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_LBUTTONDOWN: {
            POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            g_mouse = physToDip(pt);
            g_mouse_pressed = true;
            if (modal::onMouseLDown(hwnd, g_mouse)) return 0;
            if (stages::g_stage == stages::Stage::Main
                && stages::g_view == stages::View::Chat) {
                // chat 自己的 onMouseLDown 处理 picker dismiss / pack drag /
                // composer 焦点 — 别只走 dispatchClick 否则这些都不生效
                chat::onMouseLDown(hwnd, g_mouse);
            } else if (inAuthOrMain()) {
                dispatchClick(g_mouse);
                // 不在 chat 但仍想要"点其他地方取消聚焦"行为 — 这里通用清空
                // (auth view 自己的 input box 通过 hit cb 重设 focus，所以无须额外)
            }
            return 0;
        }
        case WM_LBUTTONUP: {
            g_mouse_pressed = false;
            POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            POINT dip = physToDip(pt);
            if (stages::g_stage == stages::Stage::Main
                && stages::g_view == stages::View::Chat) {
                chat::onMouseLUp(hwnd, dip);
            }
            return 0;
        }
        case WM_MOUSEWHEEL: {
            if (stages::g_stage == stages::Stage::Main
                && stages::g_view == stages::View::Chat
                && !modal::anyOpen()) {
                // picker 打开时滚 emoji grid，关闭时滚 chat 流 — chat::onWheel 自己分流
                int delta = GET_WHEEL_DELTA_WPARAM(wp);
                chat::onWheel(delta);
            }
            return 0;
        }
        case WM_KEYDOWN: {
            bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            bool ctrl  = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            if (wp == VK_ESCAPE) {
                if (modal::onKey(hwnd, (int)wp, shift, ctrl)) return 0;
                if (ui::g_account_dropdown) {
                    ui::g_account_dropdown = false;
                    ui::g_status_fold_open = false;
                    ui::g_dropdown_t.start(ui::g_dropdown_t.value(), 0.0f, 0.15f,
                                            0, curve::easeOutCubic);
                    ui::g_status_fold_t.start(ui::g_status_fold_t.value(), 0.0f, 0.15f,
                                               0, curve::easeOutCubic);
                    return 0;
                }
                if (chat::g_picker_open) {
                    chat::g_picker_open = false;
                    chat::g_picker_t.start(chat::g_picker_t.value(), 0, 0.12f,
                                           0, curve::easeOutCubic);
                    return 0;
                }
                if (stages::g_stage == stages::Stage::Main) {
                    // ESC 最小化到托盘，不真退出
                    tray::hideToTray(hwnd);
                    return 0;
                }
                PostQuitMessage(0);
                return 0;
            }
            if (modal::onKey(hwnd, (int)wp, shift, ctrl)) return 0;
            if (stages::g_stage == stages::Stage::Auth) {
                auth::onKey(hwnd, (int)wp, shift, ctrl);
                return 0;
            }
            if (stages::g_stage == stages::Stage::Main
                && stages::g_view == stages::View::Chat
                && chat::g_focus_composer) {
                chat::onKey(hwnd, (int)wp, shift, ctrl);
                return 0;
            }
            if (wp == 'F' && ctrl
                && stages::g_stage == stages::Stage::Main) {
                modal::openSearch();
                return 0;
            }
            if (wp == 'D') { g_dark = !g_dark; persist::saveTheme(g_dark); return 0; }
            return 0;
        }
        case WM_CHAR: {
            bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            if (modal::onChar(hwnd, (wchar_t)wp, ctrl)) return 0;
            if (stages::g_stage == stages::Stage::Auth) {
                auth::onChar(hwnd, (wchar_t)wp, ctrl);
                return 0;
            }
            if (stages::g_stage == stages::Stage::Main
                && stages::g_view == stages::View::Chat
                && chat::g_focus_composer) {
                chat::onChar(hwnd, (wchar_t)wp, ctrl);
                return 0;
            }
            return 0;
        }
        case WM_DROPFILES: {
            if (stages::g_stage == stages::Stage::Main
                && stages::g_view == stages::View::Chat) {
                HDROP drop = (HDROP)wp;
                UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
                for (UINT i = 0; i < n; ++i) {
                    wchar_t path[MAX_PATH];
                    if (DragQueryFileW(drop, i, path, MAX_PATH)) {
                        chat::appendMedia(path);
                    }
                }
                DragFinish(drop);
            }
            return 0;
        }
        case WM_APP + 1: {                     // Auth submit
            auth::handleSubmit(hwnd);
            return 0;
        }
        case WM_APP + 2: {                     // Auth submit result
            auth::onSubmitResult(hwnd, wp != 0);
            if (wp != 0) afterLogin(hwnd);
            return 0;
        }
        case WM_APP + 3: {                     // 头像上传结果
            std::unique_ptr<std::wstring> p((std::wstring*)lp);
            if (wp && p && !p->empty()) {
                g_avatar_path = *p;
                g_app.images().evict(g_avatar_path);   // 仅逐出该头像，不再全清缓存
                toast::show(trW("toast.avatar_synced"));
            } else {
                toast::show(trW("toast.avatar_upload_fail"));
            }
            return 0;
        }
        case WM_APP + 4: {                     // ChangePw result
            modal::onChangePwResult(wp != 0);
            if (wp) toast::show(trW("toast.pw_changed"));
            return 0;
        }
        case WM_APP + 5: {                     // Chat official channels result
            chat::applyOfficialResult();
            // 拉到 chat_id 后立即 fetchHistory 当前频道
            chat::switchChannel(chat::g_active);
            return 0;
        }
        case WM_APP + 10: {                    // WS message arrived / Wave2 媒体下载完成
            ws::drain();
            InvalidateRect(hwnd, nullptr, FALSE);  // Wave2: 历史图后台下载完 → 重绘让解码缓存接手
            return 0;
        }
        case launcher::d2d::kMsgDecodeReady: {  // 后台解码完成 → UI 线程上传+翻 Ready+重绘
            g_app.images().drainCompleted();
            g_app.gifs().drainCompleted();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_APP + 15: {                    // user tags fetched
            return 0;
        }
        case WM_APP + 16: {                    // tag add result
            modal::onAddTagResult(wp != 0, (int)lp);
            return 0;
        }
        case WM_APP + 17: {                    // tag remove result
            fetch::userTags(hwnd);     // 重新拉
            return 0;
        }
        case WM_APP + 19: {                    // pack create result
            modal::onCreatePackResult(wp != 0);
            if (wp) {
                toast::show(trW("toast.pack_created"));
                // 立即重拉 packs 列表，新 pack 出现在 picker（不用重启客户端）
                sticker::fetchMyPacks(hwnd);
            }
            return 0;
        }
        case WM_APP + 20: {                    // pack rename result
            modal::onRenamePackResult(wp != 0);
            if (wp) toast::show(trW("toast.renamed"));
            return 0;
        }
        case WM_APP + 21: {                    // chat picker → 新建表情包
            modal::openCreatePack();
            return 0;
        }
        case WM_APP + 23: {                    // 远程头像下载完成；lp = std::wstring*
            std::unique_ptr<std::wstring> p((std::wstring*)lp);
            if (!wp) {
                if (!g_avatar_path.empty()) g_app.images().evict(g_avatar_path);
                g_avatar_path.clear();
                return 0;
            }
            if (wp && p && !p->empty()) {
                g_avatar_path = *p;
                g_app.images().evict(g_avatar_path);   // 仅逐出该头像，强制重新解码
                toast::show(trW("toast.avatar_cloud_synced"));
            } else {
                if (!g_avatar_path.empty()) g_app.images().evict(g_avatar_path);
                g_avatar_path.clear();
            }
            return 0;
        }
        case WM_APP + 22: {                    // sticker packs fetched
            // 拉完检查 / 自动创建 "我的表情" pack（如果 backend 没有）
            sticker::ensureMyStickersPack(hwnd);
            return 0;
        }
        case WM_APP + 24: {                    // my stickers fetched
            return 0;
        }
        case WM_APP + 25: {                    // pack contents fetched
            return 0;
        }
        case WM_APP + 26: {                    // pack share result; lp = std::string* short_name (if pub)
            std::unique_ptr<std::string> sn((std::string*)lp);
            if (wp) {
                if (sn) {
                    std::wstring link = L"launcher://pack/";
                    for (char c : *sn) link.push_back((wchar_t)c);
                    if (OpenClipboard(hwnd)) {
                        EmptyClipboard();
                        size_t bytes = (link.size() + 1) * sizeof(wchar_t);
                        HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
                        if (h) {
                            memcpy(GlobalLock(h), link.c_str(), bytes);
                            GlobalUnlock(h);
                            SetClipboardData(CF_UNICODETEXT, h);
                        }
                        CloseClipboard();
                    }
                    toast::show(trW("toast.shared_done"));
                } else {
                    toast::show(trW("toast.share_canceled"));
                }
                // 不重拉 packs（避免 active tab 索引错乱）— sharePack 内部已写回 short_name
            } else toast::show(trW("toast.share_fail"));
            return 0;
        }
        case WM_APP + 27: {                    // pack delete result
            if (wp) {
                toast::show(trW("toast.pack_deleted"));
                sticker::fetchMyPacks(hwnd);    // 立即刷新 picker 让 pack 消失
            } else toast::show(trW("toast.delete_fail"));
            return 0;
        }
        case WM_APP + 28: {                    // pack install result
            if (wp) {
                toast::show(trW("toast.pack_installed"));
                sticker::fetchMyPacks(hwnd);
            } else toast::show(trW("toast.install_fail"));
            return 0;
        }
        case WM_APP + 30: {                    // login history result; lp = std::string*
            std::unique_ptr<std::string> p((std::string*)lp);
            if (p) modal::onHistoryResult(*p);
            return 0;
        }
        case WM_APP + 31: {                    // chat picker → rename pack
            std::unique_ptr<chat::PackActionPayload> payload((chat::PackActionPayload*)wp);
            if (payload) modal::openRenamePack(payload->id, payload->name);
            return 0;
        }
        case WM_APP + 32: {                    // chat picker → delete pack confirm
            std::unique_ptr<chat::PackActionPayload> payload((chat::PackActionPayload*)wp);
            if (payload) {
                std::string id_copy = payload->id;
                std::wstring nm_copy = payload->name;
                std::wstring msg = trW("confirm.delete_pack_msg");
                auto p = msg.find(L"{name}");
                if (p != std::wstring::npos) msg.replace(p, 6, nm_copy);
                modal::openConfirm(trW("confirm.delete_pack_title"),
                    msg,
                    [id_copy](){
                        sticker::deletePack(GetActiveWindow(), id_copy);
                    },
                    trW("picker.delete"), trW("common.cancel"), true);
            }
            return 0;
        }
        case WM_APP + 33: {                    // market listings fetched
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_APP + 29: {                    // sticker import 完成；wp=成功数 lp=pack_id
            std::unique_ptr<std::string> pid((std::string*)lp);
            if (wp > 0) {
                std::wstring msg = trW("toast.imported");
                auto p = msg.find(L"{n}");
                if (p != std::wstring::npos) msg.replace(p, 3, std::to_wstring((int)wp));
                toast::show(msg);
                // 切 picker active tab 到这个 pack（让用户立刻看到导入的图）
                if (pid) {
                    int idx = -1;
                    for (size_t i = 0; i < sticker::g_packs.size(); ++i) {
                        if (sticker::g_packs[i].id == *pid) { idx = (int)i; break; }
                    }
                    if (idx >= 0) {
                        chat::g_picker_tab = 1 + idx;     // 0 = emoji, 1+ = pack idx
                        if (!chat::g_picker_open) {
                            chat::g_picker_open = true;
                            chat::g_picker_t.start(chat::g_picker_t.value(), 1, 0.16f, 0, curve::easeOutCubic);
                        }
                    }
                }
            } else {
                toast::show(trW("toast.no_files_uploaded"));
            }
            return 0;
        }
        case WM_APP + 34: {                    // chat picker → 弹文件夹对话框 + 上传
            std::unique_ptr<std::string> pid((std::string*)wp);
            if (pid && !pid->empty()) {
                int total = sticker::totalUserStickers();
                if (total >= 50) {
                    toast::show(trW("toast.over_limit_import"));
                    return 0;
                }
                sticker::importFromFolderUi(hwnd, *pid);
            }
            return 0;
        }
        case WM_APP + 38: {                    // geoIP fetched → 重 paint Home 显示国家
            return 0;
        }
        case WM_APP + 39: {                    // sticker delete result
            // 本地已经移除，后端是否成功不影响 UI
            return 0;
        }
        case WM_APP + 40: {                    // chat picker → 删单个 sticker (lp = std::wstring* path)
            std::unique_ptr<std::wstring> p((std::wstring*)wp);
            if (p) sticker::deleteSticker(hwnd, *p);
            return 0;
        }
        case WM_APP + 41: {                    // chat picker → 我的表情想导入
            toast::show(trW("toast.pick_group_first"));
            return 0;
        }
        case WM_APP + 45: {                    // chat history fetched (lp = std::wstring* slug)
            std::unique_ptr<std::wstring> p((std::wstring*)lp);
            if (p) chat::applyHistoryResult(*p);
            return 0;
        }
        case WM_APP + 46: {                    // chat video bubble 点击 (wp = std::wstring* path)
            std::unique_ptr<std::wstring> p((std::wstring*)wp);
            if (p && !p->empty()) modal::openVideoPlayer(*p);
            return 0;
        }
        case WM_APP + 47: {                    // chat text 链接点击 (wp = std::wstring* url)
            std::unique_ptr<std::wstring> p((std::wstring*)wp);
            if (p && !p->empty()) modal::openWebPage(*p);
            return 0;
        }
        case WM_APP + 43: {                    // sticker 导出完成 (wp = success count)
            if (wp > 0) {
                std::wstring msg = trW("toast.exported_n");
                auto p = msg.find(L"{n}");
                if (p != std::wstring::npos) msg.replace(p, 3, std::to_wstring((int)wp));
                toast::show(msg);
            } else {
                toast::show(trW("toast.export_fail"));
            }
            return 0;
        }
        case WM_APP + 44: {                    // pack preview 加载完
            return 0;
        }
        case WM_APP + 48: {                    // 拖拽排序云端写入完成
            if (wp == 0) toast::show(trW("toast.reorder_fail"));
            return 0;
        }
        case WM_APP + 49: {                    // chat 链接卡片点击 → 弹 PackPreview (wp = std::string* short)
            std::unique_ptr<std::string> p((std::string*)wp);
            if (p && !p->empty()) modal::openPackPreviewModal(*p);
            return 0;
        }
        case WM_APP + 50: {                    // 消息右键菜单 (wp = struct{POINT, idx}*)
            std::unique_ptr<chat::MsgContextPayload> p((chat::MsgContextPayload*)wp);
            if (p) modal::openMsgContextMenu(p->pt, p->idx);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_APP + 52: {                    // 发消息后，把 server message_id 绑到本地 me 消息
            chat::applySendResult();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_APP + 53: {                    // 删消息结果（仅自己消息）
            if (wp == 0) toast::show(launcher::d2d::trW("toast.delete_fail"));
            return 0;
        }
        case WM_APP + 54: {                    // myProfile 拉回
            if (wp) {
                fetch::applyMyProfileResult();
            } else if ((DWORD)lp == 401 || (DWORD)lp == 403) {
                leaveInvalidSession(hwnd, true, true);
            }
            return 0;
        }
        case WM_APP + 55: {                    // chat search 结果回来
            modal::drainSearchResult();
            return 0;
        }
        case WM_APP + 57: {
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_APP + 58: {
            modal::onMuteUserResult(wp != 0);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_APP + 59: {
            modal::onUnmuteUserResult(wp != 0);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_APP + 60: {                    // 每日签到结果
            fetch::g_checkin_inflight = false;
            if (wp) {
                fetch::CheckinResult res;
                {
                    std::lock_guard<std::mutex> lk(fetch::g_checkin_mtx);
                    res = fetch::g_checkin;
                }
                if (res.level > 0) g_user.level = res.level;
                toast::show(res.granted ? trW("home.checkin_reward")
                                        : trW("home.checkin_done"));
                InvalidateRect(hwnd, nullptr, FALSE);
            } else if ((DWORD)lp == 401 || (DWORD)lp == 403) {
                leaveInvalidSession(hwnd, true, true);
            }
            return 0;
        }
        case WM_APP + 51: {                    // chat picker → 卸载非 owner pack (wp/lp 同 +32)
            std::unique_ptr<chat::PackActionPayload> payload((chat::PackActionPayload*)wp);
            if (payload) {
                std::string id_copy = payload->id;
                std::wstring nm_copy = payload->name;
                std::wstring msg = trW("confirm.uninstall_pack_msg");
                auto p = msg.find(L"{name}");
                if (p != std::wstring::npos) msg.replace(p, 6, nm_copy);
                modal::openConfirm(trW("confirm.uninstall_pack_title"),
                    msg,
                    [id_copy](){
                        // 卸载（uninstall）
                        struct A { std::string id; HWND h; };
                        auto* a = new A{ id_copy, GetActiveWindow() };
                        CreateThread(nullptr, 0, [](LPVOID lp) -> DWORD {
                            std::unique_ptr<A> a((A*)lp);
                            std::string body = "{\"session_token\":\"" + g_session_token
                                             + "\",\"pack_id\":\"" + a->id + "\"}";
                            net::postJson(L"/api/sticker/pack/uninstall", body);
                            // 本地从 g_packs 摘掉
                            {
                                std::lock_guard<std::mutex> lk(sticker::g_packs_mtx);
                                auto& v = sticker::g_packs;
                                for (auto it = v.begin(); it != v.end(); ++it) {
                                    if (it->id == a->id) { v.erase(it); break; }
                                }
                            }
                            PostMessageW(a->h, WM_APP + 27, 1, 0);
                            return 0;
                        }, a, 0, nullptr);
                    },
                    trW("picker.uninstall"), trW("common.cancel"), true);
            }
            return 0;
        }
        case kMsgAutoLoginResult: {
            stages::g_auth_validation_pending = false;
            if (wp) {
                stages::g_skip_auth_after_loading = true;
                if (stages::g_stage == stages::Stage::Loading) {
                    stages::g_time_in_stage = 1.5f;
                }
                afterLogin(hwnd);
            } else {
                DWORD status = (DWORD)lp;
                leaveInvalidSession(hwnd, status == 401 || status == 403, true);
            }
            return 0;
        }
        case WM_APP + 35: {                    // profile update result
            bool ok = wp != 0;
            auto kind = static_cast<fetch::ProfileUpdateKind>((int)lp);
            if (kind == fetch::ProfileUpdateKind::StatusText) {
                modal::onEditStatusTextResult(ok);
            } else if (kind == fetch::ProfileUpdateKind::Bio) {
                modal::onEditBioResult(ok);
            } else {
                modal::onEditStatusTextResult(ok);
                modal::onEditBioResult(ok);
            }
            if (wp) toast::show(trW("toast.saved"));
            return 0;
        }
        case WM_APP + 61: {                    // change_nickname result; wp = HTTP 状态码
            // onEditNicknameResult 自行按状态码分派 toast（含 429 冷却）+ refetch。
            modal::onEditNicknameResult((unsigned int)wp);
            return 0;
        }
        case WM_APP + 62: {                    // market listing detail loaded
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_APP + 63: {                    // market purchase result (wp = success)
            modal::onMarketPurchaseResult(wp != 0);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_APP + 64: {                    // market review result (wp = success)
            modal::onMarketReviewResult(wp != 0);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_APP + 65: {                    // emoji 反应 POST 失败 → 回滚本地乐观聚合
            chat::applyReactFailure();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_APP + 36: {                    // peer profile fetched
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_APP + 37: {                    // chat 头像右键 → 看主页 (lp = std::wstring*)
            std::unique_ptr<std::wstring> p((std::wstring*)lp);
            if (p && !p->empty()) modal::openUserProfile(*p);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case tray::kTrayCallbackMsg: {
            if (tray::onCallback(hwnd, wp, lp)) return 0;
            return 0;
        }
        case WM_RBUTTONDOWN: {
            POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            POINT dip = physToDip(pt);
            if (modal::onMouseRDown(hwnd, dip)) {
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            // 在 chat 里右键命中头像 → 看主页（不要最小化）
            if (stages::g_stage == stages::Stage::Main
                && stages::g_view == stages::View::Chat
                && chat::onMouseRDown(hwnd, dip)) {
                return 0;
            }
            if (stages::g_stage == stages::Stage::Main) {
                tray::hideToTray(hwnd);
                return 0;
            }
            PostQuitMessage(0);
            return 0;
        }
        case WM_DESTROY:
            ws::stop();
            tray::remove();
            PostQuitMessage(0);
            return 0;
        case WM_CLOSE:
            ws::stop();
            tray::remove();
            DestroyWindow(hwnd);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int APIENTRY wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // 单实例守卫：已有实例在跑则前置它并退出（测试可用 LAUNCHER_ALLOW_MULTI=1 绕过）
    HANDLE single_instance = acquireSingleInstance();
    if (single_instance == nullptr) return 0;

    // 加载持久化设置
    g_dark = persist::loadTheme(true);
    int lang = persist::loadLang((int)detectSystemLang());
    if (lang >= 0 && lang <= 2) g_lang = (Lang)lang;
    readSteamInfo();

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = inst;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"LauncherD2DPreview";
    RegisterClassExW(&wc);

    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);

    UINT dpi_for_init = GetDpiForSystem();
    if (dpi_for_init == 0) dpi_for_init = 96;
    int W0 = (int)(40.0f * dpi_for_init / 96.0f);
    int H0 = (int)(40.0f * dpi_for_init / 96.0f);

    HWND hwnd = CreateWindowExW(
        WS_EX_NOREDIRECTIONBITMAP | WS_EX_ACCEPTFILES,
        wc.lpszClassName, L"Launcher",
        WS_POPUP,
        (sw - W0) / 2, (sh - H0) / 2, W0, H0,
        nullptr, nullptr, inst, nullptr);
    if (!hwnd) return 1;
    DragAcceptFiles(hwnd, TRUE);

    RECT rc{};
    GetClientRect(hwnd, &rc);
    if (!g_app.init(hwnd, rc.right - rc.left, rc.bottom - rc.top)) {
        DestroyWindow(hwnd);
        return 1;
    }
    // 后台解码服务 —— 所有 WIC 解码在此，完成后 PostMessage(kMsgDecodeReady) 回 UI 线程。
    launcher::d2d::decodeService().start(hwnd);
    // Wave2: 后台下载池 —— 历史/WS 图片下载在此,去重合并,完成后 PostMessage(WM_APP+10)。
    launcher::d2d::DownloadPool::instance().start(4);

#ifdef LAUNCHER_VISUAL_SMOKE
    // 可插拔视觉冒烟钩子：置于 autologin 之前，绕过 applyEnvIdentity/validateSavedSession/
    // afterLogin 及所有 live fetch。run() 自带帧循环，绝不进正常消息循环。移除见 visual_smoke.h。
    if (visual_smoke::requested()) {
        int rc_smoke = visual_smoke::run(g_app, hwnd);
        launcher::d2d::decodeService().stop();
        g_app.shutdown();          // 镜像正常收尾（main 末尾 g_app.shutdown()）
        DestroyWindow(hwnd);
        return rc_smoke;
    }
#endif

    // 启动后尝试 autologin — 有 saved session 且 saved creds 时直接走主页
    {
        std::string tok, uid;
        bool from_env = false;
        std::string env_tok = envStringA("LAUNCHER_SESSION_TOKEN");
        std::string env_uid = envStringA("LAUNCHER_USER_ID");
        if (!env_tok.empty()) {
            tok = env_tok;
            uid = env_uid;
            from_env = true;
            applyEnvIdentity();
        } else if (persist::loadSession(tok, uid) && !tok.empty()) {
            from_env = false;
        }
        if (!tok.empty()) {
            g_session_token = tok;
            g_user_id = uid;
            stages::g_auth_validation_pending = true;
            // 头像如果之前下载过，本地路径还在
            wchar_t base[MAX_PATH] = {0};
            if (SHGetSpecialFolderPathW(nullptr, base, CSIDL_LOCAL_APPDATA, FALSE)) {
                std::wstring dir = std::wstring(base) + L"\\Launcher\\avatars\\self";
                std::wstring stem;
                for (char c : g_user_id) {
                    bool ok = (c >= '0' && c <= '9')
                           || (c >= 'a' && c <= 'z')
                           || (c >= 'A' && c <= 'Z')
                           || c == '-' || c == '_';
                    stem.push_back(ok ? (wchar_t)c : L'_');
                }
                for (const wchar_t* e : { L"png", L"jpg", L"jpeg", L"gif", L"webp", L"bmp" }) {
                    std::wstring p = dir + L"\\" + stem + L"." + e;
                    if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) {
                        g_avatar_path = p;
                        break;
                    }
                }
            }
            // 启动后异步刷新
            if (from_env) {
                stages::g_auth_validation_pending = false;
                stages::g_skip_auth_after_loading = true;
                afterLogin(hwnd);
            } else {
                validateSavedSession(hwnd, tok);
            }
        }
    }

    stages::enterDotStage();
    // 启动后异步查 IP 地理位置（公网 IP / VPN 出口国家）
    fetch::geoIP(hwnd);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    auto last = std::chrono::steady_clock::now();
    MSG msg{};
    bool quit = false;
    while (!quit) {
        DWORD r = g_app.waitFrame(100);
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { quit = true; break; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (quit) break;
        if (r != WAIT_OBJECT_0) continue;

        auto now = std::chrono::steady_clock::now();
        float dt = std::chrono::duration<float>(now - last).count();
        last = now;
        if (dt > 0.1f) dt = 0.1f;

        stages::tick(dt);
        ui::tickMain(dt);
        toast::tick(dt);
        stages::driveTransitions(g_app, sw, sh);

        if (g_app.beginFrame()) {
            stages::paint(g_app);
            g_app.endFrame();
        }
    }

    launcher::d2d::decodeService().stop();   // join worker 线程（须在 g_app.shutdown 前）
    g_app.shutdown();
    return 0;
}
