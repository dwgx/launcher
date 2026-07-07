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

#include <wrl/client.h>
#include <wincodec.h>
#include <shellapi.h>   // CommandLineToArgvW（--visual-smoke 命令行解析）
#include <string>
#include <vector>
#include <cstdio>

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

    modal::g_change_pw.open = false;    closeTween(modal::g_change_pw.t);
    modal::g_confirm.open = false;      closeTween(modal::g_confirm.t);
    modal::g_user_profile.open = false; closeTween(modal::g_user_profile.t);
    modal::g_edit_status.open = false;  closeTween(modal::g_edit_status.t);
    modal::g_edit_bio.open = false;     closeTween(modal::g_edit_bio.t);
    modal::g_search.open = false;       closeTween(modal::g_search.t);

    chat::g_picker_open = false;
    closeTween(chat::g_picker_t);
}

// ----------------------------------------------------------------------------
//  runInteractionTests() — 程序化交互冒烟。
//  真实代码路径:先 paint 一帧(注册 hit + 设 g_modal_hit_floor),再模拟点击调
//  modal::onMouseLDown(pt),然后断言状态。专测反复出问题的「点模态外关闭」逻辑。
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
    // paintFrame:跑一帧让当前状态注册 hit + 设 g_modal_hit_floor(paint 是纯函数)。
    auto paintFrame = [&]() {
        if (app.beginFrame()) { stages::paint(app); app.endFrame(); }
    };
    // 屏幕中心 & 远离任何模态的角落(用于「点模态外」)。1100x720 dip。
    const POINT center{ 550, 360 };
    const POINT far_corner{ 30, 700 };   // 左下角,任何居中模态都不覆盖

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
    // 走真实代码路径(paint 建立 hit + g_modal_hit_floor → modal::onMouseLDown)。
    failures += runInteractionTests(app);

    return failures == 0 ? 0 : 2;
}

}  // namespace launcher::d2d::visual_smoke

#endif  // LAUNCHER_VISUAL_SMOKE
