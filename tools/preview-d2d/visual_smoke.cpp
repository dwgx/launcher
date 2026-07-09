// visual_smoke.cpp — 可插拔视觉冒烟钩子实现。整个 TU 在 #ifdef LAUNCHER_VISUAL_SMOKE 内。
//
// 原理（见 PLAN-hook_design / capture_mechanism）：run() 自带
// beginFrame→stages::paint→capture→endFrame 循环，从不调用任何 tick，所以 paint
// 是被冻结全局的纯函数，帧完全确定。截图只经公共 accessor app.ctx()/app.wic() +
// ctx()->GetTarget()，不碰 D2DApp 私有 back_buffer_/swap_，故 d2d_app.{h,cpp} 零改动。
#include "visual_smoke.h"

#ifdef LAUNCHER_VISUAL_SMOKE

#include "stages.h"
#include "ui_main.h"
#include "auth.h"
#include "chat.h"
#include "chat_internal.h"   // g_streams / g_streams_mtx（模块内头，仅本钩子借用做确定性 seed）
#include "modals.h"
#include "fetch.h"
#include "user_state.h"
#include "hit.h"             // dispatchClick — clickReal 复刻真实 WM_LBUTTONDOWN 派发顺序
#include "overlay.h"         // g_overlays — 统一浮层栈(派发单一真源)

#include <wrl/client.h>
#include <wincodec.h>
#include <shellapi.h>   // CommandLineToArgvW（--visual-smoke 命令行解析）
#include <string>
#include <vector>
#include <cstdio>
#include <functional>

