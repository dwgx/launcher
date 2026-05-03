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
#include <utility>
#include <memory>

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

// 任意 host 的简化 GET — 用于 IP 地理查询、CDN 等。
// secure=false 走 80 端口 HTTP，true 走 443 HTTPS（skip-verify）。
inline Resp requestAny(const wchar_t* host, INTERNET_PORT port,
                       const wchar_t* path, bool secure) {
    Resp r;
    HINTERNET ses = sharedSession();
    if (!ses) return r;
    HINTERNET con = WinHttpConnect(ses, host, port, 0);
    if (!con) return r;
    HINTERNET req = WinHttpOpenRequest(con, L"GET", path, nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        secure ? WINHTTP_FLAG_SECURE : 0);
    if (!req) { WinHttpCloseHandle(con); return r; }
    if (secure) {
        DWORD opts = SECURITY_FLAG_IGNORE_UNKNOWN_CA
                   | SECURITY_FLAG_IGNORE_CERT_DATE_INVALID
                   | SECURITY_FLAG_IGNORE_CERT_CN_INVALID
                   | SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
        WinHttpSetOption(req, WINHTTP_OPTION_SECURITY_FLAGS, &opts, sizeof(opts));
    }
    if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                             WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        WinHttpCloseHandle(req); WinHttpCloseHandle(con); return r;
    }
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

// ============= WebSocket 客户端 (WinHTTP) =============
// 用法：
//   net::WsClient ws;
//   ws.connect(L"/ws/chat?session_token=...", [](const std::string& body) {
//       // 后台线程被调用，body 是单条 message
//   });
struct WsClient {
    HINTERNET h_con{nullptr};
    HINTERNET h_req{nullptr};
    HINTERNET h_ws{nullptr};
    HANDLE    h_thread{nullptr};
    std::atomic<bool> stop{false};

    using OnMessage = std::function<void(const std::string&)>;

    bool connect(const std::wstring& path, OnMessage on_msg) {
        HINTERNET ses = sharedSession();
        if (!ses) return false;
        h_con = WinHttpConnect(ses, kHost, kPort, 0);
        if (!h_con) return false;
        h_req = WinHttpOpenRequest(h_con, L"GET", path.c_str(), nullptr,
            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        if (!h_req) { WinHttpCloseHandle(h_con); h_con = nullptr; return false; }

        DWORD opts = SECURITY_FLAG_IGNORE_UNKNOWN_CA
                   | SECURITY_FLAG_IGNORE_CERT_DATE_INVALID
                   | SECURITY_FLAG_IGNORE_CERT_CN_INVALID
                   | SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
        WinHttpSetOption(h_req, WINHTTP_OPTION_SECURITY_FLAGS, &opts, sizeof(opts));
        if (!WinHttpSetOption(h_req, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0)) {
            cleanup(); return false;
        }
        if (!WinHttpSendRequest(h_req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
            cleanup(); return false;
        }
        if (!WinHttpReceiveResponse(h_req, nullptr)) { cleanup(); return false; }
        DWORD status = 0; DWORD szs = sizeof(status);
        WinHttpQueryHeaders(h_req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            nullptr, &status, &szs, nullptr);
        if (status != 101) { cleanup(); return false; }
        h_ws = WinHttpWebSocketCompleteUpgrade(h_req, 0);
        if (!h_ws) { cleanup(); return false; }
        WinHttpCloseHandle(h_req); h_req = nullptr;

        struct Arg { WsClient* self; OnMessage cb; };
        Arg* a = new Arg{ this, std::move(on_msg) };
        h_thread = CreateThread(nullptr, 0, [](LPVOID lp) -> DWORD {
            std::unique_ptr<Arg> a((Arg*)lp);
            std::string accum;
            BYTE buf[4096];
            while (!a->self->stop.load()) {
                DWORD got = 0;
                WINHTTP_WEB_SOCKET_BUFFER_TYPE bt;
                DWORD st = WinHttpWebSocketReceive(a->self->h_ws, buf, sizeof(buf), &got, &bt);
                if (st != NO_ERROR) break;
                if (bt == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) break;
                if (bt == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE
                    || bt == WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE) {
                    accum.append((char*)buf, got);
                    continue;
                }
                if (bt == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE
                    || bt == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE) {
                    accum.append((char*)buf, got);
                    if (a->cb) a->cb(accum);
                    accum.clear();
                }
            }
            return 0;
        }, a, 0, nullptr);
        return h_thread != nullptr;
    }
    void close() {
        stop.store(true);
        if (h_ws) {
            WinHttpWebSocketClose(h_ws,
                WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
        }
        if (h_thread) {
            WaitForSingleObject(h_thread, 2000);
            CloseHandle(h_thread); h_thread = nullptr;
        }
        cleanup();
    }
    ~WsClient() { close(); }

private:
    void cleanup() {
        if (h_ws)  { WinHttpCloseHandle(h_ws);  h_ws  = nullptr; }
        if (h_req) { WinHttpCloseHandle(h_req); h_req = nullptr; }
        if (h_con) { WinHttpCloseHandle(h_con); h_con = nullptr; }
    }
};

}  // namespace launcher::d2d::net
