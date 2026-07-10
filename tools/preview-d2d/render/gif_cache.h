// GIF 多帧动画缓存 —— Wave 1：非阻塞 3 态 + 字节预算 LRU + decode-to-target。
//
// 与 ImageCache 平级、同套机制（见 tmp/image-opt/PLAN-client_design.md §2）：
//   - fromFile 不再同步解码全部帧。命中 Ready→返回 Anim*；Pending/Failed/miss→nullptr
//     （miss 入队后台解码；GIF 较重 —— 所有帧都在 worker 缩放+转换）。
//   - drainCompleted() 在 UI 线程把每帧 IWICBitmap 上传成 ID2D1Bitmap，翻 Ready。
//   - intrinsicSize(path)：measure pass 不触发解码就能拿尺寸。
//   - GIF 帧字节预算独立于图片预算，编译期常量可调。
#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#ifdef DrawText
#undef DrawText
#endif
#include <d2d1_1.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "render/lru.h"
#include "render/decode_worker.h"

namespace launcher::d2d {

using Microsoft::WRL::ComPtr;

// GIF 帧缓存字节预算（独立于图片预算）。~96MB。编译期常量，易调。
constexpr size_t kGifCacheBudget = 96ull * 1024 * 1024;

class GifCache {
public:
    struct Frame {
        ComPtr<ID2D1Bitmap> bmp;
        uint32_t delay_ms = 100;
    };
    struct Anim {
        std::vector<Frame> frames;
        uint32_t total_ms = 0;
        uint32_t width = 0, height = 0;   // intrinsic（缩放前）尺寸
    };

    void init(ID2D1DeviceContext* ctx, IWICImagingFactory* wic) {
        ctx_ = ctx; wic_ = wic;
    }
    void release() {
        lru_.clear([](const DecodeKey&, Entry&){});
        intrinsic_.clear();
        ctx_ = nullptr; wic_ = nullptr;
    }
    void evict(const std::wstring& path) {
        lru_.eraseIf([&](const DecodeKey& k){ return k.path == path; },
                     [](const DecodeKey&, Entry&){});
        intrinsic_.erase(path);
    }

    // 取（必要时后台解码）GIF。命中 Ready→返回 Anim*；否则 nullptr（miss 入队）。
    const Anim* fromFile(const std::wstring& path, uint32_t targetPx = 0) {
        if (!ctx_ || path.empty()) return nullptr;
        DecodeKey k{ path, targetPx };
        if (Entry* e = lru_.get(k))
            return e->state == Entry::Ready ? &e->anim : nullptr;
        Entry e;
        e.state = Entry::Pending;
        lru_.put(k, std::move(e), 0);
        decodeService().enqueue(DecodeKind::Gif, k);
        return nullptr;
    }

    // measure pass 用：已知 intrinsic 尺寸，不触发解码。
    std::optional<SIZE> intrinsicSize(const std::wstring& path) const {
        auto it = intrinsic_.find(path);
        if (it == intrinsic_.end()) return std::nullopt;
        return it->second;
    }

    // UI 线程 kMsgDecodeReady 时调 —— 上传所有帧、翻 Ready。
    void drainCompleted() {
        if (!ctx_) return;
        auto results = decodeService().takeCompleted(DecodeKind::Gif);
        for (auto& res : results) {
            const DecodeKey& k = res.key;
            if (!res.ok || res.frames.empty()) {
                if (Entry* e = lru_.peek(k)) e->state = Entry::Failed;
                continue;
            }
            Anim anim;
            anim.width = res.iw; anim.height = res.ih;
            size_t bytes = 0;
            for (auto& df : res.frames) {
                ComPtr<ID2D1Bitmap> bmp;
                if (FAILED(ctx_->CreateBitmapFromWicBitmap(
                        df.wic.Get(), nullptr, &bmp))) continue;
                Frame f;
                f.bmp = std::move(bmp);
                f.delay_ms = df.delay_ms;
                anim.total_ms += f.delay_ms;
                bytes += (size_t)df.w * df.h * 4;
                anim.frames.push_back(std::move(f));
            }
            if (anim.frames.empty()) {
                if (Entry* e = lru_.peek(k)) e->state = Entry::Failed;
                continue;
            }
            intrinsic_[k.path] = SIZE{ (LONG)res.iw, (LONG)res.ih };
            if (Entry* e = lru_.peek(k)) {
                e->anim = std::move(anim);
                e->state = Entry::Ready;
                lru_.setBytes(k, bytes);
            } else {
                Entry ne; ne.state = Entry::Ready; ne.anim = std::move(anim);
                lru_.put(k, std::move(ne), bytes);
            }
        }
        lru_.evictToBudget(kGifCacheBudget, [](const DecodeKey&, Entry&){});
    }

    // 按 elapsed 秒挑当前帧。loop 自动循环。
    ID2D1Bitmap* frameAt(const Anim* anim, float t_seconds) {
        if (!anim || anim->frames.empty()) return nullptr;
        if (anim->total_ms == 0) return anim->frames[0].bmp.Get();
        uint32_t elapsed = (uint32_t)(t_seconds * 1000.0f) % anim->total_ms;
        uint32_t accum = 0;
        for (auto& f : anim->frames) {
            accum += f.delay_ms;
            if (elapsed < accum) return f.bmp.Get();
        }
        return anim->frames.back().bmp.Get();
    }

    // settle 签名（条目数 + 已用字节）—— 供 visual-smoke 判定解码静默。
    size_t cacheSize() const { return lru_.size() + lru_.bytes(); }

private:
    struct Entry {
        enum State { Pending, Ready, Failed } state = Pending;
        Anim anim;
    };

    ID2D1DeviceContext* ctx_{};
    IWICImagingFactory* wic_{};
    LruCache<DecodeKey, Entry, DecodeKeyHash> lru_;
    std::unordered_map<std::wstring, SIZE> intrinsic_;
};

}  // namespace launcher::d2d
