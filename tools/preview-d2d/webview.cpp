// WebView2 实现 — 详见 webview.h。

#include "webview.h"

// 用 dynamic loading 调 WebView2Loader.dll → CreateCoreWebView2EnvironmentWithOptions
// 这样即使 runtime 没装 exe 启动不会 import 失败（fallback 路径走）。

#include <objbase.h>
#include <wrl.h>
#include <ShlObj.h>
#include "../../third_party/webview2/build/native/include/WebView2.h"

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")

using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Callback;

namespace launcher::d2d::webview {

namespace {

ComPtr<ICoreWebView2Environment> g_env;
ComPtr<ICoreWebView2Controller>  g_ctrl;
ComPtr<ICoreWebView2>            g_view;
HWND g_parent = nullptr;
bool g_initing = false;
std::wstring g_pending_url;          // 在 controller 就绪前 navigate 暂存
RECT g_pending_bounds{ 0, 0, 0, 0 };
bool g_pending_visible = false;

// Loader DLL helpers — runtime 没装时 LoadLibrary 失败
using FnCreateEnv = HRESULT (STDMETHODCALLTYPE*)(
    PCWSTR browserExecutableFolder,
    PCWSTR userDataFolder,
    ICoreWebView2EnvironmentOptions* environmentOptions,
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler* env_handler);

FnCreateEnv resolveCreateEnv() {
    static FnCreateEnv fn = nullptr;
    static bool tried = false;
    if (tried) return fn;
    tried = true;
    HMODULE m = LoadLibraryW(L"WebView2Loader.dll");
    if (!m) return nullptr;
    fn = (FnCreateEnv)GetProcAddress(m, "CreateCoreWebView2EnvironmentWithOptions");
    return fn;
}

std::wstring userDataDir() {
    wchar_t base[MAX_PATH] = {0};
    if (!SHGetSpecialFolderPathW(nullptr, base, CSIDL_LOCAL_APPDATA, FALSE)) return L".";
    std::wstring dir = std::wstring(base) + L"\\Launcher\\webview2";
    CreateDirectoryW((std::wstring(base) + L"\\Launcher").c_str(), nullptr);
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

extern std::wstring g_pending_html;

void applyPendingState() {
    if (!g_ctrl) return;
    if (g_pending_bounds.right > g_pending_bounds.left) {
        g_ctrl->put_Bounds(g_pending_bounds);
    }
    g_ctrl->put_IsVisible(g_pending_visible ? TRUE : FALSE);
    if (!g_pending_html.empty() && g_view) {
        g_view->NavigateToString(g_pending_html.c_str());
        g_pending_html.clear();
        g_pending_url.clear();
    } else if (!g_pending_url.empty() && g_view) {
        g_view->Navigate(g_pending_url.c_str());
        g_pending_url.clear();
    }
}

}  // anon

bool ensure(HWND parent) {
    if (g_env || g_initing) {
        g_parent = parent;
        return true;
    }
    auto fn = resolveCreateEnv();
    if (!fn) return false;          // runtime 没装；CS2 modal fallback ShellExecute

    g_parent = parent;
    g_initing = true;

    std::wstring data_dir = userDataDir();

    HRESULT hr = fn(
        nullptr, data_dir.c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [parent](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
                if (FAILED(result) || !env) {
                    g_initing = false;
                    return result;
                }
                g_env = env;
                env->CreateCoreWebView2Controller(parent,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [](HRESULT r2, ICoreWebView2Controller* ctrl) -> HRESULT {
                            g_initing = false;
                            if (FAILED(r2) || !ctrl) return r2;
                            g_ctrl = ctrl;
                            ctrl->get_CoreWebView2(&g_view);
                            // 默认隐藏 + 设到目前 pending bounds
                            applyPendingState();
                            return S_OK;
                        }).Get());
                return S_OK;
            }).Get());
    if (FAILED(hr)) {
        g_initing = false;
        return false;
    }
    return true;
}

bool isReady() { return g_view != nullptr; }

void setBounds(int left, int top, int right, int bottom) {
    g_pending_bounds = { left, top, right, bottom };
    if (g_ctrl) g_ctrl->put_Bounds(g_pending_bounds);
}

void navigate(const std::wstring& url) {
    g_pending_url = url;
    if (g_view) {
        g_view->Navigate(url.c_str());
        g_pending_url.clear();
    }
}

namespace { std::wstring g_pending_html; }

void navigateHtml(const std::wstring& html) {
    g_pending_html = html;
    if (g_view) {
        g_view->NavigateToString(html.c_str());
        g_pending_html.clear();
    }
}

bool runtimeAvailable() {
    return resolveCreateEnv() != nullptr;
}

void show(bool visible) {
    g_pending_visible = visible;
    if (g_ctrl) g_ctrl->put_IsVisible(visible ? TRUE : FALSE);
}

}  // namespace launcher::d2d::webview
