// WIC → ID2D1Bitmap 缓存 —— Wave 1：非阻塞 3 态 + 字节预算 LRU + decode-to-target。
//
// 关键变化（见 tmp/image-opt/PLAN-client_design.md §2/§4/§5）：
//   - fromFile 不再在 paint 线程同步解码。命中 Ready→返回；Pending/Failed→nullptr；
//     miss→插 Pending + DecodeService::enqueue + 返回 nullptr（本帧画占位，下帧淡入）。
//   - 所有 WIC 解码在 DecodeService 后台线程，缩到 targetPx 长边（decode-to-display-size）。
//   - drainCompleted() 是唯一创建 ID2D1Bitmap 的地方（UI 线程 CreateBitmapFromWicBitmap）。
//   - intrinsicSize(path)：measure pass 用它拿尺寸而**不触发解码**，杀掉「开频道同步解全史」的卡顿。
//   - evict(path)：头像刷新只逐出单个路径，不再 invalidate() 全清。invalidate() 只留给 device-lost/logout。
//   - BLURHASH 缝（Wave3）：Entry 带 blurhash 字符串 + decodeBlurhash() stub（现返回 nullptr），
//     Wave3 填充 backend blurhash 后可直接生成占位图，无需重新布线。
#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <d2d1_1.h>
#include <d2d1.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>

#include "render/lru.h"
#include "render/decode_worker.h"

namespace launcher::d2d {

using Microsoft::WRL::ComPtr;

// 图片缓存字节预算（缩放后 PBGRA：bytes = w*h*4）。~192MB。编译期常量，易调。
constexpr size_t kImageCacheBudget = 192ull * 1024 * 1024;

class ImageCache {
public:
    void init(ID2D1DeviceContext* ctx, IWICImagingFactory* wic) {
        ctx_ = ctx; wic_ = wic;
    }
    void release() {
        lru_.clear([](const DecodeKey&, Entry&){});
        intrinsic_.clear();
        ctx_ = nullptr; wic_ = nullptr;
    }
    // 全清 —— 仅 device-lost / logout 用（不再用于头像刷新）。
    void invalidate() {
        lru_.clear([](const DecodeKey&, Entry&){});
        intrinsic_.clear();
    }
    // 逐出单个路径的全部 targetPx 变体（头像云同步刷新用）。连 intrinsic 一并丢，
    // 保证同名文件被覆盖后重新读尺寸+像素。
    void evict(const std::wstring& path) {
        lru_.eraseIf([&](const DecodeKey& k){ return k.path == path; },
                     [](const DecodeKey&, Entry&){});
        intrinsic_.erase(path);
    }

    // 取（必要时后台解码）targetPx 尺寸的位图。
    //   targetPx    = 目标长边像素（0 = 原生尺寸，不缩放）。缩放在 worker 里做。
    //   blurhash    = 可选（Wave3 从 Msg 传入）→ 存到 Entry 供占位；现阶段仅布线。
    //   out_opacity = 可选，回填淡入不透明度 [0,1]（Ready 后 ~120ms 斜坡）。
    // Ready → 返回位图 + touch LRU；Pending/Failed/miss → nullptr（miss 会入队解码）。
    ID2D1Bitmap* fromFile(const std::wstring& path, uint32_t targetPx = 0,
                          const std::string* blurhash = nullptr,
                          float* out_opacity = nullptr) {
        if (out_opacity) *out_opacity = 1.0f;
        if (!ctx_ || path.empty()) return nullptr;
        DecodeKey k{ path, targetPx };
        if (Entry* e = lru_.get(k)) {
            if (blurhash && e->blurhash.empty() && !blurhash->empty())
                e->blurhash = *blurhash;
            if (e->state == Entry::Ready) {
                if (out_opacity) *out_opacity = fadeOpacity(e->ready_ms);
                return e->bmp.Get();
            }
            return nullptr;  // Pending / Failed
        }
        // miss → 插 Pending 占位 + 入队后台解码
        Entry e;
        e.state = Entry::Pending;
        if (blurhash) e.blurhash = *blurhash;
        lru_.put(k, std::move(e), 0);
        decodeService().enqueue(DecodeKind::Image, k);
        return nullptr;
    }

    // measure pass 用：返回已知的 intrinsic（原始）尺寸，**绝不触发解码**。
    // worker 完成后 drainCompleted() 会填 intrinsic_。未知 → nullopt（调用方用默认框）。
    std::optional<SIZE> intrinsicSize(const std::wstring& path) const {
        auto it = intrinsic_.find(path);
        if (it == intrinsic_.end()) return std::nullopt;
        return it->second;
    }

