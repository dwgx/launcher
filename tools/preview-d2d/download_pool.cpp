// DownloadPool 实现 — 见 download_pool.h。
#include "download_pool.h"

#include <string>
#include <utility>

#include "fetch.h"

namespace launcher::d2d {

DownloadPool& DownloadPool::instance() {
    static DownloadPool pool;
    return pool;
}

DownloadPool::~DownloadPool() {
    stop();
}

void DownloadPool::start(int n) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (started_) return;
    if (n < 1) n = 1;
    stopping_ = false;
    started_ = true;
    workers_.reserve((size_t)n);
    for (int i = 0; i < n; ++i) {
        workers_.emplace_back([this] { workerLoop(); });
    }
}

void DownloadPool::stop() {
    std::vector<std::thread> to_join;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (!started_) return;
        stopping_ = true;
        to_join.swap(workers_);
    }
    cv_.notify_all();
    for (auto& t : to_join) {
        if (t.joinable()) t.join();
    }
    {
        std::lock_guard<std::mutex> lk(mtx_);
        started_ = false;
        queue_.clear();
        jobs_.clear();
    }
}

void DownloadPool::enqueue(const std::wstring& url, const std::wstring& local_path,
                           HWND notify, UINT msg) {
    if (local_path.empty()) return;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (stopping_) return;
        auto it = jobs_.find(local_path);
        if (it != jobs_.end()) {
            // 已在途/排队：合并通知目标，不再入队第二次下载。
            it->second.waiters.push_back(Waiter{ notify, msg });
            return;
        }
        Job job;
        job.url_or_media_url = url;
        job.waiters.push_back(Waiter{ notify, msg });
        jobs_.emplace(local_path, std::move(job));
        queue_.push_back(local_path);
    }
    cv_.notify_one();
}

void DownloadPool::workerLoop() {
    for (;;) {
        std::wstring local_path;
        std::wstring url;
        {
            std::unique_lock<std::mutex> lk(mtx_);
            cv_.wait(lk, [this] { return stopping_ || !queue_.empty(); });
            if (stopping_) return;
            local_path = std::move(queue_.front());
            queue_.pop_front();
            auto it = jobs_.find(local_path);
            if (it == jobs_.end()) continue;   // 理论上不会发生
            url = it->second.url_or_media_url;
        }

        // 下载在锁外进行；downloadMediaToPath 已带磁盘缓存短路（fetch.cpp:905）。
        std::string media_url;
        media_url.reserve(url.size());
        for (wchar_t c : url) media_url.push_back((char)(c & 0xFF));
        auto res = fetch::downloadMediaToPath(media_url, local_path);

        // 取出等待方并从在途表移除；此后同 path 的 enqueue 可再次触发下载。
        std::vector<Waiter> waiters;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            auto it = jobs_.find(local_path);
            if (it != jobs_.end()) {
                waiters.swap(it->second.waiters);
                jobs_.erase(it);
            }
        }
        for (const auto& w : waiters) {
            if (w.notify) {
                PostMessageW(w.notify, w.msg, res.ok ? 1u : 0u, 0);
            }
        }
    }
}

}  // namespace launcher::d2d
