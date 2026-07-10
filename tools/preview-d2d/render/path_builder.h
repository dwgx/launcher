// PathBuilder — 把 GDI+ GraphicsPath::AddLine/AddArc/AddBezier/AddPolygon
// 翻译到 ID2D1PathGeometry/GeometrySink 的简单适配器。
//
// GDI+ AddArc 用椭圆 bounding rect + start_deg + sweep_deg；D2D ArcSegment
// 用 endpoint + size + sweep_dir + arc_size。这里 do 三角函数算端点。
#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#ifdef DrawText
#undef DrawText
#endif
#include <d2d1_1.h>
#include <d2d1.h>
#include <wrl/client.h>
#include <cmath>

namespace launcher::d2d {

using Microsoft::WRL::ComPtr;

class PathBuilder {
public:
    explicit PathBuilder(ID2D1Factory* f) {
        if (f) {
            f->CreatePathGeometry(&geo_);
            if (geo_) geo_->Open(&sink_);
        }
    }
    ~PathBuilder() { close(); }

    ID2D1PathGeometry* geometry() {
        close();
        return geo_.Get();
    }

    // GDI+ AddLine(p1, p2): 如果是空 path 则 begin 在 p1；否则 line to p1 (if 不连续) + line to p2。
    void addLine(float x1, float y1, float x2, float y2) {
        ensureFigureOpen({x1, y1});
        if (last_.x != x1 || last_.y != y1) {
            sink_->AddLine({x1, y1});
        }
        sink_->AddLine({x2, y2});
        last_ = {x2, y2};
    }

    // GDI+ AddBezier(p0, c1, c2, p3)
    void addBezier(float x0, float y0,
                   float cx1, float cy1, float cx2, float cy2,
                   float x3, float y3) {
        ensureFigureOpen({x0, y0});
        if (last_.x != x0 || last_.y != y0) sink_->AddLine({x0, y0});
        sink_->AddBezier({{cx1, cy1}, {cx2, cy2}, {x3, y3}});
        last_ = {x3, y3};
    }

    // GDI+ AddArc(x, y, w, h, start_deg, sweep_deg) — 椭圆 bounding rect (x,y,w,h)
    void addArc(float x, float y, float w, float h,
                float start_deg, float sweep_deg) {
        constexpr float kPi = 3.14159265358979323846f;
        float rx = w * 0.5f, ry = h * 0.5f;
        float cx = x + rx, cy = y + ry;
        float start_rad = start_deg * kPi / 180.0f;
        float end_rad = (start_deg + sweep_deg) * kPi / 180.0f;
        D2D1_POINT_2F p0{ cx + rx * std::cos(start_rad), cy + ry * std::sin(start_rad) };
        D2D1_POINT_2F p1{ cx + rx * std::cos(end_rad),   cy + ry * std::sin(end_rad)   };
        ensureFigureOpen(p0);
        if (last_.x != p0.x || last_.y != p0.y) sink_->AddLine(p0);
        D2D1_SWEEP_DIRECTION dir = (sweep_deg >= 0)
            ? D2D1_SWEEP_DIRECTION_CLOCKWISE
            : D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE;
        D2D1_ARC_SIZE arc_size = (std::fabs(sweep_deg) > 180.0f)
            ? D2D1_ARC_SIZE_LARGE
            : D2D1_ARC_SIZE_SMALL;
        sink_->AddArc({ p1, { rx, ry }, 0.0f, dir, arc_size });
        last_ = p1;
    }

    // GDI+ AddPolygon(points[]) — 多边形 close path
    void addPolygon(const D2D1_POINT_2F* pts, size_t n) {
        if (n < 2) return;
        ensureFigureOpen(pts[0]);
        for (size_t i = 1; i < n; ++i) sink_->AddLine(pts[i]);
        last_ = pts[n - 1];
    }

    void closeFigure() {
        if (figure_open_) {
            sink_->EndFigure(D2D1_FIGURE_END_CLOSED);
            figure_open_ = false;
        }
    }
    // GDI+ Path Reset — 关掉当前 figure 但保留 geo_。
    // 业务通常 GraphicsPath 用完一次就丢，所以我们的 PathBuilder 也是 single-use。
    // 调 reset 后等同新 PathBuilder（其实就是 close 并重建）。
    void close() {
        if (sink_) {
            if (figure_open_) {
                sink_->EndFigure(D2D1_FIGURE_END_OPEN);
                figure_open_ = false;
            }
            sink_->Close();
            sink_.Reset();
        }
    }

private:
    void ensureFigureOpen(D2D1_POINT_2F p) {
        if (!sink_) return;
        if (!figure_open_) {
            sink_->BeginFigure(p, D2D1_FIGURE_BEGIN_HOLLOW);
            figure_open_ = true;
            last_ = p;
        }
    }

    ComPtr<ID2D1PathGeometry> geo_;
    ComPtr<ID2D1GeometrySink> sink_;
    bool figure_open_ = false;
    D2D1_POINT_2F last_{};
};

}  // namespace launcher::d2d
