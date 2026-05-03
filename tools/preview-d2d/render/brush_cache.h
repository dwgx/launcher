// SolidColor brush 缓存 — D2D brush 创建有 GPU 同步成本，必须缓存。
// key = ARGB32 hex (跟 GDI+ 风格 Color(0xAARRGGBB) 对齐)。
#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <d2d1_1.h>
#include <d2d1.h>
#include <wrl/client.h>
#include <cstdint>
#include <unordered_map>

namespace launcher::d2d {

using Microsoft::WRL::ComPtr;

class BrushCache {
public:
    void init(ID2D1DeviceContext* ctx) { ctx_ = ctx; }
    void release() { brushes_.clear(); ctx_ = nullptr; }

    // hex = 0xAARRGGBB；不带 alpha 的可以直接传 0xRRGGBB（alpha=0 → 全透明，要小心）。
    ID2D1SolidColorBrush* solid(uint32_t argb) {
        auto it = brushes_.find(argb);
        if (it != brushes_.end()) return it->second.Get();
        D2D1_COLOR_F c{
            ((argb >> 16) & 0xFFu) / 255.0f,
            ((argb >>  8) & 0xFFu) / 255.0f,
            ( argb        & 0xFFu) / 255.0f,
            ((argb >> 24) & 0xFFu) / 255.0f
        };
        ComPtr<ID2D1SolidColorBrush> b;
        if (FAILED(ctx_->CreateSolidColorBrush(c, &b))) return nullptr;
        auto ins = brushes_.emplace(argb, std::move(b));
        return ins.first->second.Get();
    }

    // 已 D2D ColorF — 量化到 8-bit ARGB 当 key（避免 float 哈希）。
    ID2D1SolidColorBrush* solid(D2D1_COLOR_F c) {
        auto q = [](float v) -> uint32_t {
            if (v < 0.0f) v = 0.0f; if (v > 1.0f) v = 1.0f;
            return (uint32_t)(v * 255.0f + 0.5f) & 0xFFu;
        };
        uint32_t key = (q(c.a) << 24) | (q(c.r) << 16) | (q(c.g) << 8) | q(c.b);
        return solid(key);
    }

    // RGB hex + 浮点 alpha（业务里"渐入渐出"常用：rgb 不变只动 alpha）
    ID2D1SolidColorBrush* solidA(uint32_t rgb, float alpha) {
        if (alpha < 0.0f) alpha = 0.0f; if (alpha > 1.0f) alpha = 1.0f;
        uint32_t a = (uint32_t)(alpha * 255.0f + 0.5f) & 0xFFu;
        return solid((a << 24) | (rgb & 0xFFFFFFu));
    }

private:
    ID2D1DeviceContext* ctx_{};
    std::unordered_map<uint32_t, ComPtr<ID2D1SolidColorBrush>> brushes_;
};

}  // namespace launcher::d2d
