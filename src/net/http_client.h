#pragma once

// libcurl 封装。Phase 7 接 Ed25519 验签 + AES-GCM。
// 当前只暴露 GET，POST/header/超时后续扩展。

#include "app/common.h"
#include <vector>

namespace launcher::net {

struct HttpResponse {
    long status{0};
    std::vector<u8> body;
};

class HttpClient {
public:
    HttpClient();
    ~HttpClient();
    LAUNCHER_DISALLOW_COPY(HttpClient);

    Result<HttpResponse> get(const std::string& url, int timeout_ms = 10'000);

private:
    void* m_curl{nullptr};   // CURL*，避免在头里 #include curl.h
};

}  // namespace launcher::net
