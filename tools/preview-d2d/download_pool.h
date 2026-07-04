// 固定线程数的媒体下载池 — Wave2 下载侧。
// 复用 fetch::downloadMediaToPath（自带磁盘缓存短路，见 fetch.cpp:905），
// 用 N 个后台 std::thread 消费一个作业队列；以 local_path 为键做去重合并：
// 同一 local_path 的重复 enqueue 不会再起一次下载，只把新的通知目标挂到
// 已在途的作业上，下载完成后一次性 PostMessage 通知所有等待方。
#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace launcher::d2d {

class DownloadPool {
public:
    static DownloadPool& instance();

    // 起 n 个 worker（默认 4）。已启动则忽略后续调用。
    void start(int n = 4);

    // 停机：置停止标志、唤醒并 join 所有 worker。可重复调用。
    void stop();

    // 入队一个下载。url 为 /api/media URL（免带 session_token，下载时自动补），
    // local_path 为目标缓存路径。完成后 PostMessage(notify, msg, wParam=ok, 0)。
    // 若 local_path 已在途，仅把 (notify,msg) 追加为等待方，不再起第二次下载。
    void enqueue(const std::wstring& url, const std::wstring& local_path,
                 HWND notify, UINT msg);

    DownloadPool(const DownloadPool&) = delete;
    DownloadPool& operator=(const DownloadPool&) = delete;

private:
    DownloadPool() = default;
    ~DownloadPool();

    struct Waiter {
        HWND notify;
        UINT msg;
    };
    struct Job {
        std::wstring url_or_media_url;
        std::vector<Waiter> waiters;
    };

    void workerLoop();

    std::mutex mtx_;
    std::condition_variable cv_;
    std::deque<std::wstring> queue_;                  // 待处理的 local_path，FIFO
    std::unordered_map<std::wstring, Job> jobs_;      // local_path -> 在途/排队作业
    std::vector<std::thread> workers_;
    bool started_ = false;
    bool stopping_ = false;
};

}  // namespace launcher::d2d
