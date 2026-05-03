// 30+ Claude 风 SVG icons — 1:1 复刻 tools/preview/icons.inl。
// GDI+ Pen + GraphicsPath::AddArc/AddBezier → D2D ID2D1PathGeometry via PathBuilder。

#include "icons.h"
#include "render/path_builder.h"
#include "render/primitives.h"

#include <cmath>
#include <vector>

namespace launcher::d2d::icons {

void drawIcon(D2DApp& app, Name n, float x, float y, float size,
              uint32_t stroke_argb, float stroke_w) {
    auto* ctx = app.ctx();
    auto& br = app.brushes();
    auto* brush = br.solid(stroke_argb);
    if (!brush) return;
    auto* round = app.strokes().round();
    float thick = stroke_w * (size / 24.0f);

    auto sx = [&](float v) { return x + v * size / 24.0f; };
    auto sy = [&](float v) { return y + v * size / 24.0f; };
    auto sw_ = [&](float v) { return v * size / 24.0f; };
    auto* factory = app.factory();

    auto drawPath = [&](PathBuilder& p, bool fill = false) {
        auto* g = p.geometry();
        if (!g) return;
        if (fill) ctx->FillGeometry(g, brush);
        else      ctx->DrawGeometry(g, brush, thick, round);
    };
    auto line = [&](float x1, float y1, float x2, float y2) {
        ctx->DrawLine({ sx(x1), sy(y1) }, { sx(x2), sy(y2) }, brush, thick, round);
    };
    auto ell = [&](float xx, float yy, float ww, float hh, bool fill = false) {
        D2D1_ELLIPSE e = { { sx(xx) + sw_(ww) * 0.5f, sy(yy) + sw_(hh) * 0.5f },
                           sw_(ww) * 0.5f, sw_(hh) * 0.5f };
        if (fill) ctx->FillEllipse(e, brush);
        else      ctx->DrawEllipse(e, brush, thick, round);
    };

    switch (n) {
    case Name::Home: {
        PathBuilder p1(factory);
        p1.addLine(sx(3), sy(11.5f), sx(12), sy(4));
        p1.addLine(sx(12), sy(4),    sx(21), sy(11.5f));
        drawPath(p1);
        PathBuilder p2(factory);
        p2.addLine(sx(5), sy(10.5f), sx(5), sy(20));
        p2.addLine(sx(5), sy(20),    sx(19), sy(20));
        p2.addLine(sx(19), sy(20),   sx(19), sy(10.5f));
        drawPath(p2);
        PathBuilder p3(factory);
        p3.addLine(sx(10), sy(20), sx(10), sy(15));
        p3.addLine(sx(10), sy(15), sx(14), sy(15));
        p3.addLine(sx(14), sy(15), sx(14), sy(20));
        drawPath(p3);
        break;
    }
    case Name::Library: {
        line(6, 4, 6, 20);
        line(10, 4, 10, 20);
        PathBuilder p(factory);
        float r = 1.5f;
        p.addArc(sx(14), sy(4),     sw_(r * 2), sw_(r * 2), 180, 90);
        p.addArc(sx(20) - sw_(r*2), sy(4), sw_(r*2), sw_(r*2), 270, 90);
        p.addArc(sx(20) - sw_(r*2), sy(20) - sw_(r*2), sw_(r*2), sw_(r*2), 0, 90);
        p.addArc(sx(14), sy(20) - sw_(r*2), sw_(r*2), sw_(r*2), 90, 90);
        p.closeFigure();
        drawPath(p);
        line(16, 9, 18, 9);
        break;
    }
    case Name::Cloud: {
        PathBuilder cp(factory);
        cp.addArc(sx(4),  sy(11), sw_(7),  sw_(7),  90,  180);
        cp.addArc(sx(8),  sy(6),  sw_(10), sw_(10), 180, 180);
        cp.addArc(sx(13), sy(10), sw_(8),  sw_(8),  270, 180);
        cp.addLine(sx(17), sy(18), sx(7), sy(18));
        cp.closeFigure();
        drawPath(cp);
        break;
    }
    case Name::Chat: {
        float r = 3.0f;
        PathBuilder rr(factory);
        rr.addArc(sx(3), sy(4), sw_(r * 2), sw_(r * 2), 180, 90);
        rr.addArc(sx(21) - sw_(r * 2), sy(4), sw_(r * 2), sw_(r * 2), 270, 90);
        rr.addArc(sx(21) - sw_(r * 2), sy(16) - sw_(r * 2), sw_(r * 2), sw_(r * 2), 0, 90);
        rr.addLine(sx(11), sy(16), sx(8), sy(20));
        rr.addLine(sx(8),  sy(20), sx(7), sy(16));
        rr.addArc(sx(3),   sy(16) - sw_(r * 2), sw_(r * 2), sw_(r * 2), 90, 90);
        rr.closeFigure();
        drawPath(rr);
        break;
    }
    case Name::Settings: {
        // 8 齿星形多边形 + 内圆
        constexpr int teeth = 8;
        constexpr int verts = teeth * 2;
        std::vector<D2D1_POINT_2F> pts(verts);
        for (int i = 0; i < verts; ++i) {
            float ang = (i * (360.0f / verts) - 90.0f) * 3.14159265f / 180.0f;
            float r = (i % 2 == 0) ? 10.0f : 7.5f;
            pts[i] = { sx(12 + std::cos(ang) * r), sy(12 + std::sin(ang) * r) };
        }
        PathBuilder p(factory);
        p.addPolygon(pts.data(), pts.size());
        p.closeFigure();
        drawPath(p);
        ell(8.5f, 8.5f, 7.0f, 7.0f);
        break;
    }
    case Name::Logout: {
        PathBuilder rp(factory);
        rp.addLine(sx(9), sy(4), sx(5), sy(4));
        rp.addArc(sx(3), sy(4), sw_(4), sw_(4), 270, -90);
        rp.addLine(sx(3), sy(6), sx(3), sy(18));
        rp.addArc(sx(3), sy(16), sw_(4), sw_(4), 180, -90);
        rp.addLine(sx(5), sy(20), sx(9), sy(20));
        drawPath(rp);
        line(16, 17, 21, 12);
        line(21, 12, 16, 7);
        line(21, 12, 9, 12);
        break;
    }
    case Name::Logo: {
        // 外环 + 内圆 fill
        D2D1_ELLIPSE outer = { { sx(12), sy(12) }, sw_(9), sw_(9) };
        ctx->DrawEllipse(outer, brush, 2.0f * (size / 24.0f), round);
        ell(8, 8, 8, 8, /*fill=*/true);
        break;
    }
    case Name::Eye: {
        PathBuilder ep(factory);
        ep.addArc(sx(2), sy(8), sw_(20), sw_(8), 180, 180);
        ep.addArc(sx(2), sy(8), sw_(20), sw_(8), 0,   180);
        drawPath(ep);
        ell(9, 9, 6, 6);
        break;
    }
    case Name::EyeOff: {
        line(3, 3, 21, 21);
        PathBuilder ep(factory);
        ep.addArc(sx(2), sy(8), sw_(20), sw_(8), 180, 180);
        ep.addArc(sx(2), sy(8), sw_(20), sw_(8), 0,   180);
        drawPath(ep);
        break;
    }
    case Name::X:
        line(6, 6, 18, 18);
        line(18, 6, 6, 18);
        break;
    case Name::History: {
        PathBuilder hp(factory);
        hp.addArc(sx(3), sy(3), sw_(18), sw_(18), 30, 290);
        drawPath(hp);
        line(3, 3, 3, 8);
        line(3, 8, 8, 8);
        line(12, 7, 12, 12);
        line(12, 12, 15, 14);
        break;
    }
    case Name::Search:
        ell(4, 4, 14, 14);
        line(16.5f, 16.5f, 20, 20);
        break;
    case Name::Send: {
        PathBuilder sp(factory);
        sp.addLine(sx(3.5f), sy(11.5f), sx(21), sy(4));
        sp.addLine(sx(21),   sy(4),     sx(13.5f), sy(21.5f));
        sp.addLine(sx(13.5f), sy(21.5f), sx(11.5f), sy(14.5f));
        sp.closeFigure();
        drawPath(sp);
        break;
    }
    case Name::Phone: {
        PathBuilder pp(factory);
        pp.addLine(sx(22), sy(16.9f), sx(22), sy(19.9f));
        pp.addBezier(sx(22),   sy(19.9f),
                     sx(22),   sy(20.9f),
                     sx(21),   sy(21.9f),
                     sx(19.8f), sy(21.9f));
        pp.addBezier(sx(19.8f), sy(21.9f),
                     sx(11.2f), sy(21.9f),
                     sx(4),     sy(14.7f),
                     sx(2.2f),  sy(4.2f));
        pp.addBezier(sx(2.2f), sy(4.2f),
                     sx(2.1f), sy(2.9f),
                     sx(3.1f), sy(2),
                     sx(4.1f), sy(2));
        pp.addLine(sx(4.1f), sy(2), sx(7.1f), sy(2));
        drawPath(pp);
        break;
    }
    case Name::Video: {
        float r = 2.0f;
        PathBuilder rp(factory);
        rp.addArc(sx(2),                 sy(6),                 sw_(r * 2), sw_(r * 2), 180, 90);
        rp.addArc(sx(16) - sw_(r * 2),   sy(6),                 sw_(r * 2), sw_(r * 2), 270, 90);
        rp.addArc(sx(16) - sw_(r * 2),   sy(18) - sw_(r * 2),   sw_(r * 2), sw_(r * 2), 0,   90);
        rp.addArc(sx(2),                 sy(18) - sw_(r * 2),   sw_(r * 2), sw_(r * 2), 90,  90);
        rp.closeFigure();
        drawPath(rp);
        PathBuilder tp(factory);
        tp.addLine(sx(22), sy(8),  sx(16), sy(12));
        tp.addLine(sx(16), sy(12), sx(22), sy(16));
        tp.closeFigure();
        drawPath(tp);
        break;
    }
    case Name::More:
        ell(11, 4,  2, 2, /*fill=*/true);
        ell(11, 11, 2, 2, /*fill=*/true);
        ell(11, 18, 2, 2, /*fill=*/true);
        break;
    case Name::Smile: {
        ell(3, 3, 18, 18);
        PathBuilder mp(factory);
        mp.addArc(sx(8), sy(11), sw_(8), sw_(6), 0, 180);
        drawPath(mp);
        ell(8.5f, 8.5f, 1.2f, 1.2f, /*fill=*/true);
        ell(14.5f, 8.5f, 1.2f, 1.2f, /*fill=*/true);
        break;
    }
    case Name::Paperclip: {
        PathBuilder pp(factory);
        pp.addBezier(sx(21), sy(12),    sx(21),    sy(16.5f), sx(15),    sy(22),    sx(11.5f), sy(21.5f));
        pp.addBezier(sx(11.5f), sy(21.5f), sx(7),  sy(21),    sx(2.5f),  sy(16.5f), sx(4),     sy(12));
        pp.addBezier(sx(4),    sy(12),    sx(7),  sy(7.5f),  sx(11.5f), sy(3),     sx(15),    sy(5));
        drawPath(pp);
        break;
    }
    case Name::Check: {
        PathBuilder cp(factory);
        cp.addLine(sx(4), sy(12),  sx(9),  sy(17));
        cp.addLine(sx(9), sy(17),  sx(20), sy(6));
        drawPath(cp);
        break;
    }
    case Name::Check2: {
        PathBuilder c1(factory);
        c1.addLine(sx(1), sy(12), sx(5),  sy(16));
        c1.addLine(sx(5), sy(16), sx(14), sy(7));
        drawPath(c1);
        PathBuilder c2(factory);
        c2.addLine(sx(9),  sy(16), sx(13), sy(20));
        c2.addLine(sx(13), sy(20), sx(23), sy(10));
        drawPath(c2);
        break;
    }
    case Name::User: {
        ell(8, 4, 8, 8);
        PathBuilder up(factory);
        up.addArc(sx(4), sy(13), sw_(16), sw_(16), 180, 180);
        drawPath(up);
        break;
    }
    case Name::Moon: {
        PathBuilder mp(factory);
        mp.addBezier(sx(21), sy(12.8f), sx(20), sy(18),  sx(15), sy(21), sx(11), sy(20));
        mp.addBezier(sx(11), sy(20),    sx(5),  sy(18),  sx(3),  sy(12), sx(5),  sy(7));
        mp.addBezier(sx(5),  sy(7),     sx(7),  sy(4),   sx(10), sy(3),  sx(11.2f), sy(3));
        mp.addBezier(sx(11.2f), sy(3),  sx(10), sy(7),   sx(13), sy(11), sx(21), sy(12.8f));
        mp.closeFigure();
        drawPath(mp);
        break;
    }
    case Name::Shield: {
        PathBuilder sp(factory);
        sp.addLine(sx(12), sy(3), sx(4), sy(6));
        sp.addLine(sx(4),  sy(6), sx(4), sy(12));
        sp.addBezier(sx(4),  sy(12), sx(4),    sy(17), sx(7.5f),  sy(20.5f), sx(12), sy(21));
        sp.addBezier(sx(12), sy(21), sx(16.5f), sy(20.5f), sx(20), sy(17), sx(20), sy(12));
        sp.addLine(sx(20), sy(12), sx(20), sy(6));
        sp.addLine(sx(20), sy(6),  sx(12), sy(3));
        sp.closeFigure();
        drawPath(sp);
        break;
    }
    case Name::Bell: {
        PathBuilder bp(factory);
        bp.addArc(sx(6), sy(2), sw_(12), sw_(12), 180, 180);
        bp.addBezier(sx(18), sy(8),  sx(18), sy(15), sx(21), sy(16), sx(21), sy(16));
        bp.addLine(sx(21), sy(16), sx(3),  sy(16));
        bp.addBezier(sx(3),  sy(16), sx(3),  sy(15), sx(6),  sy(15), sx(6),  sy(8));
        drawPath(bp);
        PathBuilder cp(factory);
        cp.addArc(sx(10), sy(19), sw_(4), sw_(4), 0, 180);
        drawPath(cp);
        break;
    }
    case Name::Reply: {
        line(9, 7, 4, 12);
        line(4, 12, 9, 17);
        PathBuilder rp(factory);
        rp.addBezier(sx(4), sy(12), sx(12), sy(12), sx(20), sy(13), sx(20), sy(20));
        drawPath(rp);
        break;
    }
    case Name::At: {
        ell(3, 3, 18, 18);
        ell(8, 8, 8, 8);
        line(16, 8, 16, 14);
        PathBuilder ap(factory);
        ap.addBezier(sx(16), sy(14), sx(18), sy(14), sx(20), sy(13), sx(20), sy(11));
        drawPath(ap);
        break;
    }
    case Name::Link: {
        PathBuilder l1(factory);
        l1.addArc(sx(3), sy(9), sw_(6), sw_(6), 90, 180);
        l1.addLine(sx(6),  sy(9),  sx(11), sy(9));
        l1.addLine(sx(11), sy(15), sx(6),  sy(15));
        drawPath(l1);
        PathBuilder l2(factory);
        l2.addArc(sx(15), sy(9), sw_(6), sw_(6), 270, 180);
        l2.addLine(sx(13), sy(9),  sx(18), sy(9));
        l2.addLine(sx(18), sy(15), sx(13), sy(15));
        drawPath(l2);
        line(8, 12, 16, 12);
        break;
    }
    case Name::Play: {
        PathBuilder pp(factory);
        pp.addLine(sx(7),  sy(5),  sx(20), sy(12));
        pp.addLine(sx(20), sy(12), sx(7),  sy(19));
        pp.closeFigure();
        drawPath(pp, /*fill=*/true);
        break;
    }
    case Name::Trash: {
        line(4, 7, 20, 7);
        PathBuilder rp(factory);
        rp.addLine(sx(6), sy(7), sx(7), sy(20));
        rp.addArc(sx(7), sy(18), sw_(4), sw_(4), 180, -90);
        rp.addLine(sx(11), sy(20), sx(13), sy(20));
        rp.addArc(sx(13), sy(18), sw_(4), sw_(4), 270, -90);
        rp.addLine(sx(17), sy(20), sx(18), sy(7));
        drawPath(rp);
        line(9, 7, 9, 4);
        line(15, 7, 15, 4);
        line(9, 4, 15, 4);
        break;
    }
    case Name::Edit: {
        PathBuilder ep(factory);
        ep.addLine(sx(4), sy(20),  sx(20), sy(4));
        ep.addLine(sx(20), sy(4),  sx(16), sy(8));
        ep.addLine(sx(16), sy(8),  sx(8),  sy(16));
        ep.addLine(sx(8), sy(16),  sx(4),  sy(20));
        drawPath(ep);
        break;
    }
    case Name::Plus:
        line(12, 5, 12, 19);
        line(5, 12, 19, 12);
        break;
    case Name::Hash:
        line(9, 4, 7, 20);
        line(17, 4, 15, 20);
        line(4, 9, 20, 9);
        line(4, 15, 20, 15);
        break;
    case Name::Cart: {
        line(3, 3,  6, 3);
        line(6, 3,  7, 7);
        line(7, 7,  21, 7);
        line(21, 7, 18, 15);
        line(7, 7,  8,  15);
        line(8, 15, 18, 15);
        ell(8 - 1.5f, 19 - 1.5f, 3, 3);
        ell(17 - 1.5f, 19 - 1.5f, 3, 3);
        break;
    }
    }
}

}  // namespace launcher::d2d::icons