    // UI 线程收到 kMsgDecodeReady 后调 —— 取走后台完成的图，做 GPU 上传，翻 Ready。
    // 这是唯一 CreateBitmapFromWicBitmap 的地方（D2D ctx 单线程约束）。
    void drainCompleted() {
        if (!ctx_) return;
        auto results = decodeService().takeCompleted(DecodeKind::Image);
        for (auto& res : results) {
            const DecodeKey& k = res.key;
            if (!res.ok || res.frames.empty()) {
                if (Entry* e = lru_.peek(k)) e->state = Entry::Failed;
                continue;
            }
            ComPtr<ID2D1Bitmap> bmp;
            if (FAILED(ctx_->CreateBitmapFromWicBitmap(
                    res.frames[0].wic.Get(), nullptr, &bmp))) {
                if (Entry* e = lru_.peek(k)) e->state = Entry::Failed;
                continue;
            }
            intrinsic_[k.path] = SIZE{ (LONG)res.iw, (LONG)res.ih };
            size_t bytes = (size_t)res.frames[0].w * res.frames[0].h * 4;
            if (Entry* e = lru_.peek(k)) {
                e->bmp = std::move(bmp);
                e->iw = res.iw; e->ih = res.ih;
                e->state = Entry::Ready;
                e->ready_ms = GetTickCount();
                lru_.setBytes(k, bytes);
            } else {
                // 极少见：Pending 项在解码期间被逐出 —— 直接以 Ready 重新插入。
                Entry ne;
                ne.state = Entry::Ready;
                ne.bmp = std::move(bmp);
                ne.iw = res.iw; ne.ih = res.ih;
                ne.ready_ms = GetTickCount();
                lru_.put(k, std::move(ne), bytes);
            }
        }
        // 预算逐出（UI 线程、paint 之前；可见位图刚被 touch 到 MRU，不会被逐）。
        lru_.evictToBudget(kImageCacheBudget, [](const DecodeKey&, Entry&){});
    }

    // 从内存 buffer 同步加载（PNG/JPG byte stream）—— 罕用路径，保持同步语义。
    ID2D1Bitmap* fromMemory(const std::wstring& key,
                            const void* data, size_t size) {
        if (!ctx_ || !wic_ || !data || size == 0) return nullptr;
        DecodeKey k{ key, 0 };
        if (Entry* e = lru_.get(k))
            return e->state == Entry::Ready ? e->bmp.Get() : nullptr;

        ComPtr<IWICStream> stream;
        if (FAILED(wic_->CreateStream(&stream))) return nullptr;
        if (FAILED(stream->InitializeFromMemory(
                (BYTE*)const_cast<void*>(data), (DWORD)size))) return nullptr;
        ComPtr<IWICBitmapDecoder> decoder;
        if (FAILED(wic_->CreateDecoderFromStream(
                stream.Get(), nullptr,
                WICDecodeMetadataCacheOnDemand, &decoder))) return nullptr;
        ComPtr<IWICBitmapFrameDecode> frame;
        if (FAILED(decoder->GetFrame(0, &frame))) return nullptr;
        ComPtr<IWICFormatConverter> conv;
        if (FAILED(wic_->CreateFormatConverter(&conv))) return nullptr;
        if (FAILED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA,
                WICBitmapDitherTypeNone, nullptr, 0.0,
                WICBitmapPaletteTypeMedianCut))) return nullptr;
        ComPtr<ID2D1Bitmap> bmp;
        if (FAILED(ctx_->CreateBitmapFromWicBitmap(conv.Get(), nullptr, &bmp)))
            return nullptr;
        uint32_t iw = 0, ih = 0; frame->GetSize(&iw, &ih);
        Entry e; e.state = Entry::Ready; e.bmp = bmp;
        e.iw = iw; e.ih = ih; e.ready_ms = GetTickCount();
        Entry* ins = lru_.put(k, std::move(e), (size_t)iw * ih * 4);
        return ins->bmp.Get();
    }

    // settle 签名（条目数 + 已用字节）—— 供 visual-smoke 判定「解码是否已静默」。
    // 入队新 Pending 会改变条目数；Pending→Ready 上传会改变字节数，两者都覆盖。
    size_t cacheSize() const { return lru_.size() + lru_.bytes(); }

private:
    struct Entry {
        enum State { Pending, Ready, Failed } state = Pending;
        ComPtr<ID2D1Bitmap> bmp;
        uint32_t iw = 0, ih = 0;      // intrinsic（缩放前原始）尺寸
        uint32_t ready_ms = 0;        // GetTickCount() at Ready — 淡入起点
        std::string blurhash;         // Wave3 seam：backend 下发的 blurhash 串
        ComPtr<ID2D1Bitmap> blur_bmp; // Wave3 seam：blurhash 解码出的占位图（现为空）
    };

    // Ready 后 ~120ms 线性淡入。ready_ms==0 视为无淡入（fromMemory / 老图）。
    static float fadeOpacity(uint32_t ready_ms) {
        if (ready_ms == 0) return 1.0f;
        uint32_t dt = GetTickCount() - ready_ms;
        if (dt >= 120) return 1.0f;
        return (float)dt / 120.0f;
    }

    // BLURHASH SEAM（Wave3）：把 blurhash 串解成一张小占位位图。
    // 现在是 stub 返回 nullptr —— Wave3 在此实现 blurhash→RGB→ID2D1Bitmap，
    // 调用点（fromFile miss 分支 / paint 占位）已预留，无需重新布线。
    ComPtr<ID2D1Bitmap> decodeBlurhash(const std::string& /*hash*/) {
        return {};  // TODO(Wave3): 实现 blurhash 解码
    }

    ID2D1DeviceContext* ctx_{};
    IWICImagingFactory* wic_{};
    LruCache<DecodeKey, Entry, DecodeKeyHash> lru_;
    std::unordered_map<std::wstring, SIZE> intrinsic_;  // path → 原始尺寸（跨逐出保留）
};

}  // namespace launcher::d2d
