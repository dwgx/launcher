// GIF 多帧动画缓存 — IWICBitmapDecoder::GetFrameCount + 每帧 GetMetadataQueryReader
// /grctlext/Delay (units = 1/100s) → ID2D1Bitmap 数组 + 时间轮转。
//
// 跟 ImageCache 平级。一个 GIF 文件解码一次就缓存所有帧，paint 时按 elapsed
// 时间挑当前帧。
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

#include <unordered_map>
#include <vector>
#include <string>
#include <cstdint>

namespace launcher::d2d {

using Microsoft::WRL::ComPtr;

class GifCache {
public:
    struct Frame {
        ComPtr<ID2D1Bitmap> bmp;
        uint32_t delay_ms = 100;
    };
    struct Anim {
        std::vector<Frame> frames;
        uint32_t total_ms = 0;
        uint32_t width = 0, height = 0;
    };

    void init(ID2D1DeviceContext* ctx, IWICImagingFactory* wic) {
        ctx_ = ctx;
        wic_ = wic;
    }
    void release() { anims_.clear(); ctx_ = nullptr; wic_ = nullptr; }
    void invalidate() { anims_.clear(); }

    // 解码 GIF 全部帧 → 缓存。返回 nullptr 表示不是 GIF / 解码失败。
    const Anim* fromFile(const std::wstring& path) {
        if (!ctx_ || !wic_) return nullptr;
        auto it = anims_.find(path);
        if (it != anims_.end()) return &it->second;

        ComPtr<IWICBitmapDecoder> dec;
        if (FAILED(wic_->CreateDecoderFromFilename(
                path.c_str(), nullptr, GENERIC_READ,
                WICDecodeMetadataCacheOnDemand, &dec))) return nullptr;

        UINT count = 0;
        if (FAILED(dec->GetFrameCount(&count)) || count == 0) return nullptr;

        Anim anim;
        for (UINT i = 0; i < count; ++i) {
            ComPtr<IWICBitmapFrameDecode> frame;
            if (FAILED(dec->GetFrame(i, &frame))) continue;

            ComPtr<IWICFormatConverter> conv;
            if (FAILED(wic_->CreateFormatConverter(&conv))) continue;
            if (FAILED(conv->Initialize(
                    frame.Get(),
                    GUID_WICPixelFormat32bppPBGRA,
                    WICBitmapDitherTypeNone, nullptr, 0.0,
                    WICBitmapPaletteTypeMedianCut))) continue;

            Frame f;
            if (FAILED(ctx_->CreateBitmapFromWicBitmap(
                    conv.Get(), nullptr, &f.bmp))) continue;

            // 默认 100ms（GIF 没 metadata 时）
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
            if (f.delay_ms < 20) f.delay_ms = 20;     // GIF 实践最低 ~20ms
            if (f.delay_ms > 5000) f.delay_ms = 5000;

            if (i == 0) frame->GetSize(&anim.width, &anim.height);
            anim.total_ms += f.delay_ms;
            anim.frames.push_back(std::move(f));
        }

        if (anim.frames.empty()) return nullptr;
        auto ins = anims_.emplace(path, std::move(anim));
        return &ins.first->second;
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

private:
    ID2D1DeviceContext* ctx_{};
    IWICImagingFactory* wic_{};
    std::unordered_map<std::wstring, Anim> anims_;
};

}  // namespace launcher::d2d
