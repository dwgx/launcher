// Segoe UI Emoji 彩色字形位图缓存。
//
// 问题：emoji picker 每帧对几十个可见 emoji 调 ctx->DrawText(ENABLE_COLOR_FONT)，
// Segoe UI Emoji 的 COLR/CPAL 彩色字形每帧都要重新栅格化 → 卡顿。而图片表情包是
// 预解码成 ID2D1Bitmap + DrawBitmap，所以流畅。
//
// 解法：把每个 emoji 字形一次性栅格化进离屏 ID2D1Bitmap，之后 DrawBitmap 复用，
// 跟图片表情包同一条快路径。每个 (emoji, 格子尺寸, 字号) 组合只栅格化一次。
#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <d2d1_1.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <string>
#include <string_view>
#include <unordered_map>

namespace launcher::d2d {

using Microsoft::WRL::ComPtr;

class EmojiCache {
public:
    void init(ID2D1DeviceContext* ctx, IDWriteFactory* dwrite) {
        ctx_ = ctx;
        dwrite_ = dwrite;
    }
    void release() {
        bitmaps_.clear();
        ctx_ = nullptr;
        dwrite_ = nullptr;
    }
    // device 重建后调（与 ImageCache 一致，暂未接 device-lost，先留接口）
    void invalidate() { bitmaps_.clear(); }

    // 取（必要时栅格化）emoji 字形位图。
    //   cell_dip      = 方形格子边长（DIP），位图按此尺寸渲染
    //   font_size_dip = 字号（DIP）
    // 失败返 nullptr（调用方应回退到 DrawText）。
    ID2D1Bitmap* get(std::wstring_view emoji, float cell_dip, float font_size_dip) {
        if (!ctx_ || !dwrite_ || emoji.empty()) return nullptr;
        Key k{
            std::wstring(emoji),
            (int)(cell_dip * 4.0f + 0.5f),       // 量化到 0.25 DIP
            (int)(font_size_dip * 4.0f + 0.5f),
        };
        auto it = bitmaps_.find(k);
        if (it != bitmaps_.end()) return it->second.Get();

        ComPtr<ID2D1Bitmap> bmp = rasterize(emoji, cell_dip, font_size_dip);
        if (!bmp) return nullptr;  // 失败不缓存，下次重试（字体缺失极少见）
        auto ins = bitmaps_.emplace(std::move(k), std::move(bmp));
        return ins.first->second.Get();
    }

private:
    // 离屏栅格化：兼容 RT 继承父 ctx 的 DPI，位图像素 = cell_dip * dpi/96。
    // 字形在 cell 内水平+垂直居中；调用方用目标矩形做 hover 放大。
    // 2x 超采样：以 2 倍像素密度栅格化彩色字形，DrawBitmap 时线性缩小，
    // hover 放大或高 DPI 下仍保持锐利（消除"发糊/塑料"观感，更贴近系统原生）。
    ComPtr<ID2D1Bitmap> rasterize(std::wstring_view emoji, float cell_dip,
                                  float font_size_dip) {
        constexpr float kSupersample = 2.0f;
        ComPtr<ID2D1BitmapRenderTarget> brt;
        D2D1_SIZE_F logical = D2D1::SizeF(cell_dip, cell_dip);
        D2D1_SIZE_U pixels  = D2D1::SizeU(
            (UINT32)(cell_dip * kSupersample + 0.5f),
            (UINT32)(cell_dip * kSupersample + 0.5f));
        if (FAILED(ctx_->CreateCompatibleRenderTarget(
                &logical, &pixels, nullptr,
                D2D1_COMPATIBLE_RENDER_TARGET_OPTIONS_NONE, &brt)))
            return {};

        ComPtr<IDWriteTextFormat> fmt;
        if (FAILED(dwrite_->CreateTextFormat(
                L"Segoe UI Emoji", nullptr,
                DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL, font_size_dip, L"zh-cn", &fmt)))
            return {};
        fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

        // 彩色 emoji 用字体自带调色板；brush 仅作单色 fallback 部分的着色。
        ComPtr<ID2D1SolidColorBrush> brush;
        if (FAILED(brt->CreateSolidColorBrush(
                D2D1::ColorF(D2D1::ColorF::White), &brush)))
            return {};

        brt->BeginDraw();
        brt->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));  // 全透明背景
        brt->DrawText(
            emoji.data(), (UINT32)emoji.size(), fmt.Get(),
            D2D1::RectF(0.0f, 0.0f, cell_dip, cell_dip), brush.Get(),
            D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT,
            DWRITE_MEASURING_MODE_NATURAL);
        if (FAILED(brt->EndDraw())) return {};

        // GetBitmap 取得的位图独立持有，brt 释放后仍有效（标准 glyph-atlas 模式）。
        ComPtr<ID2D1Bitmap> out;
        if (FAILED(brt->GetBitmap(&out))) return {};
        return out;
    }

    struct Key {
        std::wstring e;
        int cell_q;  // cell_dip * 4 量化
        int size_q;  // font_size_dip * 4 量化
    };
    struct KeyHash {
        size_t operator()(const Key& k) const noexcept {
            size_t h = std::hash<std::wstring>{}(k.e);
            h ^= (size_t)k.cell_q * 0x9e3779b1u + (h << 6) + (h >> 2);
            h ^= (size_t)k.size_q * 0x85ebca6bu + (h << 6) + (h >> 2);
            return h;
        }
    };
    struct KeyEq {
        bool operator()(const Key& a, const Key& b) const noexcept {
            return a.cell_q == b.cell_q && a.size_q == b.size_q && a.e == b.e;
        }
    };

    ID2D1DeviceContext* ctx_{};
    IDWriteFactory* dwrite_{};
    std::unordered_map<Key, ComPtr<ID2D1Bitmap>, KeyHash, KeyEq> bitmaps_;
};

}  // namespace launcher::d2d