namespace launcher::d2d::visual_smoke {

using Microsoft::WRL::ComPtr;

// ----------------------------------------------------------------------------
//  requested() — env LAUNCHER_VISUAL_SMOKE 非空 或 命令行含 --visual-smoke
// ----------------------------------------------------------------------------
bool requested() {
    wchar_t buf[8]{};
    DWORD n = GetEnvironmentVariableW(L"LAUNCHER_VISUAL_SMOKE", buf, 8);
    if (n > 0) return true;   // 存在且非空

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool found = false;
    if (argv) {
        for (int i = 1; i < argc; ++i) {
            if (lstrcmpiW(argv[i], L"--visual-smoke") == 0) { found = true; break; }
        }
        LocalFree(argv);
    }
    return found;
}

// ----------------------------------------------------------------------------
//  outputDir() — 从 env LAUNCHER_VISUAL_SMOKE_OUT 或默认 .visual-smoke/ 取输出目录
//  (工作目录是 build_d2d_visual.bat pushd 后的 tools/preview-d2d)
// ----------------------------------------------------------------------------
static std::wstring outputDir() {
    std::wstring dir = L".visual-smoke";
    wchar_t buf[MAX_PATH]{};
    DWORD n = GetEnvironmentVariableW(L"LAUNCHER_VISUAL_SMOKE_OUT", buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) dir.assign(buf, n);
    CreateDirectoryW(dir.c_str(), nullptr);   // 已存在 → ERROR_ALREADY_EXISTS，忽略
    return dir;
}

// ----------------------------------------------------------------------------
//  capture() — 在 stages::paint 之后、app.endFrame() 之前调（target 只在此窗口活着）
//  live back buffer → CPU-readable copy → WIC PNG。仅用公共 ctx()/wic()。
// ----------------------------------------------------------------------------
static bool capture(D2DApp& app, const std::wstring& path) {
    auto* ctx = app.ctx();
    if (!ctx) return false;
    ctx->Flush(nullptr, nullptr);                    // 把排队的 draw 刷到 surface

    ComPtr<ID2D1Image> tgt_img;
    ctx->GetTarget(&tgt_img);
    ComPtr<ID2D1Bitmap1> src;
    if (!tgt_img || FAILED(tgt_img.As(&src))) return false;   // live back-buffer wrapper
    D2D1_SIZE_U px = src->GetPixelSize();

    // back buffer 是 TARGET|CANNOT_DRAW（非 CPU-readable，d2d_app.cpp:230）→ 建 CPU_READ 副本
    D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    ComPtr<ID2D1Bitmap1> dst;
    if (FAILED(ctx->CreateBitmap(px, nullptr, 0, &bp, &dst))) return false;
    if (FAILED(dst->CopyFromBitmap(nullptr, src.Get(), nullptr))) return false;

    D2D1_MAPPED_RECT mr{};
    if (FAILED(dst->Map(D2D1_MAP_OPTIONS_READ, &mr))) return false;

    bool ok = false;
    auto* wic = app.wic();
    if (wic) {
        ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapEncoder> enc;
        ComPtr<IWICBitmapFrameEncode> frame;
        ComPtr<IPropertyBag2> props;
        if (SUCCEEDED(wic->CreateStream(&stream)) &&
            SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
            SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
            SUCCEEDED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
            SUCCEEDED(enc->CreateNewFrame(&frame, &props)) &&
            SUCCEEDED(frame->Initialize(props.Get()))) {
            frame->SetSize(px.width, px.height);
            // premultiplied BGRA — 与 back buffer 格式精确匹配，WritePixels 即 memcpy
            WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppPBGRA;
            frame->SetPixelFormat(&fmt);
            if (SUCCEEDED(frame->WritePixels(px.height, mr.pitch, mr.pitch * px.height, mr.bits)) &&
                SUCCEEDED(frame->Commit()) &&
                SUCCEEDED(enc->Commit())) {
                ok = true;
            }
        }
    }

    dst->Unmap();
    dst.Reset();   // endFrame 前释放，避免持有 buffer 引用
    return ok;
}

// ----------------------------------------------------------------------------
//  freezeMainTweens() — run() 从不 tick，未 start 的 tween value() 返 from(0)
//  → sidebar/opacity 会是 0、视图全空。用 start(1,1,0.001f) 强制入场动画完成：
//  start 置 elapsed=-delay=0，value() 在 elapsed<=0 时返 from=1（anim.h:29-33），
//  无需 tick 即恒返 1。直接置 g_stage=Main（不走 enterMainStage，它会重新 arm 滑入 tween）。
// ----------------------------------------------------------------------------
static void freezeToMain() {
    using namespace stages;
    g_sidebar_x.start(1, 1, 0.001f);
    g_topbar_y.start(1, 1, 0.001f);
    g_main_opacity.start(1, 1, 0.001f);
    g_view_fade.start(1, 1, 0.001f);
    g_stage = Stage::Main;
    g_time_in_stage = 0.0f;   // 冻结 caret 相位（auth.cpp:134 用它算闪烁）
}

// 强制某个 modal/overlay 的入场 tween 立即完成（同 freeze 原理）。
static void freezeTween(Tween& t) { t.start(1, 1, 0.001f); }

// ----------------------------------------------------------------------------
//  seedFixtures() — 一次性注入静态内容，让离线（无后端）也能渲出有意义的帧。
// ----------------------------------------------------------------------------
static void seedFixtures() {
    // g_user 已有合理默认（user_state.h），补状态/签名让 profile/编辑框有内容
    g_user.status_text = L"Building the launcher";
    g_user.bio = L"Visual-smoke fixture user. Offline deterministic render.";
    g_status = UserStatus::Online;

    // 头像置空 → drawAvatarPill 走「主色圆 + 首字母」占位（确定性，ui_main.cpp:135-143）。
    // 注意：不留残留的旧路径，否则会试图从磁盘加载不确定的图。
    g_avatar_path.clear();

    // 抑制 auth 表单闪烁 caret（focus=-1 → 无 field focused，auth.cpp:131）
    auth::g_form.focus = -1;

    // chat：确定性频道 + 一条静态消息流，关掉 composer 焦点与表情选择器
    chat::g_active = L"general";
    chat::g_focus_composer = false;
    chat::g_picker_open = false;
    {
        std::lock_guard<std::mutex> lk(chat::g_streams_mtx);
        auto& msgs = chat::g_streams[L"general"];
        msgs.clear();
        chat::Msg m1;
        m1.kind = chat::MsgKind::Text;
        m1.from = L"peer";
        m1.author = L"alice";
        m1.author_key = L"alice";
        m1.peer_key = L"alice";
        m1.body = L"Welcome to the general channel.";
        m1.time = L"12:00";
        m1.send_state = chat::MsgSendState::Sent;
        msgs.push_back(m1);
        chat::Msg m2;
        m2.kind = chat::MsgKind::Text;
        m2.from = L"me";
        m2.author = g_user.nickname;
        m2.author_key = g_user.username;
        m2.body = L"Hello — this is a visual-smoke fixture message.";
        m2.time = L"12:01";
        m2.send_state = chat::MsgSendState::Sent;
        msgs.push_back(m2);
    }

    // 离线 market：置 loaded=true → 显示 empty 态而非 loading spinner（ui_main.cpp:754-761）
    fetch::g_market_loaded = true;
}

// ----------------------------------------------------------------------------
//  resetOverlays() — 关掉所有浮层，回到「Home 干净基底」。scenario 之间调用，
//  避免上一屏的 modal/dropdown/picker 残留到下一帧（paint 是 .open||.t>0 才画）。
// ----------------------------------------------------------------------------
static void closeTween(Tween& t) { t.start(0, 0, 0.001f); }

static void resetOverlays() {
    ui::g_account_dropdown = false;
    closeTween(ui::g_dropdown_t);

    // 全部 18 个浮层都必须归零 —— 之前只关了一部分,残留的 market_detail/cs2/history
    // 等会让 anyOpen() 在下一个 case 仍为真,污染 modal::onMouseLDown 判定。交互矩阵
    // 逐个 case 依赖「干净基底」,故这里穷举所有 .open + 各自 tween。
    modal::g_change_pw.open = false;          closeTween(modal::g_change_pw.t);
    modal::g_confirm.open = false;            closeTween(modal::g_confirm.t);
    modal::g_cs2.open = false;                closeTween(modal::g_cs2.t);
    modal::g_market_detail_modal.open = false; closeTween(modal::g_market_detail_modal.t);
    modal::g_history.open = false;            closeTween(modal::g_history.t);
    modal::g_addtag.open = false;             closeTween(modal::g_addtag.t);
    modal::g_createpack.open = false;         closeTween(modal::g_createpack.t);
    modal::g_renamepack.open = false;         closeTween(modal::g_renamepack.t);
    modal::g_user_profile.open = false;       closeTween(modal::g_user_profile.t);
    modal::g_user_profile.cur_key.clear();    // 清 dedupe key,避免 I6 case 间互相影响
    modal::g_edit_status.open = false;        closeTween(modal::g_edit_status.t);
    modal::g_edit_bio.open = false;           closeTween(modal::g_edit_bio.t);
    modal::g_edit_nickname.open = false;      closeTween(modal::g_edit_nickname.t);
    modal::g_mute_user.open = false;          closeTween(modal::g_mute_user.t);
    modal::g_pack_preview_modal.open = false; closeTween(modal::g_pack_preview_modal.t);
    modal::g_webview_modal.open = false;      closeTween(modal::g_webview_modal.t);
    modal::g_search.open = false;             closeTween(modal::g_search.t);
    // 上下文/浮动菜单也归零 —— 统一焦点模型的回归测试依赖 case 间干净基底
    // (菜单在 anyOpen() 内,残留会让 modal::onMouseLDown 误判)。
    modal::g_msg_menu.open = false;     closeTween(modal::g_msg_menu.t);
    modal::g_user_menu.open = false;    closeTween(modal::g_user_menu.t);
    modal::g_chat_more.open = false;    closeTween(modal::g_chat_more.t);

    chat::g_picker_open = false;
    closeTween(chat::g_picker_t);

    // 浮层栈也清零 —— 派发已全走 g_overlays。虽然每个 case 都会 paintFrame() 重建栈,
    // 但"无浮层→点击应返回 false"这类 case 若不先 paint 会读到上一 case 的残留栈。
    g_overlays.clear();
}

// ----------------------------------------------------------------------------
//  runInteractionTests() — 程序化交互冒烟。
//  真实代码路径:先 paint 一帧(注册 hit + 重建浮层栈 g_overlays),再模拟点击调
//  modal::onMouseLDown(pt)/clickReal(pt),然后断言状态。专测反复出问题的浮层交互。
//  结果写到 <outputDir>\interaction-report.txt(主进程读),返回失败数。
// ----------------------------------------------------------------------------
static int runInteractionTests(D2DApp& app) {
    // 结果用最朴素的 ASCII 文件 + OutputDebugString,避免 fopen ccs 编码/缓冲的坑。
    std::wstring report = outputDir() + L"\\interaction-report.txt";
    FILE* f = _wfopen(report.c_str(), L"wb");
    int fails = 0;
    auto line = [&](const char* s) {
        OutputDebugStringA(s); OutputDebugStringA("\n");
        if (f) { fputs(s, f); fputc('\n', f); fflush(f); }
    };
    auto check = [&](const char* name, bool ok) {
        if (!ok) ++fails;
        char buf[128]; _snprintf_s(buf, sizeof buf, _TRUNCATE, "%s %s", ok ? "PASS" : "FAIL", name);
        line(buf);
    };
    auto trace = [&](const char* where) {
        char buf[128]; _snprintf_s(buf, sizeof buf, _TRUNCATE, "-- %s", where);
        line(buf);
    };
    trace("ENTER");
    // paintFrame:跑一帧让当前状态注册 hit + 重建浮层栈 g_overlays(paint 是纯函数)。
    auto paintFrame = [&]() {
        if (app.beginFrame()) { stages::paint(app); app.endFrame(); }
    };
    // 屏幕中心 & 远离任何模态的角落(用于「点模态外」)。1100x720 dip。
    const POINT center{ 550, 360 };
    const POINT far_corner{ 30, 700 };   // 左下角,任何居中模态都不覆盖

    // clickReal:复刻 main.cpp:213-220 真实 WM_LBUTTONDOWN 三路派发顺序,让测试走
    // 真正的泄漏路径(而非旧测试只直呼 modal::onMouseLDown 的模态路径)。picker 只活在
    // chat 路径下,只有经此才能暴露「点 picker 外泄漏到身后视图」的 bug。
    auto clickReal = [&](POINT pt) {
        g_mouse = pt;   // 关键:真实 WM_LBUTTONDOWN 先设 g_mouse,测试之前漏了 → 行为偏差
        g_mouse_pressed = true;
        if (modal::onMouseLDown(nullptr, pt)) { g_mouse_pressed = false; return; }
        if (stages::g_view == stages::View::Chat) chat::onMouseLDown(nullptr, pt);
        else dispatchClick(pt);
        g_mouse_pressed = false;
    };

    // --- 测 1:资料卡点外关闭 ---
    trace("t1.reset");     resetOverlays();
    stages::g_view = stages::View::Chat;
    trace("t1.open");      modal::openUserProfile(L"me");
    trace("t1.freeze");    freezeTween(modal::g_user_profile.t);
    trace("t1.paint");     paintFrame();
    check("user_profile.open_after_open", modal::g_user_profile.open);
    trace("t1.click");     modal::onMouseLDown(nullptr, far_corner);   // 点卡片外
    check("user_profile.closed_on_outside_click", !modal::g_user_profile.open);

    // --- 测 2:资料卡点内不关 ---
    resetOverlays();
    modal::openUserProfile(L"me");
    freezeTween(modal::g_user_profile.t);
    paintFrame();
    modal::onMouseLDown(nullptr, center);       // 点卡片内(居中区)
    check("user_profile.stays_open_on_inside_click", modal::g_user_profile.open);

    // --- 测 3:搜索点外关闭 ---
    resetOverlays();
    modal::g_search.open = true;
    freezeTween(modal::g_search.t);
    paintFrame();
    check("search.open_after_open", modal::g_search.open);
    modal::onMouseLDown(nullptr, far_corner);
    check("search.closed_on_outside_click", !modal::g_search.open);

    // --- 测 4:三个点菜单点外关闭 ---
    resetOverlays();
    stages::g_view = stages::View::Chat;
    modal::openChatMoreMenu(POINT{ 1050, 100 });
    freezeTween(modal::g_chat_more.t);
    paintFrame();
    check("chat_more.open_after_open", modal::g_chat_more.open);
    modal::onMouseLDown(nullptr, far_corner);
    check("chat_more.closed_on_outside_click", !modal::g_chat_more.open);

    // --- 测 5:改密码模态点外关闭 ---
    resetOverlays();
    modal::g_change_pw.open = true;
    freezeTween(modal::g_change_pw.t);
    paintFrame();
    modal::onMouseLDown(nullptr, far_corner);
    check("change_pw.closed_on_outside_click", !modal::g_change_pw.open);

    // --- 测 6:无模态时点击不被 onMouseLDown 吞(返回 false,交给下层) ---
    resetOverlays();
    paintFrame();
    check("no_modal.onMouseLDown_returns_false", !modal::onMouseLDown(nullptr, center));

    // ========================================================================
    //  统一焦点模型回归:msg_menu / user_menu / chat_more / picker 的
    //  「点浮层外关闭 + 不泄漏到身后视图」。旧测试只走 modal 路径且漏了
    //  msg_menu/user_menu/picker —— 正是这类 bug 得以出厂的原因。这里用
    //  clickReal(真实三路派发)+ 可观测泄漏探针(g_focus_composer / g_active)。
    // ========================================================================

    // --- T-picker-1:点 picker 外(输入框)不泄漏出输入框焦点 [修复前 FAIL,修复后 PASS] ---
    // 这是「先失败后通过」的核心证明:修复前 chat::onMouseLDown 在关闭前先
    // dispatchClick,命中输入框 -> g_focus_composer=true -> FAIL;修复后点外提前
    // 吞掉 -> g_focus_composer 保持 false -> PASS。
    resetOverlays();
    stages::g_view = stages::View::Chat;
    chat::g_active = L"general";
    chat::g_focus_composer = false;
    chat::setPickerOpen(true);
    freezeTween(chat::g_picker_t);
    paintFrame();
    {
        // 输入框中心;picker 浮在输入框上方,其中心应在 picker 矩形之外。
        POINT composer_center{
            (LONG)(chat::g_composer.bounds.x + chat::g_composer.bounds.w * 0.5f),
            (LONG)(chat::g_composer.bounds.y + chat::g_composer.bounds.h * 0.5f) };
        check("picker.composer_outside_picker",
              !chat::g_picker_rect.contains(composer_center));
        clickReal(composer_center);
        check("picker.no_leak_composer_focus", chat::g_focus_composer == false);
    }

    // --- T-picker-2:点远角(空白)关闭 picker [两侧都过,固定行为] ---
    resetOverlays();
    stages::g_view = stages::View::Chat;
    chat::setPickerOpen(true);
    freezeTween(chat::g_picker_t);
    paintFrame();
    clickReal(far_corner);
    check("picker.closed_on_outside_click", !chat::g_picker_open);

    // --- T-picker-3:点频道列(x<240)不泄漏成切频道 [修复前 FAIL,修复后 PASS] ---
    // seed 第 2 个频道,点频道列内、picker 外的一点,断言 g_active 未翻转。
    resetOverlays();
    stages::g_view = stages::View::Chat;
    chat::g_active = L"general";
    {
        std::lock_guard<std::mutex> lk(chat::g_streams_mtx);
        auto& r = chat::g_streams[L"random"];
        if (r.empty()) { chat::Msg m; m.kind = chat::MsgKind::Text; m.body = L"x"; r.push_back(m); }
    }
    chat::setPickerOpen(true);
    freezeTween(chat::g_picker_t);
    paintFrame();
    {
        // 频道列列宽约 240dip;选一点在列内且在 picker 矩形之外。
        POINT chan_pt{ 120, 300 };
        if (!chat::g_picker_rect.contains(chan_pt)) {
            clickReal(chan_pt);
            check("picker.no_leak_channel_switch", chat::g_active == L"general");
        } else {
            check("picker.no_leak_channel_switch (skipped: rect overlap)", true);
        }
    }

    // --- T-msg-1/2:右键消息菜单 点外关闭 + 不泄漏 [HEAD 应过,回归守卫] ---
    resetOverlays();
    stages::g_view = stages::View::Chat;
    chat::g_active = L"general";
    chat::g_focus_composer = false;
    modal::openMsgContextMenu(POINT{ 550, 300 }, 0);
    freezeTween(modal::g_msg_menu.t);
    paintFrame();
    check("msg_menu.open_after_open", modal::g_msg_menu.open);
    clickReal(far_corner);
    check("msg_menu.closed_on_outside_click", !modal::g_msg_menu.open);
    check("msg_menu.no_leak",
          chat::g_focus_composer == false && chat::g_active == L"general");

    // --- T-user-1/2:用户上下文菜单 点外关闭 + 不泄漏 [HEAD 应过,回归守卫] ---
    resetOverlays();
    stages::g_view = stages::View::Chat;
    chat::g_active = L"general";
    chat::g_focus_composer = false;
    modal::openUserContextMenu(POINT{ 550, 300 }, L"alice", L"alice");
    freezeTween(modal::g_user_menu.t);
    paintFrame();
    check("user_menu.open_after_open", modal::g_user_menu.open);
    clickReal(far_corner);
    check("user_menu.closed_on_outside_click", !modal::g_user_menu.open);
    check("user_menu.no_leak",
          chat::g_focus_composer == false && chat::g_active == L"general");

    // --- T-more-1:三点菜单 点外不泄漏(扩展测 4:补 no-leak 探针) ---
    resetOverlays();
    stages::g_view = stages::View::Chat;
    chat::g_active = L"general";
    chat::g_focus_composer = false;
    modal::openChatMoreMenu(POINT{ 1050, 100 });
    freezeTween(modal::g_chat_more.t);
    paintFrame();
    check("chat_more.open_after_open2", modal::g_chat_more.open);
    clickReal(far_corner);
    check("chat_more.closed_on_outside_click2", !modal::g_chat_more.open);
    check("chat_more.no_leak",
          chat::g_focus_composer == false && chat::g_active == L"general");

    // === 补全:其余 overlay 的「点外关闭」(I1)穷尽覆盖 ===
    // 用 lambda 统一测:open 后 paintFrame,断言 open;clickReal 点外,断言 closed。
    auto testOutsideClose = [&](const char* nm, std::function<void()> open,
                                bool* open_flag) {
        resetOverlays();
        stages::g_view = stages::View::Home;
        open();
        paintFrame();
        char b1[96]; _snprintf_s(b1, sizeof b1, _TRUNCATE, "%s.open_after_open", nm);
        check(b1, *open_flag);
        clickReal(far_corner);
        char b2[96]; _snprintf_s(b2, sizeof b2, _TRUNCATE, "%s.closed_on_outside_click", nm);
        check(b2, !*open_flag);
    };

    testOutsideClose("history",
        [](){ modal::g_history.open = true; freezeTween(modal::g_history.t); },
        &modal::g_history.open);
    testOutsideClose("addtag",
        [](){ modal::openAddTag(); freezeTween(modal::g_addtag.t); },
        &modal::g_addtag.open);
    testOutsideClose("createpack",
        [](){ modal::openCreatePack(); freezeTween(modal::g_createpack.t); },
        &modal::g_createpack.open);
    testOutsideClose("renamepack",
        [](){ modal::openRenamePack("pid1", L"MyPack"); freezeTween(modal::g_renamepack.t); },
        &modal::g_renamepack.open);
    testOutsideClose("edit_status",
        [](){ modal::openEditStatusText(); freezeTween(modal::g_edit_status.t); },
        &modal::g_edit_status.open);
    testOutsideClose("edit_bio",
        [](){ modal::g_edit_bio.open = true; freezeTween(modal::g_edit_bio.t); },
        &modal::g_edit_bio.open);
    testOutsideClose("edit_nickname",
        [](){ modal::openEditNickname(); freezeTween(modal::g_edit_nickname.t); },
        &modal::g_edit_nickname.open);
    testOutsideClose("market_detail",
        [](){ modal::openMarketDetail("m1"); freezeTween(modal::g_market_detail_modal.t); },
        &modal::g_market_detail_modal.open);
    testOutsideClose("mute_user",
        [](){ modal::openMuteUser(L"u1", L"someone"); freezeTween(modal::g_mute_user.t); },
        &modal::g_mute_user.open);

    // confirm 对话框:设计上「点外不关」(必须显式选是/否),验证这个预期行为。
    resetOverlays();
    stages::g_view = stages::View::Home;
    modal::openConfirm(L"T", L"Body", nullptr);
    freezeTween(modal::g_confirm.t);
    paintFrame();
    check("confirm.open_after_open", modal::g_confirm.open);
    clickReal(far_corner);
    check("confirm.blocks_outside_click_stays_open", modal::g_confirm.open);

    // ================= 重构新增覆盖(统一浮层栈 OverlayStack) =================
    // escReal / rdownReal:复刻 WndProc 的 ESC 与右键真实派发(都走 g_overlays)。
    auto escReal   = [&](){ return g_overlays.onEsc(); };
    auto rdownReal = [&](POINT p){ return modal::onMouseRDown(nullptr, p); };

    // --- N1: ESC 关最顶层浮层(此前 modal::onKey 分支;现走统一栈)---
    resetOverlays();
    stages::g_view = stages::View::Home;
    modal::openUserProfile(L"me");
    freezeTween(modal::g_user_profile.t);
    paintFrame();
    check("esc.closes_modal", (escReal(), !modal::g_user_profile.open));

    // --- N2: Confirm 点外不关,但 ESC 能关(取消语义)---
    resetOverlays();
    stages::g_view = stages::View::Home;
    modal::openConfirm(L"T", L"Body", nullptr);
    freezeTween(modal::g_confirm.t);
    paintFrame();
    check("esc.closes_confirm", (escReal(), !modal::g_confirm.open));

    // --- N3: 账号下拉点外关闭(GAP2 —— 此前靠全窗背景吞击,现入栈统一判定)---
    resetOverlays();
    stages::g_view = stages::View::Home;
    ui::g_account_dropdown = true;
    freezeTween(ui::g_dropdown_t);
    paintFrame();
    check("dropdown.open_after_open", ui::g_account_dropdown);
    clickReal(center);   // 中心远离右上角下拉卡片
    check("dropdown.closed_on_outside_click", !ui::g_account_dropdown);

    // --- N4: 账号下拉 ESC 关闭 ---
    resetOverlays();
    stages::g_view = stages::View::Home;
    ui::g_account_dropdown = true;
    freezeTween(ui::g_dropdown_t);
    paintFrame();
    check("dropdown.closed_on_esc", (escReal(), !ui::g_account_dropdown));

    // --- N4b: 模态打开时 dim 背景禁止拖窗(修 C4 —— WM_NCHITTEST 之前把 dim 上
    // 的按拖判成 HTCAPTION 拖走整窗,导致点外关闭静默失效)。pointBlocksDrag 应对
    // 阻塞浮层的整窗(含 dim)返回 true → WndProc 返回 HTCLIENT 而非 HTCAPTION。---
    resetOverlays();
    stages::g_view = stages::View::Home;
    modal::openMarketDetail("m1");
    freezeTween(modal::g_market_detail_modal.t);
    paintFrame();
    check("modal.dim_blocks_window_drag", g_overlays.pointBlocksDrag(far_corner));
    // 无浮层时空白处应可拖窗(pointBlocksDrag=false)。
    resetOverlays();
    stages::g_view = stages::View::Home;
    paintFrame();
    check("no_overlay.bg_draggable", !g_overlays.pointBlocksDrag(far_corner));

    // --- N5: 模态点"内"不关(inside-click 生效面 —— 此前仅 user_profile 覆盖)---
    resetOverlays();
    stages::g_view = stages::View::Home;
    modal::openMarketDetail("m1");
    freezeTween(modal::g_market_detail_modal.t);
    paintFrame();
    clickReal(center);   // 居中模态,center 落在卡片内
    check("market_detail.stays_open_on_inside_click", modal::g_market_detail_modal.open);

    // --- N6: picker 点"内"不关(defer 回 chat,修 C7 pack 拖拽路径的前提)---
    resetOverlays();
    stages::g_view = stages::View::Chat;
    chat::setPickerOpen(true);
    freezeTween(chat::g_picker_t);
    paintFrame();
    {
        // picker 卡片中心(g_picker_rect 在 paint 时设定)。
        POINT pin{ (LONG)(chat::g_picker_rect.x + chat::g_picker_rect.w * 0.5f),
                   (LONG)(chat::g_picker_rect.y + chat::g_picker_rect.h * 0.5f) };
        clickReal(pin);
        check("picker.stays_open_on_inside_click", chat::g_picker_open);
    }

    // --- N7: 右键菜单点"外"关闭(onMouseRDown 走统一栈,此前无覆盖)---
    resetOverlays();
    stages::g_view = stages::View::Chat;
    modal::openChatMoreMenu(POINT{ 1050, 100 });
    freezeTween(modal::g_chat_more.t);
    paintFrame();
    check("chat_more.open_for_rdown", modal::g_chat_more.open);
    rdownReal(far_corner);
    check("chat_more.closed_on_outside_rdown", !modal::g_chat_more.open);

    // --- N8: 无浮层时点击不消费(栈空 → onLDown 返回 false,交给下层 view)---
    resetOverlays();
    stages::g_view = stages::View::Home;
    paintFrame();
    check("no_overlay.ldown_returns_false", !modal::onMouseLDown(nullptr, center));

    // ============== 贴近实机:真实右键路径复现(Bug A + Bug B) ==============
    // 现有 msg_menu 测试直呼 openMsgContextMenu()+clickReal,绕过了真实右键命中检测
    // (chat::onMouseRDown 遍历 g_msg_row_hits/g_msg_hits)与真实坐标。这里 seed
    // text/image/sticker 三类消息,paint 填充命中表,再走真实 chat::onMouseRDown
    // 在每条消息实际矩形上右键,断言菜单开;然后点菜单外断言关。
    {
        // seed 三类消息到 general
        {
            std::lock_guard<std::mutex> lk(chat::g_streams_mtx);
            auto& msgs = chat::g_streams[L"general"];
            msgs.clear();
            auto mk = [&](chat::MsgKind k, const std::wstring& body, int64_t sid){
                chat::Msg m; m.kind = k; m.from = L"peer"; m.author = L"alice";
                m.author_key = L"alice"; m.peer_key = L"alice"; m.body = body;
                m.time = L"12:00"; m.send_state = chat::MsgSendState::Sent;
                m.server_id = sid; msgs.push_back(m);
            };
            mk(chat::MsgKind::Text,    L"plain text message", 1001);
            mk(chat::MsgKind::Image,   L"C:/nonexistent/img.png", 1002);
            mk(chat::MsgKind::Sticker, L"C:/nonexistent/sticker.png", 1003);
        }
        resetOverlays();
        stages::g_view = stages::View::Chat;
        chat::g_active = L"general";
        chat::g_focus_composer = false;
        paintFrame();   // 填充 g_msg_row_hits(每条可见消息一行)

        // 快照命中表(idx -> 行矩形中心),再逐类右键
        auto rowCenterForIdx = [&](int idx, POINT* out)->bool {
            for (auto& h : chat::g_msg_row_hits) {
                if (h.idx == idx) {
                    out->x = (LONG)(h.rect.x + h.rect.w * 0.5f);
                    out->y = (LONG)(h.rect.y + h.rect.h * 0.5f);
                    return true;
                }
            }
            return false;
        };
        const char* kindName[3] = { "text", "image", "sticker" };
        for (int idx = 0; idx < 3; ++idx) {
            resetOverlays();
            stages::g_view = stages::View::Chat;
            chat::g_active = L"general";
            paintFrame();
            POINT rc{};
            bool has_row = rowCenterForIdx(idx, &rc);
            char bh[96]; _snprintf_s(bh, sizeof bh, _TRUNCATE,
                "rmenu.%s.has_row_hit", kindName[idx]);
            check(bh, has_row);   // Bug A:每类消息都应有行命中矩形
            if (!has_row) continue;
            // 真实右键路径:先 modal 栈(无浮层→false),再 chat::onMouseRDown
            bool handled = chat::onMouseRDown(nullptr, rc);
            freezeTween(modal::g_msg_menu.t);
            char bo[96]; _snprintf_s(bo, sizeof bo, _TRUNCATE,
                "rmenu.%s.opens_on_rclick", kindName[idx]);
            check(bo, handled && modal::g_msg_menu.open);   // Bug A:菜单应弹出
            paintFrame();   // 让菜单注册进 g_overlays
            // 点菜单外(远角)→ 应关闭
            clickReal(far_corner);
            char bc[96]; _snprintf_s(bc, sizeof bc, _TRUNCATE,
                "rmenu.%s.closes_on_outside", kindName[idx]);
            check(bc, !modal::g_msg_menu.open);   // Bug B:点外应关
        }
    }

    // ===== NCHITTEST 根因回归(真正的 bug 源:菜单外/消息行被判 HTCAPTION 吞点击)=====
    // Bug B 真因:菜单(无 dim)打开时,菜单外的点被 NCHITTEST 判成 HTCAPTION →
    // Windows 进入拖窗、不发 WM_LBUTTONDOWN → onLDown 收不到 → 点外关不掉。
    // 修复:任一 dismiss_on_outside 浮层打开时,pointBlocksDrag 对整窗返回 true。
    {
        resetOverlays();
        stages::g_view = stages::View::Chat;
        chat::g_active = L"general";
        modal::openChatMoreMenu(POINT{ 1050, 100 });
        freezeTween(modal::g_chat_more.t);
        paintFrame();
        // 菜单外的远角:必须 blocksDrag=true(否则拖窗吞点击,菜单关不掉)
        check("nchit.menu_open_blocks_drag_outside", g_overlays.pointBlocksDrag(far_corner));
        // 菜单内的点:也应 blocksDrag=true
        check("nchit.menu_open_blocks_drag_inside", g_overlays.pointBlocksDrag(POINT{ 960, 130 }));
    }
    {
        // 无浮层时空白仍可拖窗(不能因为修复把整窗永久锁死)
        resetOverlays();
        stages::g_view = stages::View::Home;
        paintFrame();
        check("nchit.no_overlay_bg_draggable", !g_overlays.pointBlocksDrag(far_corner));
    }
    // Bug A 真因:纯文本消息行只进 g_msg_row_hits、不进 g_hits → hoverInteractive 判
    // HTCAPTION → 右键落拖窗、收不到 WM_RBUTTONDOWN。修复:chat 消息流区域 = HTCLIENT。
    {
        {
            std::lock_guard<std::mutex> lk(chat::g_streams_mtx);
            auto& msgs = chat::g_streams[L"general"];
            msgs.clear();
            chat::Msg m; m.kind = chat::MsgKind::Text; m.from = L"peer";
            m.author = L"alice"; m.author_key = L"alice"; m.peer_key = L"alice";
            m.body = L"plain text"; m.time = L"12:00";
            m.send_state = chat::MsgSendState::Sent; m.server_id = 2001;
            msgs.push_back(m);
        }
        resetOverlays();
        stages::g_view = stages::View::Chat;
        chat::g_active = L"general";
        paintFrame();
        // 消息流区域内一点必须判为 stream(→ NCHITTEST HTCLIENT → 右键可达)
        POINT sp{ (LONG)(chat::g_chat_stream_rect.x + chat::g_chat_stream_rect.w * 0.5f),
                  (LONG)(chat::g_chat_stream_rect.y + chat::g_chat_stream_rect.h * 0.5f) };
        check("nchit.chat_stream_is_client", chat::pointInStream(sp));
        // 顶栏区域(y<48)不应算 stream(留给拖窗)
        check("nchit.topbar_not_stream", !chat::pointInStream(POINT{ 550, 20 }));
    }

    // ===== 本会话新特性回归:附件托盘(C)/ picker 键盘导航(D)/ emoji 搜索(E)=====
    trace("feat.enter");
    // --- C:附件暂存区改变 composer 高度 + 发送清空 ---
    {
        resetOverlays();
        stages::g_view = stages::View::Chat;
        chat::g_active = L"general";
        chat::g_composer.reset();
        chat::g_composer_attachments.clear();
        paintFrame();
        // 直接塞一个暂存附件(绕过写权限门控,测的是托盘+退格删除逻辑本身)
        chat::g_composer_attachments.push_back({ L"C:/nonexistent/x.png", "image" });
        check("attach.tray_has_chip", chat::g_composer_attachments.size() == 1);
        // Backspace 在文本最前 + 有附件 → 删末尾附件(仅当频道可写才走 onChar;否则跳过)
        chat::g_focus_composer = true;
        chat::onChar(nullptr, 0x08, false);
        if (chat::canWriteActiveChannel()) {
            check("attach.backspace_removes_chip", chat::g_composer_attachments.empty());
        } else {
            check("attach.backspace_removes_chip (skipped: channel not writable)", true);
            chat::g_composer_attachments.clear();
        }
    }
    // --- E:emoji 搜索过滤(kEmojiKw 关键词子串 AND)---
    {
        int all = chat::emojiCount();
        check("search.empty_query_returns_all",
              (int)chat::filteredEmojiIndices(L"").size() == all);
        auto heart = chat::filteredEmojiIndices(L"heart");
        check("search.heart_narrows", !heart.empty() && (int)heart.size() < all);
        // 多词 AND:两个词都命中才算(结果 ≤ 单词)
        auto two = chat::filteredEmojiIndices(L"laugh cry");
        check("search.multiword_and_subset", two.size() <= chat::filteredEmojiIndices(L"laugh").size());
        // 无命中串 → 空;中文查询(非 ascii)→ 空(关键词是英文)
        check("search.nomatch_empty", chat::filteredEmojiIndices(L"zzqzzq_nomatch").empty());
        check("search.nonascii_empty", chat::filteredEmojiIndices(L"表情").empty());
    }
    // --- E:打开 picker 即聚焦搜索框;打字进搜索框且不漏给 composer ---
    {
        resetOverlays();
        stages::g_view = stages::View::Chat;
        chat::g_active = L"general";
        chat::g_focus_composer = true;
        chat::g_composer.reset();
        chat::setPickerOpen(true);
        freezeTween(chat::g_picker_t);
        paintFrame();
        check("search.focused_on_open", chat::g_picker_search_focus);
        // 模拟键入 'h' 'e' → 进搜索框,不进 composer 文本
        bool consumed = chat::onPickerChar(nullptr, L'h', false)
                     && chat::onPickerChar(nullptr, L'e', false);
        check("search.char_consumed_by_picker", consumed);
        check("search.char_into_searchbox", chat::g_picker_search.text == L"he");
        check("search.char_not_leaked_to_composer", chat::g_composer.text.empty());
        // 关闭 picker 清空搜索
        chat::setPickerOpen(false);
        check("search.cleared_on_close", chat::g_picker_search.text.empty());
    }
    // --- D:picker 方向键导航设置选中格(在过滤结果范围内)---
    {
        resetOverlays();
        stages::g_view = stages::View::Chat;
        chat::g_active = L"general";
        chat::setPickerOpen(true);
        freezeTween(chat::g_picker_t);
        paintFrame();
        chat::g_picker_sel_idx = -1;
        chat::onPickerKey(nullptr, VK_DOWN, false, false);   // 首次方向键 → 选中首格(0)
        check("picker.nav_selects_first", chat::g_picker_sel_idx == 0);
        chat::onPickerKey(nullptr, VK_RIGHT, false, false);
        check("picker.nav_right_advances", chat::g_picker_sel_idx == 1);
        chat::setPickerOpen(false);
    }

    resetOverlays();
    if (f) { fprintf(f, "TOTAL_FAILS %d\n", fails); fclose(f); }
    return fails;
}

//  run() — 驱动全部 scenario，逐屏截图。见 PLAN-scenario_matrix。
// ----------------------------------------------------------------------------
int run(D2DApp& app, HWND hwnd) {
    const std::wstring dir = outputDir();

    // 目标几何 1100x720 dip → 物理 px（app.dpi()）。SetWindowPos + requestResize，
    // 下次 beginFrame 里 doResize 生效，widthDip()/heightDip() 就是 1100/720。
    // 不 ShowWindow —— GetBuffer(0) 在隐藏窗口下照常可用（DComp target 已在 init 绑定）。
    int physW = (int)(1100.0f * app.dpi() / 96.0f + 0.5f);
    int physH = (int)(720.0f  * app.dpi() / 96.0f + 0.5f);
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    SetWindowPos(hwnd, nullptr, (sw - physW) / 2, (sh - physH) / 2, physW, physH,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    app.requestResize(physW, physH);

    seedFixtures();
    freezeToMain();

    int failures = 0;
    // shot: 应用完 seam 后跑一帧并截图。paint 是冻结全局的纯函数 → 确定性。
    // Wave1 起解码走后台线程 —— smoke 无消息泵，故这里手动 settle：先画一帧让 paint
    // 入队解码，再有界地 Sleep + 让 cache 自己 drainCompleted，直到连续两次无新完成，
    // 最后一帧才截图。保证 cover/头像等异步图在截图里是真图而非占位。
    auto shot = [&](const std::wstring& name) {
        if (app.beginFrame()) { stages::paint(app); app.endFrame(); }   // 入队解码
        int quiet = 0;
        for (int i = 0; i < 40 && quiet < 2; ++i) {                     // 上限 ~600ms
            Sleep(15);
            size_t before = app.images().cacheSize() + app.gifs().cacheSize();
            app.images().drainCompleted();
            app.gifs().drainCompleted();
            size_t after = app.images().cacheSize() + app.gifs().cacheSize();
            // 再画一帧：可能触发新一批 miss→enqueue（如 cover 尺寸到位后的 reflow）。
            if (app.beginFrame()) { stages::paint(app); app.endFrame(); }
            quiet = (after == before) ? quiet + 1 : 0;
        }
        if (app.beginFrame()) {
            stages::paint(app);
            if (!capture(app, dir + L"\\" + name)) ++failures;
            app.endFrame();
        } else {
            ++failures;
        }
    };

    // ---- 1/2: Auth login / register（Main 之前，走 auth::paintAuthView）----
    // 强制 auth 卡片入场 tween 完成，否则 op<=0.001 直接 return（auth.cpp:170）。
    freezeTween(stages::g_auth_card_op);
    stages::g_auth_card_y.start(0, 0, 0.001f);   // 卡片 y 偏移归零
    stages::g_stage = stages::Stage::Auth;
    stages::g_time_in_stage = 0.0f;
    stages::g_auth_mode = stages::AuthMode::Login;
    shot(L"01_auth_login.png");
    stages::g_auth_mode = stages::AuthMode::Register;
    shot(L"02_auth_register.png");

    // ---- 3..9: 主壳内各 view ----
    freezeToMain();
    resetOverlays();
    stages::g_view = stages::View::Home;      shot(L"03_home.png");
    stages::g_view = stages::View::Lunching;  shot(L"04_lunching.png");
    stages::g_view = stages::View::Chat;      shot(L"05_chat.png");
    stages::g_view = stages::View::Market;    shot(L"06_market.png");
    stages::g_view = stages::View::Settings;  shot(L"07_settings.png");
    stages::g_view = stages::View::Profile;   shot(L"08_profile.png");
    stages::g_view = stages::View::Cloud;     shot(L"09_cloud.png");

    // ---- 10: 账户下拉浮层（基于 Home）----
    stages::g_view = stages::View::Home;
    resetOverlays();
    ui::g_account_dropdown = true;
    freezeTween(ui::g_dropdown_t);
    shot(L"10_dropdown.png");

    // ---- 11..16: 可达 modal（基于 Home，.open=true + .t 强制完成）----
    resetOverlays();
    modal::g_edit_status.open = true;  freezeTween(modal::g_edit_status.t);
    shot(L"11_modal_editstatus.png");

    resetOverlays();
    modal::g_edit_bio.open = true;     freezeTween(modal::g_edit_bio.t);
    shot(L"12_modal_editbio.png");

    resetOverlays();
    modal::openUserProfile(L"me");     // self 快照，避免触发网络 peerProfile
    freezeTween(modal::g_user_profile.t);
    shot(L"13_modal_userprofile.png");

    resetOverlays();
    modal::g_change_pw.open = true;    freezeTween(modal::g_change_pw.t);
    shot(L"14_modal_changepw.png");

    resetOverlays();
    modal::openConfirm(L"Confirm", L"Visual-smoke confirm dialog fixture.", nullptr);
    freezeTween(modal::g_confirm.t);
    shot(L"15_modal_confirm.png");

    resetOverlays();
    modal::g_search.open = true;       freezeTween(modal::g_search.t);
    shot(L"16_modal_search.png");

    // ---- 17: 表情/贴纸选择器 overlay（基于 Chat）----
    resetOverlays();
    stages::g_view = stages::View::Chat;
    chat::g_picker_open = true;
    freezeTween(chat::g_picker_t);
    shot(L"17_overlay_picker.png");

    // ---- 交互冒烟：程序化模拟点击，验证「点模态外关闭 / 点内不关」等逻辑。----
    // 走真实代码路径(paint 建立 hit + 重建浮层栈 g_overlays → modal::onMouseLDown)。
    failures += runInteractionTests(app);

    return failures == 0 ? 0 : 2;
}

}  // namespace launcher::d2d::visual_smoke

#endif  // LAUNCHER_VISUAL_SMOKE
