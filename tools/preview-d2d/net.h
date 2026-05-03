// WinHTTP 客户端 — 直接复制自 tools/preview/net.inl，不变。
// 后端 https://154.40.36.22:1337，LE IP cert，TLS skip-verify (prod 改 pinning)。
#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <winhttp.h>
#include <atomic>
#include <functional>
#include <string>
#include <vector>

#pragma comment(lib, "winhttp.lib")

namespace launcher::d2d::net {

constexpr const wchar_t* kHost = L"154.40.36.22";
constexpr INTERNET_PORT kPort = 1337;
constexpr const wchar_t* kUserAgent = L"Launcher-D2D/0.1";

struct Resp {
    DWORD status = 0;
    std::string body;
    bool ok() const { return status >= 200 && status < 300; }
};

inline std::string jsonStr(const std::string& body, const char* field) {
    std::string key = "\""; key += field; key += "\":\"";
    auto p = body.find(key);
    if (p == std::string::npos) return "";
    p += key.size();
    auto e = body.find('"', p);
    if (e == std::string::npos) return "";
    return body.substr(p, e - p);
}
inline long long jsonInt(const std::string& body, const char* field) {
    std::string key = "\""; key += field; key += "\":";
    auto p = body.find(key);
    if (p == std::string::npos) return 0;
    p += key.size();
    while (p < body.size() && (body[p] == ' ' || body[p] == '"')) p++;
    return _atoi64(body.c_str() + p);
}

inline HINTERNET sharedSession() {
    static HINTERNET s = []() -> HINTERNET {
        HINTERNET h = WinHttpOpen(kUserAgent,
            WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (h) {
            DWORD t_resolve = 5000, t_connect = 5000, t_send = 15000, t_recv = 30000;
            WinHttpSetTimeouts(h, t_resolve, t_connect, t_send, t_recv);
        }
        return h;
    }();
    return s;
}

inline Resp request(const wchar_t* verb, const wchar_t* path,
                    const std::string& body,
                    const std::wstring& content_type,
                    const std::wstring& extra_headers = L"") {
    Resp r;
    HINTERNET ses = sharedSession();
    if (!ses) return r;
    HINTERNET con = WinHttpConnect(ses, kHost, kPort, 0);
    if (!con) return r;
    HINTERNET req = WinHttpOpenRequest(con, verb, path, nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!req) { WinHttpCloseHandle(con); return r; }

    DWORD opts = SECURITY_FLAG_IGNORE_UNKNOWN_CA
               | SECURITY_FLAG_IGNORE_CERT_DATE_INVALID
               | SECURITY_FLAG_IGNORE_CERT_CN_INVALID
               | SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
    WinHttpSetOption(req, WINHTTP_OPTION_SECURITY_FLAGS, &opts, sizeof(opts));

    std::wstring headers;
    if (!content_type.empty()) headers += L"Content-Type: " + content_type + L"\r\n";
    headers += extra_headers;

    BOOL ok = WinHttpSendRequest(req,
        headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
        headers.empty() ? 0 : (DWORD)-1,
        body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)body.data(),
        (DWORD)body.size(), (DWORD)body.size(), 0);
    if (!ok) { WinHttpCloseHandle(req); WinHttpCloseHandle(con); return r; }

    if (!WinHttpReceiveResponse(req, nullptr)) {
        WinHttpCloseHandle(req); WinHttpCloseHandle(con); return r;
    }

    DWORD status = 0; DWORD szs = sizeof(status);
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        nullptr, &status, &szs, nullptr);
    r.status = status;

    DWORD avail = 0;
    while (WinHttpQueryDataAvailable(req, &avail) && avail > 0) {
        std::vector<char> buf(avail);
        DWORD nrd = 0;
        if (!WinHttpReadData(req, buf.data(), avail, &nrd)) break;
        r.body.append(buf.data(), nrd);
        if (nrd == 0) break;
    }

    WinHttpCloseHandle(req);
    WinHttpCloseHandle(con);
    return r;
}

inline Resp postJson(const wchar_t* path, const std::string& json,
                     const std::string& session_token = "") {
    std::wstring extra;
    if (!session_token.empty()) {
        extra = L"X-Session-Token: ";
        for (char c : session_token) extra.push_back((wchar_t)c);
        extra += L"\r\n";
    }
    return request(L"POST", path, json, L"application/json", extra);
}

inline std::string jsonEscape(const std::wstring& s) {
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return "";
    std::string utf8(n - 1, 0);
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, utf8.data(), n, nullptr, nullptr);
    std::string out;
    for (char c : utf8) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if ((unsigned char)c < 0x20) {
                    char buf[8]; sprintf_s(buf, "\\u%04x", c);
                    out += buf;
                } else out += c;
        }
    }
    return out;
}

inline Resp uploadMultipart(const wchar_t* path,
                            const std::string& session_token,
                            const std::wstring& field_name,
                            const std::wstring& filename,
                            const std::string& mime,
                            const std::vector<BYTE>& bytes) {
    const std::string boundary = "----LauncherMP-7Yk3";
    std::string body;
    body += "--" + boundary + "\r\n";
    body += "Content-Disposition: form-data; name=\"session_token\"\r\n\r\n";
    body += session_token + "\r\n";
    body += "--" + boundary + "\r\n";
    int fnn = WideCharToMultiByte(CP_UTF8, 0, filename.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string fname_utf8(fnn - 1, 0);
    WideCharToMultiByte(CP_UTF8, 0, filename.c_str(), -1, fname_utf8.data(), fnn, nullptr, nullptr);
    int fdn = WideCharToMultiByte(CP_UTF8, 0, field_name.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string field_utf8(fdn - 1, 0);
    WideCharToMultiByte(CP_UTF8, 0, field_name.c_str(), -1, field_utf8.data(), fdn, nullptr, nullptr);
    body += "Content-Disposition: form-data; name=\"" + field_utf8 + "\"; filename=\"" + fname_utf8 + "\"\r\n";
    body += "Content-Type: " + mime + "\r\n\r\n";
    body.append((const char*)bytes.data(), bytes.size());
    body += "\r\n--" + boundary + "--\r\n";

    std::wstring ct = L"multipart/form-data; boundary=";
    for (char c : boundary) ct.push_back((wchar_t)c);
    return request(L"POST", path, body, ct);
}

// 异步 helper —— 简化常见 fetch 模式：起线程跑 fn(arg) 然后 PostMessage 给 hwnd
template<class T>
inline void runAsync(HWND hwnd, UINT notify_msg, T arg, std::function<bool(const T&)> fn) {
    struct Pack { HWND h; UINT m; T arg; std::function<bool(const T&)> fn; };
    auto* p = new Pack{ hwnd, notify_msg, std::move(arg), std::move(fn) };
    CreateThread(nullptr, 0, [](LPVOID lp) -> DWORD {
        auto* p = (Pack*)lp;
        bool ok = p->fn(p->arg);
        PostMessageW(p->h, p->m, ok ? 1 : 0, 0);
        delete p;
        return 0;
    }, p, 0, nullptr);
}

}  // namespace launcher::d2d::net
