// ID2D1StrokeStyle 缓存 — 业务用得少（圆头/平头/虚线），全局 4-5 个就够。
#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <d2d1_1.h>
#include <d2d1.h>
#include <wrl/client.h>

namespace launcher::d2d {

using Microsoft::WRL::ComPtr;

class StrokeCache {
public:
    void init(ID2D1Factory1* f) { factory_ = f; }
    void release() { round_.Reset(); factory_ = nullptr; }

    // 圆头圆尾圆 join — spinner / progress 弧用
    ID2D1StrokeStyle* round() {
        if (!round_ && factory_) {
            factory_->CreateStrokeStyle(
                D2D1::StrokeStyleProperties(
                    D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_FLAT,
                    D2D1_LINE_JOIN_ROUND, 10.0f,
                    D2D1_DASH_STYLE_SOLID, 0.0f),
                nullptr, 0, &round_);
        }
        return round_.Get();
    }

private:
    ID2D1Factory1* factory_{};
    ComPtr<ID2D1StrokeStyle> round_;
};

}  // namespace launcher::d2d
