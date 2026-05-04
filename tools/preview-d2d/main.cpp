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
#include <memory>
#include <utility>

#pragma comment(lib, "user32.lib")

using namespace launcher::d2d;

static D2DApp g_app;

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
            return anyHover(dip) ? HTCLIENT : HTCAPTION;
        }
        case WM_MOUSEMOVE: {
            POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            g_mouse = physToDip(pt);
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
                if (chat::g_picker_open) {
                    chat::g_picker_open = false;
                    chat::g_picker_t.start(chat::g_picker_t.value(), 0, 0.18f,
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
            if (wp == 'D') { g_dark = !g_dark; persist::saveTheme(g_dark); return 0; }
            if (wp == 'S' && stages::g_stage == stages::Stage::Loading) {
                stages::g_skip_auth_after_loading = true;
                stages::g_time_in_stage = 1.5f;
                return 0;
            }
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
            toast::show(wp ? L"头像已同步到云端 ✓" : L"头像上传失败");
            return 0;
        }
        case WM_APP + 4: {                     // ChangePw result
            modal::onChangePwResult(wp != 0);
            if (wp) toast::show(L"密码已修改，请重新登录");
            return 0;
        }
        case WM_APP + 5: {                     // Chat official channels result
            chat::applyOfficialResult();
            // 拉到 chat_id 后立即 fetchHistory 当前频道
            chat::fetchHistory(hwnd, chat::g_active);
            return 0;
        }
        case WM_APP + 10: {                    // WS message arrived
            ws::drain();
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
                toast::show(L"表情包已创建");
                // 立即重拉 packs 列表，新 pack 出现在 picker（不用重启客户端）
                sticker::fetchMyPacks(hwnd);
            }
            return 0;
        }
        case WM_APP + 20: {                    // pack rename result
            modal::onRenamePackResult(wp != 0);
            if (wp) toast::show(L"已重命名");
            return 0;
        }
        case WM_APP + 21: {                    // chat picker → 新建表情包
            modal::openCreatePack();
            return 0;
        }
        case WM_APP + 23: {                    // 远程头像下载完成；lp = std::wstring*
            std::unique_ptr<std::wstring> p((std::wstring*)lp);
            if (p && !p->empty()) {
                g_avatar_path = *p;
                g_app.images().invalidate();   // 强制下次重新解码
                toast::show(L"头像已从云端同步 ✓");
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
            if (wp) {
                if (lp) {
                    auto* sn = (std::string*)lp;
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
                    toast::show(L"分享链接已复制到剪贴板 ✓");
                } else {
                    toast::show(L"已取消分享");
                }
                // 不重拉 packs（避免 active tab 索引错乱）— sharePack 内部已写回 short_name
            } else toast::show(L"分享失败");
            return 0;
        }
        case WM_APP + 27: {                    // pack delete result
            if (wp) {
                toast::show(L"已删除表情包");
                sticker::fetchMyPacks(hwnd);    // 立即刷新 picker 让 pack 消失
            } else toast::show(L"删除失败");
            return 0;
        }
        case WM_APP + 28: {                    // pack install result
            if (wp) {
                toast::show(L"已安装表情包");
                sticker::fetchMyPacks(hwnd);
            } else toast::show(L"安装失败");
            return 0;
        }
        case WM_APP + 30: {                    // login history result; lp = std::string*
            std::unique_ptr<std::string> p((std::string*)lp);
            if (p) modal::onHistoryResult(*p);
            return 0;
        }
        case WM_APP + 31: {                    // chat picker → rename pack
            auto* pid = (std::string*)wp;
            auto* nm = (std::wstring*)lp;
            if (pid && nm) modal::openRenamePack(*pid, *nm);
            return 0;
        }
        case WM_APP + 32: {                    // chat picker → delete pack confirm
            auto* pid = (std::string*)wp;
            auto* nm = (std::wstring*)lp;
            if (pid && nm) {
                std::string id_copy = *pid;
                std::wstring nm_copy = *nm;
                modal::openConfirm(L"删除表情包",
                    L"确认删除「" + nm_copy + L"」？分享出去的也会失效。",
                    [id_copy](){
                        sticker::deletePack(GetActiveWindow(), id_copy);
                    },
                    L"删除", L"取消", true);
            }
            return 0;
        }
        case WM_APP + 33: {                    // market listings fetched
            return 0;
        }
        case WM_APP + 29: {                    // sticker import 完成；wp=成功数 lp=pack_id
            wchar_t buf[64];
            if (wp > 0) {
                swprintf_s(buf, L"已导入 %d 张表情 ✓", (int)wp);
                toast::show(buf);
                // 切 picker active tab 到这个 pack（让用户立刻看到导入的图）
                if (lp) {
                    auto* pid = (std::string*)lp;
                    int idx = -1;
                    for (size_t i = 0; i < sticker::g_packs.size(); ++i) {
                        if (sticker::g_packs[i].id == *pid) { idx = (int)i; break; }
                    }
                    if (idx >= 0) {
                        chat::g_picker_tab = 1 + idx;     // 0 = emoji, 1+ = pack idx
                        if (!chat::g_picker_open) {
                            chat::g_picker_open = true;
                            chat::g_picker_t.start(0, 1, 0.22f, 0, curve::easeOutBack);
                        }
                    }
                }
            } else {
                toast::show(L"未上传任何文件（检查文件夹）");
            }
            return 0;
        }
        case WM_APP + 34: {                    // chat picker → 弹文件夹对话框 + 上传
            auto* pid = (std::string*)wp;
            if (pid && !pid->empty()) {
                int total = sticker::totalUserStickers();
                if (total >= 50) {
                    toast::show(L"已达上限（50/用户）— 删些再导入");
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
            auto* p = (std::wstring*)wp;
            if (p) sticker::deleteSticker(hwnd, *p);
            return 0;
        }
        case WM_APP + 41: {                    // chat picker → 我的表情想导入
            toast::show(L"请先切到/新建一个表情包分组再导入");
            return 0;
        }
        case WM_APP + 45: {                    // chat history fetched (lp = std::wstring* slug)
            std::unique_ptr<std::wstring> p((std::wstring*)lp);
            if (p) chat::applyHistoryResult(*p);
            return 0;
        }
        case WM_APP + 46: {                    // chat video bubble 点击 (wp = std::wstring* path)
            auto* p = (std::wstring*)wp;
            if (p && !p->empty()) modal::openVideoPlayer(*p);
            return 0;
        }
        case WM_APP + 47: {                    // chat text 链接点击 (wp = std::wstring* url)
            auto* p = (std::wstring*)wp;
            if (p && !p->empty()) modal::openWebPage(*p);
            return 0;
        }
        case WM_APP + 43: {                    // sticker 导出完成 (wp = success count)
            wchar_t buf[64];
            if (wp > 0) {
                swprintf_s(buf, L"已导出 %d 张到目标文件夹 ✓", (int)wp);
                toast::show(buf);
            } else {
                toast::show(L"导出失败（文件夹无法访问）");
            }
            return 0;
        }
        case WM_APP + 44: {                    // pack preview 加载完
            return 0;
        }
        case WM_APP + 48: {                    // 拖拽排序云端写入完成
            if (wp == 0) toast::show(L"排序保存失败（仅本地）");
            return 0;
        }
        case WM_APP + 49: {                    // chat 链接卡片点击 → 弹 PackPreview (wp = std::string* short)
            auto* p = (std::string*)wp;
            if (p && !p->empty()) modal::openPackPreviewModal(*p);
            return 0;
        }
        case WM_APP + 50: {                    // 消息右键菜单 (wp = struct{POINT, idx}*)
            struct Pl { POINT pt; int idx; };
            auto* p = (Pl*)wp;
            if (p) modal::openMsgContextMenu(p->pt, p->idx);
            return 0;
        }
        case WM_APP + 52: {                    // 发消息后，把 server message_id 绑到本地 me 消息
            chat::applySendResult();
            return 0;
        }
        case WM_APP + 53: {                    // 删消息结果（仅自己消息）
            if (wp == 0) toast::show(launcher::d2d::trW("toast.delete_fail"));
            return 0;
        }
        case WM_APP + 54: {                    // myProfile 拉回
            // g_user / g_status 已经在 fetch::myProfile worker 线程里写好；这里仅触发重画
            return 0;
        }
        case WM_APP + 51: {                    // chat picker → 卸载非 owner pack (wp/lp 同 +32)
            auto* pid = (std::string*)wp;
            auto* nm = (std::wstring*)lp;
            if (pid && nm) {
                std::string id_copy = *pid;
                std::wstring nm_copy = *nm;
                modal::openConfirm(L"卸载表情包",
                    L"确认卸载「" + nm_copy + L"」？只是从你的列表移除，不影响别人。",
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
                    L"卸载", L"取消", true);
            }
            return 0;
        }
        case WM_APP + 35: {                    // profile update result
            modal::onEditStatusTextResult(wp != 0);
            modal::onEditBioResult(wp != 0);
            if (wp) toast::show(L"已保存");
            return 0;
        }
        case WM_APP + 36: {                    // peer profile fetched
            // paint 帧自动用最新 g_peer
            return 0;
        }
        case WM_APP + 37: {                    // chat 头像右键 → 看主页 (lp = std::wstring*)
            std::unique_ptr<std::wstring> p((std::wstring*)lp);
            if (p && !p->empty()) modal::openUserProfile(*p);
            return 0;
        }
        case tray::kTrayCallbackMsg: {
            if (tray::onCallback(hwnd, wp, lp)) return 0;
            return 0;
        }
        case WM_RBUTTONDOWN: {
            POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            POINT dip = physToDip(pt);
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

    // 加载持久化设置
    g_dark = persist::loadTheme(true);
    int lang = persist::loadLang((int)Lang::ZhCN);
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

    // 启动后尝试 autologin — 有 saved session 且 saved creds 时直接走主页
    {
        std::string tok, uid;
        if (persist::loadSession(tok, uid) && !tok.empty()) {
            g_session_token = tok;
            g_user_id = uid;
            stages::g_skip_auth_after_loading = true;
            // 头像如果之前下载过，本地路径还在
            wchar_t base[MAX_PATH] = {0};
            if (SHGetSpecialFolderPathW(nullptr, base, CSIDL_LOCAL_APPDATA, FALSE)) {
                std::wstring dir = std::wstring(base) + L"\\Launcher";
                for (const wchar_t* e : { L"png", L"jpg", L"jpeg", L"gif", L"webp", L"bmp" }) {
                    std::wstring p = dir + L"\\avatar." + e;
                    if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) {
                        g_avatar_path = p;
                        break;
                    }
                }
            }
            // 启动后异步刷新
            PostMessageW(hwnd, WM_APP + 50, 0, 0);   // 自定义：稍后 afterLogin
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
    bool autologin_kicked = false;
    while (!quit) {
        DWORD r = g_app.waitFrame(100);
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { quit = true; break; }
            if (msg.message == WM_APP + 50 && !autologin_kicked) {
                autologin_kicked = true;
                afterLogin(hwnd);
                continue;
            }
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

    g_app.shutdown();
    return 0;
}
