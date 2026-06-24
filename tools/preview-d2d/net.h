// WinHTTP 客户端。
#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <winhttp.h>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include <functional>
#include <string>
#include <vector>
#include <utility>
#include <memory>

#pragma comment(lib, "winhttp.lib")

namespace launcher::d2d::net {

constexpr const wchar_t* kDefaultHost = L"154.40.36.22";
constexpr INTERNET_PORT kDefaultPort = 1337;
constexpr const wchar_t* kUserAgent = L"Launcher-D2D/0.1";

struct Resp {
    DWORD status = 0;
    std::string body;
    bool ok() const { return status >= 200 && status < 300; }
};

struct Endpoint {
    std::wstring host = kDefaultHost;
    INTERNET_PORT port = kDefaultPort;
    bool secure = true;
    bool allow_insecure_tls = false;
};

inline bool envTruthy(const wchar_t* value) {
    return wcscmp(value, L"1") == 0
        || _wcsicmp(value, L"true") == 0
        || _wcsicmp(value, L"yes") == 0
        || _wcsicmp(value, L"on") == 0;
}

inline Endpoint endpoint() {
    static Endpoint ep = []() {
        Endpoint out;
        wchar_t host[256]{};
        DWORD hn = GetEnvironmentVariableW(L"LAUNCHER_API_HOST", host, (DWORD)(sizeof(host) / sizeof(host[0])));
        if (hn > 0 && hn < (sizeof(host) / sizeof(host[0]))) out.host = host;
        wchar_t port_s[16]{};
        DWORD pn = GetEnvironmentVariableW(L"LAUNCHER_API_PORT", port_s, (DWORD)(sizeof(port_s) / sizeof(port_s[0])));
        if (pn > 0 && pn < (sizeof(port_s) / sizeof(port_s[0]))) {
            int p = _wtoi(port_s);
            if (p > 0 && p <= 65535) out.port = (INTERNET_PORT)p;
        }
        wchar_t scheme[16]{};
        DWORD sn = GetEnvironmentVariableW(L"LAUNCHER_API_SCHEME", scheme, (DWORD)(sizeof(scheme) / sizeof(scheme[0])));
        if (sn > 0 && _wcsicmp(scheme, L"http") == 0) out.secure = false;
        wchar_t insecure[16]{};
        DWORD in = GetEnvironmentVariableW(L"LAUNCHER_ALLOW_INSECURE_TLS", insecure, (DWORD)(sizeof(insecure) / sizeof(insecure[0])));
        if (in > 0 && in < (sizeof(insecure) / sizeof(insecure[0]))) {
            out.allow_insecure_tls = envTruthy(insecure);
        } else if (out.secure && _wcsicmp(out.host.c_str(), kDefaultHost) == 0 && out.port == kDefaultPort) {
            out.allow_insecure_tls = true;
        }
        return out;
    }();
    return ep;
}

inline void maybeAllowInsecureTls(HINTERNET req) {
    if (!endpoint().allow_insecure_tls) return;
    DWORD opts = SECURITY_FLAG_IGNORE_UNKNOWN_CA
               | SECURITY_FLAG_IGNORE_CERT_DATE_INVALID
               | SECURITY_FLAG_IGNORE_CERT_CN_INVALID
               | SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
    WinHttpSetOption(req, WINHTTP_OPTION_SECURITY_FLAGS, &opts, sizeof(opts));
}

inline int jsonHex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

inline void appendUtf8(std::string& out, uint32_t cp) {
    if (cp <= 0x7Fu) {
        out.push_back((char)cp);
    } else if (cp <= 0x7FFu) {
        out.push_back((char)(0xC0u | (cp >> 6)));
        out.push_back((char)(0x80u | (cp & 0x3Fu)));
    } else if (cp <= 0xFFFFu) {
        out.push_back((char)(0xE0u | (cp >> 12)));
        out.push_back((char)(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back((char)(0x80u | (cp & 0x3Fu)));
    } else if (cp <= 0x10FFFFu) {
        out.push_back((char)(0xF0u | (cp >> 18)));
        out.push_back((char)(0x80u | ((cp >> 12) & 0x3Fu)));
        out.push_back((char)(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back((char)(0x80u | (cp & 0x3Fu)));
    }
}

inline bool readJsonU16(const std::string& body, size_t p, uint32_t& out) {
    if (p + 4 > body.size()) return false;
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
        int h = jsonHex(body[p + i]);
        if (h < 0) return false;
        v = (v << 4) | (uint32_t)h;
    }
    out = v;
    return true;
}

inline bool parseJsonStringAt(const std::string& body, size_t quote, std::string& out, size_t* end_pos = nullptr) {
    out.clear();
    if (quote >= body.size() || body[quote] != '"') return false;
    size_t p = quote + 1;
    while (p < body.size()) {
        char c = body[p++];
        if (c == '"') {
            if (end_pos) *end_pos = p;
            return true;
        }
        if (c != '\\') {
            out.push_back(c);
            continue;
        }
        if (p >= body.size()) return false;
        char e = body[p++];
        switch (e) {
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'u': {
                uint32_t cp = 0;
                if (!readJsonU16(body, p, cp)) return false;
                p += 4;
                if (cp >= 0xD800u && cp <= 0xDBFFu
                    && p + 6 <= body.size()
                    && body[p] == '\\' && body[p + 1] == 'u') {
                    uint32_t low = 0;
                    if (readJsonU16(body, p + 2, low) && low >= 0xDC00u && low <= 0xDFFFu) {
                        cp = 0x10000u + (((cp - 0xD800u) << 10) | (low - 0xDC00u));
                        p += 6;
                    }
                }
                appendUtf8(out, cp);
                break;
            }
            default:
                out.push_back(e);
                break;
        }
    }
    return false;
}

inline size_t findJsonObjectEnd(const std::string& body, size_t open_pos) {
    if (open_pos >= body.size() || body[open_pos] != '{') return std::string::npos;
    int depth = 1;
    bool in_str = false;
    bool esc = false;
    for (size_t p = open_pos + 1; p < body.size(); ++p) {
        char c = body[p];
        if (in_str) {
            if (esc) esc = false;
            else if (c == '\\') esc = true;
            else if (c == '"') in_str = false;
            continue;
        }
        if (c == '"') in_str = true;
        else if (c == '{') ++depth;
        else if (c == '}') {
            if (--depth == 0) return p;
        }
    }
    return std::string::npos;
}

inline size_t findJsonArrayEnd(const std::string& body, size_t open_pos) {
    if (open_pos >= body.size() || body[open_pos] != '[') return std::string::npos;
    int depth = 1;
    bool in_str = false;
    bool esc = false;
    for (size_t p = open_pos + 1; p < body.size(); ++p) {
        char c = body[p];
        if (in_str) {
            if (esc) esc = false;
            else if (c == '\\') esc = true;
            else if (c == '"') in_str = false;
            continue;
        }
        if (c == '"') in_str = true;
        else if (c == '[') ++depth;
        else if (c == ']') {
            if (--depth == 0) return p;
        }
    }
    return std::string::npos;
}

inline bool jsonFinishValueRange(const std::string& body, size_t value_begin, size_t& end) {
    if (value_begin >= body.size()) return false;
    if (body[value_begin] == '"') {
        std::string ignored;
        size_t ep = value_begin;
        if (!parseJsonStringAt(body, value_begin, ignored, &ep)) return false;
        end = ep;
        return true;
    }
    if (body[value_begin] == '{') {
        size_t ep = findJsonObjectEnd(body, value_begin);
        if (ep == std::string::npos) return false;
        end = ep + 1;
        return true;
    }
    if (body[value_begin] == '[') {
        size_t ep = findJsonArrayEnd(body, value_begin);
        if (ep == std::string::npos) return false;
        end = ep + 1;
        return true;
    }
    size_t ep = value_begin;
    while (ep < body.size() && body[ep] != ',' && body[ep] != '}' && body[ep] != ']'
           && body[ep] != '\r' && body[ep] != '\n') {
        ++ep;
    }
    while (ep > value_begin && (body[ep - 1] == ' ' || body[ep - 1] == '\t')) --ep;
    end = ep;
    return true;
}

inline bool jsonValueRange(const std::string& body, const char* field, size_t& begin, size_t& end) {
    begin = 0;
    end = 0;
    if (!field || !*field) return false;
    size_t root = 0;
    while (root < body.size()
           && (body[root] == ' ' || body[root] == '\t' || body[root] == '\r' || body[root] == '\n')) {
        ++root;
    }
    if (root < body.size() && body[root] == '{') {
        size_t p = root + 1;
        while (p < body.size()) {
            while (p < body.size()
                   && (body[p] == ' ' || body[p] == '\t' || body[p] == '\r' || body[p] == '\n')) {
                ++p;
            }
            if (p >= body.size() || body[p] == '}') return false;
            if (body[p] != '"') return false;

            std::string key;
            size_t key_end = p;
            if (!parseJsonStringAt(body, p, key, &key_end)) return false;
            p = key_end;
            while (p < body.size()
                   && (body[p] == ' ' || body[p] == '\t' || body[p] == '\r' || body[p] == '\n')) {
                ++p;
            }
            if (p >= body.size() || body[p] != ':') return false;
            ++p;
            while (p < body.size()
                   && (body[p] == ' ' || body[p] == '\t' || body[p] == '\r' || body[p] == '\n')) {
                ++p;
            }
            size_t value_begin = p;
            size_t value_end = p;
            if (!jsonFinishValueRange(body, value_begin, value_end)) return false;
            if (key == field) {
                begin = value_begin;
                end = value_end;
                return true;
            }
            p = value_end;
            while (p < body.size()
                   && (body[p] == ' ' || body[p] == '\t' || body[p] == '\r' || body[p] == '\n')) {
                ++p;
            }
            if (p < body.size() && body[p] == ',') {
                ++p;
                continue;
            }
            if (p < body.size() && body[p] == '}') return false;
            return false;
        }
        return false;
    }

    std::string key = "\""; key += field; key += "\":";
    auto p = body.find(key);
    if (p == std::string::npos) return false;
    p += key.size();
    while (p < body.size() && (body[p] == ' ' || body[p] == '\t' || body[p] == '\r' || body[p] == '\n')) ++p;
    if (p >= body.size()) return false;
    begin = p;
    return jsonFinishValueRange(body, p, end);
}

inline std::string jsonRaw(const std::string& body, const char* field) {
    size_t b = 0, e = 0;
    if (!jsonValueRange(body, field, b, e) || e <= b) return "";
    return body.substr(b, e - b);
}

inline std::string jsonObject(const std::string& body, const char* field) {
    size_t b = 0, e = 0;
    if (!jsonValueRange(body, field, b, e) || e <= b || body[b] != '{') return "";
    return body.substr(b, e - b);
}

inline std::string jsonStr(const std::string& body, const char* field) {
    size_t b = 0, e = 0;
    if (!jsonValueRange(body, field, b, e) || e <= b) return "";
    if (body.compare(b, 4, "null") == 0) return "";
    if (body[b] != '"') return "";
    std::string out;
    parseJsonStringAt(body, b, out);
    return out;
}
inline long long jsonInt(const std::string& body, const char* field) {
    size_t b = 0, e = 0;
    if (!jsonValueRange(body, field, b, e) || e <= b) return 0;
    if (body[b] == '"') {
        std::string s;
        if (parseJsonStringAt(body, b, s)) return _atoi64(s.c_str());
        return 0;
    }
    return _atoi64(body.c_str() + b);
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
    Endpoint ep = endpoint();
    HINTERNET con = WinHttpConnect(ses, ep.host.c_str(), ep.port, 0);
    if (!con) return r;
    HINTERNET req = WinHttpOpenRequest(con, verb, path, nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, ep.secure ? WINHTTP_FLAG_SECURE : 0);
    if (!req) { WinHttpCloseHandle(con); return r; }

    maybeAllowInsecureTls(req);

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
        maybeAllowInsecureTls(req);
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
        Endpoint ep = endpoint();
        h_con = WinHttpConnect(ses, ep.host.c_str(), ep.port, 0);
        if (!h_con) return false;
        h_req = WinHttpOpenRequest(h_con, L"GET", path.c_str(), nullptr,
            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, ep.secure ? WINHTTP_FLAG_SECURE : 0);
        if (!h_req) { WinHttpCloseHandle(h_con); h_con = nullptr; return false; }

        maybeAllowInsecureTls(h_req);
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
