// IDWriteTextFormat 缓存 + 一次性 IDWriteTextLayout 测量 helper。
// DirectWrite 是 D2D 文字唯一渲染管线，原生支持 Win11 彩色 emoji COLR/CPAL。
#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <string>
#include <string_view>
#include <unordered_map>

namespace launcher::d2d {

using Microsoft::WRL::ComPtr;

class TextCache {
public:
    void init(IDWriteFactory* dwrite) { dwrite_ = dwrite; }
    void release() { formats_.clear(); dwrite_ = nullptr; }

    // size_dip = pt * 96/72；weight 默认 NORMAL，bold = 700。
    IDWriteTextFormat* format(const wchar_t* face, float size_dip,
                              DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL,
                              DWRITE_FONT_STYLE style = DWRITE_FONT_STYLE_NORMAL) {
        Key k{ face ? face : L"", (int)(size_dip * 100.0f + 0.5f), (int)weight, (int)style };
        auto it = formats_.find(k);
        if (it != formats_.end()) return it->second.Get();

        ComPtr<IDWriteTextFormat> f;
        HRESULT hr = dwrite_->CreateTextFormat(
            k.face.c_str(), nullptr,
            weight, style, DWRITE_FONT_STRETCH_NORMAL,
            size_dip, L"zh-cn", &f);
        if (FAILED(hr)) return nullptr;

        auto ins = formats_.emplace(std::move(k), std::move(f));
        return ins.first->second.Get();
    }

    // 测量文字 — 一次性 layout（不缓存 layout，因为文本变化太多）
    bool measure(IDWriteTextFormat* fmt, std::wstring_view text,
                 float max_w, float max_h, DWRITE_TEXT_METRICS* out) {
        if (!fmt || !out || !dwrite_) return false;
        ComPtr<IDWriteTextLayout> layout;
        HRESULT hr = dwrite_->CreateTextLayout(text.data(), (UINT32)text.size(),
                                                fmt, max_w, max_h, &layout);
        if (FAILED(hr)) return false;
        return SUCCEEDED(layout->GetMetrics(out));
    }

    // 创建 layout 给 ctx->DrawTextLayout（多次相同文本时复用比 DrawText 快）
    ComPtr<IDWriteTextLayout> layout(IDWriteTextFormat* fmt, std::wstring_view text,
                                     float max_w, float max_h) {
        ComPtr<IDWriteTextLayout> r;
        if (!fmt || !dwrite_) return r;
        dwrite_->CreateTextLayout(text.data(), (UINT32)text.size(),
                                  fmt, max_w, max_h, &r);
        return r;
    }

private:
    struct Key {
        std::wstring face;
        int size_q;   // size_dip * 100 量化
        int weight;
        int style;
    };
    struct KeyHash {
        size_t operator()(const Key& k) const noexcept {
            size_t h = std::hash<std::wstring>{}(k.face);
            h ^= (size_t)k.size_q * 0x9e3779b1u + (h << 6) + (h >> 2);
            h ^= (size_t)k.weight * 0x85ebca6bu + (h << 6) + (h >> 2);
            h ^= (size_t)k.style  * 0xc2b2ae35u + (h << 6) + (h >> 2);
            return h;
        }
    };
    struct KeyEq {
        bool operator()(const Key& a, const Key& b) const noexcept {
            return a.size_q == b.size_q && a.weight == b.weight
                && a.style == b.style && a.face == b.face;
        }
    };

    IDWriteFactory* dwrite_{};
    std::unordered_map<Key, ComPtr<IDWriteTextFormat>, KeyHash, KeyEq> formats_;
};

}  // namespace launcher::d2d
