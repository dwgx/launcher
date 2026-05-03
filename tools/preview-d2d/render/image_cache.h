// WIC → ID2D1Bitmap 缓存。GDI+ 那边 Image::FromFile 的 D2D 等价。
//
// 头像 / sticker / pack cover 都已经下载到 %LOCALAPPDATA%/Launcher/...
// D2D 用 WIC 解码这些已存文件，预转 PBGRA + premul（DComp 强制要求）。
#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <d2d1_1.h>
#include <d2d1.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <string>
#include <unordered_map>

namespace launcher::d2d {

using Microsoft::WRL::ComPtr;

class ImageCache {
public:
    void init(ID2D1DeviceContext* ctx, IWICImagingFactory* wic) {
        ctx_ = ctx; wic_ = wic;
    }
    void release() {
        bitmaps_.clear(); ctx_ = nullptr; wic_ = nullptr;
    }
    // device 重建后调（暂未实现 device-lost 处理，先留接口）
    void invalidate() { bitmaps_.clear(); }

    // 从文件路径加载 → ID2D1Bitmap，失败返 nullptr。
    ID2D1Bitmap* fromFile(const std::wstring& path) {
        if (!ctx_ || !wic_) return nullptr;
        auto it = bitmaps_.find(path);
        if (it != bitmaps_.end()) return it->second.Get();

        ComPtr<IWICBitmapDecoder> decoder;
        if (FAILED(wic_->CreateDecoderFromFilename(
                path.c_str(), nullptr, GENERIC_READ,
                WICDecodeMetadataCacheOnDemand, &decoder))) return nullptr;
        ComPtr<ID2D1Bitmap> bmp = decodeFirstFrame(decoder.Get());
        if (!bmp) return nullptr;
        auto ins = bitmaps_.emplace(path, std::move(bmp));
        return ins.first->second.Get();
    }

    // 从内存 buffer 加载（PNG/JPG/GIF byte stream）
    ID2D1Bitmap* fromMemory(const std::wstring& key,
                            const void* data, size_t size) {
        if (!ctx_ || !wic_ || !data || size == 0) return nullptr;
        auto it = bitmaps_.find(key);
        if (it != bitmaps_.end()) return it->second.Get();

        ComPtr<IWICStream> stream;
        if (FAILED(wic_->CreateStream(&stream))) return nullptr;
        if (FAILED(stream->InitializeFromMemory(
                (BYTE*)const_cast<void*>(data), (DWORD)size))) return nullptr;
        ComPtr<IWICBitmapDecoder> decoder;
        if (FAILED(wic_->CreateDecoderFromStream(
                stream.Get(), nullptr,
                WICDecodeMetadataCacheOnDemand, &decoder))) return nullptr;
        ComPtr<ID2D1Bitmap> bmp = decodeFirstFrame(decoder.Get());
        if (!bmp) return nullptr;
        auto ins = bitmaps_.emplace(key, std::move(bmp));
        return ins.first->second.Get();
    }

private:
    ComPtr<ID2D1Bitmap> decodeFirstFrame(IWICBitmapDecoder* decoder) {
        ComPtr<IWICBitmapFrameDecode> frame;
        if (FAILED(decoder->GetFrame(0, &frame))) return {};
        // 转 32bpp PBGRA + premul（D2D / DComp 通用格式）
        ComPtr<IWICFormatConverter> conv;
        if (FAILED(wic_->CreateFormatConverter(&conv))) return {};
        if (FAILED(conv->Initialize(
                frame.Get(),
                GUID_WICPixelFormat32bppPBGRA,
                WICBitmapDitherTypeNone, nullptr, 0.0,
                WICBitmapPaletteTypeMedianCut))) return {};
        ComPtr<ID2D1Bitmap> bmp;
        if (FAILED(ctx_->CreateBitmapFromWicBitmap(conv.Get(), nullptr, &bmp))) return {};
        return bmp;
    }

    ID2D1DeviceContext* ctx_{};
    IWICImagingFactory* wic_{};
    std::unordered_map<std::wstring, ComPtr<ID2D1Bitmap>> bitmaps_;
};

}  // namespace launcher::d2d
