// DecodeService 实现 — 见 decode_worker.h 顶部注释。
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "render/decode_worker.h"

#include <objbase.h>
#include <algorithm>

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

namespace launcher::d2d {

// worker 数：2 条足够喂满 UI 侧 GPU 上传，又不至于打爆磁盘/CPU。
static constexpr int kWorkerCount = 2;

DecodeService& decodeService() {
    static DecodeService svc;
    return svc;
}

void DecodeService::start(HWND notify) {
    std::lock_guard<std::mutex> lk(jobs_mtx_);
    notify_ = notify;
    if (running_) return;
    running_ = true;
    for (int i = 0; i < kWorkerCount; ++i)
        workers_.emplace_back([this] { workerLoop(); });
}

void DecodeService::stop() {
    {
        std::lock_guard<std::mutex> lk(jobs_mtx_);
        if (!running_) return;
        running_ = false;
    }
    jobs_cv_.notify_all();
    for (auto& t : workers_) if (t.joinable()) t.join();
    workers_.clear();
    std::lock_guard<std::mutex> lk(done_mtx_);
    done_.clear();
}

void DecodeService::enqueue(DecodeKind kind, const DecodeKey& key) {
    {
        std::lock_guard<std::mutex> lk(jobs_mtx_);
        if (!running_) return;
        jobs_.push_back(Job{ kind, key });
    }
    jobs_cv_.notify_one();
}

std::vector<DecodeResult> DecodeService::takeCompleted(DecodeKind kind) {
    std::vector<DecodeResult> out;
    std::lock_guard<std::mutex> lk(done_mtx_);
    for (auto it = done_.begin(); it != done_.end();) {
        if (it->kind == kind) {
            out.push_back(std::move(*it));
            it = done_.erase(it);
        } else {
            ++it;
        }
    }
    return out;
}

void DecodeService::workerLoop() {
    // 每个 worker 自己的 COM apartment（MTA）+ 自己的 WIC factory。
    HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ComPtr<IWICImagingFactory> wic;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                     IID_PPV_ARGS(&wic));

    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lk(jobs_mtx_);
            jobs_cv_.wait(lk, [this] { return !running_ || !jobs_.empty(); });
            if (!running_ && jobs_.empty()) break;
            job = jobs_.front();
            jobs_.pop_front();
        }

        DecodeResult res;
        if (wic) {
            res = decodeOne(wic.Get(), job.kind, job.key);
        } else {
            res.kind = job.kind;
            res.key  = job.key;
            res.ok   = false;
        }
        {
            std::lock_guard<std::mutex> lk(done_mtx_);
            done_.push_back(std::move(res));
        }
        if (notify_) PostMessageW(notify_, kMsgDecodeReady, 0, 0);
    }

    wic.Reset();
    if (SUCCEEDED(co)) CoUninitialize();
}

// 从一个已选帧生产出 targetPx-缩放后的 agile IWICBitmap（PBGRA premul）。
// scaled_out 回填缩放后的像素宽高。失败返回 nullptr。
static ComPtr<IWICBitmap> scaleAndConvert(IWICImagingFactory* wic,
                                          IWICBitmapFrameDecode* frame,
                                          uint32_t iw, uint32_t ih,
                                          uint32_t targetPx,
                                          uint32_t& out_w, uint32_t& out_h) {
    out_w = iw; out_h = ih;
    IWICBitmapSource* src = frame;  // 默认直接转换原帧

    ComPtr<IWICBitmapScaler> scaler;
    if (targetPx > 0 && iw > 0 && ih > 0) {
        uint32_t longest = (iw > ih) ? iw : ih;
        if (longest > targetPx) {
            // 长边缩到 targetPx；绝不放大。
            double s = (double)targetPx / (double)longest;
            uint32_t sw = (uint32_t)(iw * s + 0.5);
            uint32_t sh = (uint32_t)(ih * s + 0.5);
            if (sw < 1) sw = 1;
            if (sh < 1) sh = 1;
            if (SUCCEEDED(wic->CreateBitmapScaler(&scaler))
                && SUCCEEDED(scaler->Initialize(
                       frame, sw, sh, WICBitmapInterpolationModeFant))) {
                src = scaler.Get();
                out_w = sw; out_h = sh;
            }
        }
    }

    ComPtr<IWICFormatConverter> conv;
    if (FAILED(wic->CreateFormatConverter(&conv))) return {};
    if (FAILED(conv->Initialize(
            src, GUID_WICPixelFormat32bppPBGRA,
            WICBitmapDitherTypeNone, nullptr, 0.0,
            WICBitmapPaletteTypeMedianCut))) return {};

    // 物化成独立 CPU 位图（agile，可跨线程交给 UI）。
    ComPtr<IWICBitmap> out;
    if (FAILED(wic->CreateBitmapFromSource(
            conv.Get(), WICBitmapCacheOnLoad, &out))) return {};
    return out;
}

DecodeResult DecodeService::decodeOne(IWICImagingFactory* wic,
                                      const DecodeKind kind,
                                      const DecodeKey& key) {
    DecodeResult res;
    res.kind = kind;
    res.key  = key;

    ComPtr<IWICBitmapDecoder> dec;
    if (FAILED(wic->CreateDecoderFromFilename(
            key.path.c_str(), nullptr, GENERIC_READ,
            WICDecodeMetadataCacheOnDemand, &dec)))
        return res;  // ok=false

    UINT count = 0;
    if (FAILED(dec->GetFrameCount(&count)) || count == 0) return res;

    if (kind == DecodeKind::Image) count = 1;   // 静态图只要第 0 帧

    for (UINT i = 0; i < count; ++i) {
        ComPtr<IWICBitmapFrameDecode> frame;
        if (FAILED(dec->GetFrame(i, &frame))) continue;

        uint32_t iw = 0, ih = 0;
        frame->GetSize(&iw, &ih);
        if (i == 0) { res.iw = iw; res.ih = ih; }

        DecodedFrame f;
        f.wic = scaleAndConvert(wic, frame.Get(), iw, ih, key.targetPx, f.w, f.h);
        if (!f.wic) continue;

        // GIF 帧延迟（1/100s 单位；缺省 100ms）。静态图忽略。
        if (kind == DecodeKind::Gif) {
            UINT delay_centi = 10;
            ComPtr<IWICMetadataQueryReader> meta;
            if (SUCCEEDED(frame->GetMetadataQueryReader(&meta))) {
                PROPVARIANT pv; PropVariantInit(&pv);
                if (SUCCEEDED(meta->GetMetadataByName(L"/grctlext/Delay", &pv))
                    && pv.vt == VT_UI2 && pv.uiVal > 0) {
                    delay_centi = pv.uiVal;
                }
                PropVariantClear(&pv);
            }
            f.delay_ms = delay_centi * 10;
            if (f.delay_ms < 20)   f.delay_ms = 20;
            if (f.delay_ms > 5000) f.delay_ms = 5000;
        }
        res.frames.push_back(std::move(f));
    }

    res.ok = !res.frames.empty();
    return res;
}

}  // namespace launcher::d2d
