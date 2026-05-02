#include "net/http_client.h"
#include <curl/curl.h>

namespace launcher::net {

namespace {
size_t writeCb(char* p, size_t sz, size_t n, void* ud) {
    auto* v = static_cast<std::vector<u8>*>(ud);
    size_t total = sz * n;
    v->insert(v->end(), p, p + total);
    return total;
}
}  // namespace

HttpClient::HttpClient() {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    m_curl = curl_easy_init();
}

HttpClient::~HttpClient() {
    if (m_curl) curl_easy_cleanup(static_cast<CURL*>(m_curl));
    curl_global_cleanup();
}

Result<HttpResponse> HttpClient::get(const std::string& url, int timeout_ms) {
    if (!m_curl) return { {}, 1 };
    CURL* c = static_cast<CURL*>(m_curl);
    curl_easy_reset(c);

    HttpResponse r;
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, static_cast<long>(timeout_ms));
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, writeCb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &r.body);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "Launcher/0.1 (+windows)");
    curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");

    // Phase 7：补 CURLOPT_PINNEDPUBLICKEY 做证书 pinning
    CURLcode rc = curl_easy_perform(c);
    if (rc != CURLE_OK) return { {}, static_cast<int>(rc) };

    long code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    r.status = code;
    return { std::move(r), 0 };
}

}  // namespace launcher::net
