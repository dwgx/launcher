// DecodeService — 把所有 WIC 解码搬离 paint 线程（见 tmp/image-opt/PLAN-client_design.md §1）。
//
// 进程级单例，持 1-2 个 std::thread；每个 worker 自己 CoInitializeEx(MTA) + 自己的
// IWICImagingFactory。管线（全部 off-thread）：
//   CreateDecoderFromFilename → GetFrame(0) → 读 intrinsic GetSize()
//   → IWICBitmapScaler->Initialize(frame, scaledW, scaledH, Fant)   （targetPx 长边，绝不放大）
//   → IWICFormatConverter->Initialize(scaler, PBGRA)
//   → wic->CreateBitmapFromSource(conv, WICBitmapCacheOnLoad) => ComPtr<IWICBitmap> CPU 位图
// 结果 {kind,key,iw,ih,frames} 推入 completed 队列（mutex）后 PostMessageW(kMsgDecodeReady)。
//
// 关键约束：ID2D1DeviceContext 是单线程的 —— CreateBitmapFromWicBitmap（GPU 上传）必须留在
// UI 线程（在 ImageCache/GifCache::drainCompleted 里做）。WIC 内存位图是 agile 的，跨线程
// 交接 IWICBitmap 安全（WICBitmapCacheOnLoad 已把像素完全物化）。
#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace launcher::d2d {

using Microsoft::WRL::ComPtr;

// UI 线程收到此消息 → g_app.images().drainCompleted() + g_app.gifs().drainCompleted()。
// WM_APP 段位占用：现有业务用到 +65，tray 用 +100，本消息取空位 +66。
constexpr UINT kMsgDecodeReady = WM_APP + 66;

enum class DecodeKind { Image, Gif };

// 缓存键 —— (path, targetPx)。同一文件的头像尺寸与全览尺寸是不同 key，可并存。
struct DecodeKey {
    std::wstring path;
    uint32_t     targetPx = 0;   // 0 = 原生尺寸（不缩放）
    bool operator==(const DecodeKey& o) const {
        return targetPx == o.targetPx && path == o.path;
    }
};
struct DecodeKeyHash {
    size_t operator()(const DecodeKey& k) const noexcept {
        size_t h = std::hash<std::wstring>{}(k.path);
        h ^= (size_t)k.targetPx * 0x9e3779b1u + (h << 6) + (h >> 2);
        return h;
    }
};

struct DecodedFrame {
    ComPtr<IWICBitmap> wic;        // agile CPU 位图；UI 线程 CreateBitmapFromWicBitmap
    uint32_t           delay_ms = 100;
    uint32_t           w = 0, h = 0;   // 缩放后（= 上传后的位图像素）
};

struct DecodeResult {
    DecodeKind               kind = DecodeKind::Image;
    DecodeKey                key;
    bool                     ok = false;
    uint32_t                 iw = 0, ih = 0;   // intrinsic（缩放前原始）尺寸
    std::vector<DecodedFrame> frames;          // Image: 1 帧；Gif: N 帧
};

class DecodeService {
public:
    // notify = 完成后 PostMessage 的目标窗口（UI 线程消息泵）。可重复调用（幂等）。
    void start(HWND notify);
    void stop();

    // 入队一个解码任务。调用方（cache）保证同一 key 只入队一次（miss→插 Pending→enqueue）。
    void enqueue(DecodeKind kind, const DecodeKey& key);

    // UI 线程取走已完成结果（清空内部队列）。kind 过滤：Image cache 只取 Image，反之亦然。
    std::vector<DecodeResult> takeCompleted(DecodeKind kind);

private:
    void workerLoop();
    DecodeResult decodeOne(IWICImagingFactory* wic, const DecodeKind kind,
                           const DecodeKey& key);

    struct Job { DecodeKind kind; DecodeKey key; };

    std::vector<std::thread>  workers_;
    std::deque<Job>           jobs_;
    std::mutex                jobs_mtx_;
    std::condition_variable   jobs_cv_;

    std::vector<DecodeResult> done_;
    std::mutex                done_mtx_;

    HWND  notify_ = nullptr;
    bool  running_ = false;
};

// 进程级单例。
DecodeService& decodeService();

}  // namespace launcher::d2d
