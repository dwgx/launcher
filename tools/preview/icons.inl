// Claude 审美 SVG 图标 — 用 GraphicsPath 复刻 design components.jsx 的 stroke=1.7 set.
// 全部 24x24 viewBox，渲染时按 size 缩放居中。
#pragma once

#include <vector>

namespace icons {

enum class Name {
    Home, Library, Cloud, Chat, Settings, Logout, Logo,
    Eye, EyeOff, X, History, Search, Send, Phone, Video, More,
    Smile, Paperclip, Check, Check2, User, Moon, Shield, Bell,
    Reply, At, Link, Play, Trash, Edit, Plus, Hash,
    Cart,   // 购物车 — 用于 Market 侧栏
};

// 在 [x,y, x+size, y+size] 区域画 24x24 viewBox 路径
void drawSvg(Graphics& g, Name n, float x, float y, float size, Color stroke,
             float stroke_w = 1.7f) {
    Pen pen(stroke, stroke_w * (size / 24.0f));
    pen.SetStartCap(LineCapRound);
    pen.SetEndCap(LineCapRound);
    pen.SetLineJoin(LineJoinRound);
    // Why Inset: 跟 strokeRR 同样的理由 — Center 半像素跨界让 stroke 雾化
    pen.SetAlignment(PenAlignmentInset);
    SolidBrush fill(stroke);

    // 缩放：把 24x24 viewBox 映射到 [x,y,size,size]
    auto sx = [&](float v) { return x + v * size / 24.0f; };
    auto sy = [&](float v) { return y + v * size / 24.0f; };
    auto sw = [&](float v) { return v * size / 24.0f; };

    GraphicsPath p;
    switch (n) {
    case Name::Home:
        p.StartFigure();
        p.AddLine(sx(3), sy(11.5f), sx(12), sy(4));
        p.AddLine(sx(12), sy(4),  sx(21), sy(11.5f));
        g.DrawPath(&pen, &p);
        p.Reset();
        p.AddLine(sx(5), sy(10.5f), sx(5), sy(20));
        p.AddLine(sx(5), sy(20),    sx(19), sy(20));
        p.AddLine(sx(19), sy(20),   sx(19), sy(10.5f));
        g.DrawPath(&pen, &p);
        p.Reset();
        p.AddLine(sx(10), sy(20), sx(10), sy(15));
        p.AddLine(sx(10), sy(15), sx(14), sy(15));
        p.AddLine(sx(14), sy(15), sx(14), sy(20));
        g.DrawPath(&pen, &p);
        break;
    case Name::Library:
        g.DrawLine(&pen, sx(6),  sy(4), sx(6),  sy(20));
        g.DrawLine(&pen, sx(10), sy(4), sx(10), sy(20));
        {
            GraphicsPath rp;
            float r = sw(1.5f);
            rp.AddArc(sx(14), sy(4),  r*2, r*2, 180, 90);
            rp.AddArc(sx(20)-r*2, sy(4),  r*2, r*2, 270, 90);
            rp.AddArc(sx(20)-r*2, sy(20)-r*2, r*2, r*2, 0, 90);
            rp.AddArc(sx(14), sy(20)-r*2, r*2, r*2, 90, 90);
            rp.CloseFigure();
            g.DrawPath(&pen, &rp);
        }
        g.DrawLine(&pen, sx(16), sy(9), sx(18), sy(9));
        break;
    case Name::Cloud:
        // 简化云路径
        {
            GraphicsPath cp;
            cp.AddArc(sx(4), sy(11), sw(7), sw(7), 90, 180);
            cp.AddArc(sx(8), sy(6), sw(10), sw(10), 180, 180);
            cp.AddArc(sx(13), sy(10), sw(8), sw(8), 270, 180);
            cp.AddLine(sx(17), sy(18), sx(7), sy(18));
            cp.CloseFigure();
            g.DrawPath(&pen, &cp);
        }
        break;
    case Name::Chat:
        {
            // 圆角矩形 + 底左尖（speech bubble）
            float r = sw(3);
            GraphicsPath rr;
            rr.AddArc(sx(3), sy(4), r*2, r*2, 180, 90);            // top-left
            rr.AddArc(sx(21)-r*2, sy(4), r*2, r*2, 270, 90);       // top-right
            rr.AddArc(sx(21)-r*2, sy(16)-r*2, r*2, r*2, 0, 90);    // bottom-right
            rr.AddLine(sx(11), sy(16), sx(8), sy(20));             // 底缘到尖
            rr.AddLine(sx(8), sy(20), sx(7), sy(16));              // 尖回去
            rr.AddArc(sx(3), sy(16)-r*2, r*2, r*2, 90, 90);        // bottom-left
            rr.CloseFigure();
            g.DrawPath(&pen, &rr);
        }
        break;
    case Name::Settings:
        {
            // 真齿轮：8 齿星形多边形 (16 个交替 r) + 内圆
            const int teeth = 8;
            const int verts = teeth * 2;
            std::vector<PointF> pts(verts);
            for (int i = 0; i < verts; ++i) {
                float ang = (i * (360.0f / verts) - 90.0f) * 3.14159265f / 180.0f;
                // 偶数索引外凸（齿尖），奇数内凹（齿根）
                float r = (i % 2 == 0) ? 10.0f : 7.5f;
                pts[i].X = sx(12 + cosf(ang) * r);
                pts[i].Y = sy(12 + sinf(ang) * r);
            }
            GraphicsPath gp;
            gp.AddPolygon(pts.data(), verts);
            g.DrawPath(&pen, &gp);
            // 内圆
            g.DrawEllipse(&pen, sx(8.5f), sy(8.5f), sw(7.0f), sw(7.0f));
        }
        break;
    case Name::Logout:
        // 框
        {
            GraphicsPath rp;
            rp.AddLine(sx(9), sy(4), sx(5), sy(4));
            rp.AddArc(sx(3), sy(4), sw(4), sw(4), 270, -90);
            rp.AddLine(sx(3), sy(6), sx(3), sy(18));
            rp.AddArc(sx(3), sy(16), sw(4), sw(4), 180, -90);
            rp.AddLine(sx(5), sy(20), sx(9), sy(20));
            g.DrawPath(&pen, &rp);
        }
        // 箭头
        g.DrawLine(&pen, sx(16), sy(17), sx(21), sy(12));
        g.DrawLine(&pen, sx(21), sy(12), sx(16), sy(7));
        g.DrawLine(&pen, sx(21), sy(12), sx(9),  sy(12));
        break;
    case Name::Logo:
        // Claude 风：外环 stroke + 内圆 fill
        {
            Pen wp(stroke, 2.0f * (size / 24.0f));
            g.DrawEllipse(&wp, sx(3), sy(3), sw(18), sw(18));
        }
        g.FillEllipse(&fill, sx(8), sy(8), sw(8), sw(8));
        break;
    case Name::Eye:
        {
            GraphicsPath ep;
            ep.AddArc(sx(2), sy(8), sw(20), sw(8), 180, 180);
            ep.AddArc(sx(2), sy(8), sw(20), sw(8), 0, 180);
            g.DrawPath(&pen, &ep);
        }
        g.DrawEllipse(&pen, sx(9), sy(9), sw(6), sw(6));
        break;
    case Name::EyeOff:
        g.DrawLine(&pen, sx(3), sy(3), sx(21), sy(21));
        {
            GraphicsPath ep;
            ep.AddArc(sx(2), sy(8), sw(20), sw(8), 180, 180);
            ep.AddArc(sx(2), sy(8), sw(20), sw(8), 0, 180);
            g.DrawPath(&pen, &ep);
        }
        break;
    case Name::X:
        g.DrawLine(&pen, sx(6), sy(6), sx(18), sy(18));
        g.DrawLine(&pen, sx(18), sy(6), sx(6), sy(18));
        break;
    case Name::History:
        {
            GraphicsPath hp;
            hp.AddArc(sx(3), sy(3), sw(18), sw(18), 30, 290);
            g.DrawPath(&pen, &hp);
        }
        g.DrawLine(&pen, sx(3), sy(3), sx(3), sy(8));
        g.DrawLine(&pen, sx(3), sy(8), sx(8), sy(8));
        g.DrawLine(&pen, sx(12), sy(7), sx(12), sy(12));
        g.DrawLine(&pen, sx(12), sy(12), sx(15), sy(14));
        break;
    case Name::Search:
        g.DrawEllipse(&pen, sx(4), sy(4), sw(14), sw(14));
        g.DrawLine(&pen, sx(16.5f), sy(16.5f), sx(20), sy(20));
        break;
    case Name::Send:
        {
            GraphicsPath sp;
            sp.AddLine(sx(3.5f), sy(11.5f), sx(21), sy(4));
            sp.AddLine(sx(21), sy(4), sx(13.5f), sy(21.5f));
            sp.AddLine(sx(13.5f), sy(21.5f), sx(11.5f), sy(14.5f));
            sp.CloseFigure();
            g.DrawPath(&pen, &sp);
        }
        break;
    case Name::Phone:
        {
            GraphicsPath pp;
            pp.AddLine(sx(22), sy(16.9f), sx(22), sy(19.9f));
            pp.AddBezier(sx(22), sy(19.9f), sx(22), sy(20.9f), sx(21), sy(21.9f), sx(19.8f), sy(21.9f));
            pp.AddBezier(sx(19.8f), sy(21.9f), sx(11.2f), sy(21.9f), sx(4), sy(14.7f), sx(2.2f), sy(4.2f));
            pp.AddBezier(sx(2.2f), sy(4.2f), sx(2.1f), sy(2.9f), sx(3.1f), sy(2), sx(4.1f), sy(2));
            pp.AddLine(sx(4.1f), sy(2), sx(7.1f), sy(2));
            g.DrawPath(&pen, &pp);
        }
        break;
    case Name::Video:
        {
            GraphicsPath rp;
            float r = sw(2);
            rp.AddArc(sx(2), sy(6), r*2, r*2, 180, 90);
            rp.AddArc(sx(16)-r*2, sy(6), r*2, r*2, 270, 90);
            rp.AddArc(sx(16)-r*2, sy(18)-r*2, r*2, r*2, 0, 90);
            rp.AddArc(sx(2), sy(18)-r*2, r*2, r*2, 90, 90);
            rp.CloseFigure();
            g.DrawPath(&pen, &rp);
        }
        // 右侧三角
        {
            GraphicsPath tp;
            tp.AddLine(sx(22), sy(8), sx(16), sy(12));
            tp.AddLine(sx(16), sy(12), sx(22), sy(16));
            tp.CloseFigure();
            g.DrawPath(&pen, &tp);
        }
        break;
    case Name::More:
        g.FillEllipse(&fill, sx(11), sy(4), sw(2), sw(2));
        g.FillEllipse(&fill, sx(11), sy(11), sw(2), sw(2));
        g.FillEllipse(&fill, sx(11), sy(18), sw(2), sw(2));
        break;
    case Name::Smile:
        g.DrawEllipse(&pen, sx(3), sy(3), sw(18), sw(18));
        // 嘴
        {
            GraphicsPath mp;
            mp.AddArc(sx(8), sy(11), sw(8), sw(6), 0, 180);
            g.DrawPath(&pen, &mp);
        }
        // 眼
        g.FillEllipse(&fill, sx(8.5f), sy(8.5f), sw(1.2f), sw(1.2f));
        g.FillEllipse(&fill, sx(14.5f), sy(8.5f), sw(1.2f), sw(1.2f));
        break;
    case Name::Paperclip:
        {
            GraphicsPath pp;
            pp.AddBezier(sx(21), sy(12), sx(21), sy(16.5f), sx(15), sy(22), sx(11.5f), sy(21.5f));
            pp.AddBezier(sx(11.5f), sy(21.5f), sx(7), sy(21), sx(2.5f), sy(16.5f), sx(4), sy(12));
            pp.AddBezier(sx(4), sy(12), sx(7), sy(7.5f), sx(11.5f), sy(3), sx(15), sy(5));
            g.DrawPath(&pen, &pp);
        }
        break;
    case Name::Check:
        {
            GraphicsPath cp;
            cp.AddLine(sx(4), sy(12), sx(9), sy(17));
            cp.AddLine(sx(9), sy(17), sx(20), sy(6));
            g.DrawPath(&pen, &cp);
        }
        break;
    case Name::Check2:
        // 双勾
        {
            GraphicsPath cp;
            cp.AddLine(sx(1), sy(12), sx(5), sy(16));
            cp.AddLine(sx(5), sy(16), sx(14), sy(7));
            g.DrawPath(&pen, &cp);
        }
        {
            GraphicsPath cp;
            cp.AddLine(sx(9), sy(16), sx(13), sy(20));
            cp.AddLine(sx(13), sy(20), sx(23), sy(10));
            g.DrawPath(&pen, &cp);
        }
        break;
    case Name::User:
        g.DrawEllipse(&pen, sx(8), sy(4), sw(8), sw(8));
        {
            GraphicsPath up;
            up.AddArc(sx(4), sy(13), sw(16), sw(16), 180, 180);
            g.DrawPath(&pen, &up);
        }
        break;
    case Name::Moon:
        {
            GraphicsPath mp;
            mp.AddBezier(sx(21), sy(12.8f), sx(20), sy(18), sx(15), sy(21), sx(11), sy(20));
            mp.AddBezier(sx(11), sy(20), sx(5), sy(18), sx(3), sy(12), sx(5), sy(7));
            mp.AddBezier(sx(5), sy(7), sx(7), sy(4), sx(10), sy(3), sx(11.2f), sy(3));
            mp.AddBezier(sx(11.2f), sy(3), sx(10), sy(7), sx(13), sy(11), sx(21), sy(12.8f));
            mp.CloseFigure();
            g.DrawPath(&pen, &mp);
        }
        break;
    case Name::Shield:
        {
            GraphicsPath sp;
            sp.AddLine(sx(12), sy(3), sx(4), sy(6));
            sp.AddLine(sx(4), sy(6), sx(4), sy(12));
            sp.AddBezier(sx(4), sy(12), sx(4), sy(17), sx(7.5f), sy(20.5f), sx(12), sy(21));
            sp.AddBezier(sx(12), sy(21), sx(16.5f), sy(20.5f), sx(20), sy(17), sx(20), sy(12));
            sp.AddLine(sx(20), sy(12), sx(20), sy(6));
            sp.AddLine(sx(20), sy(6),  sx(12), sy(3));
            sp.CloseFigure();
            g.DrawPath(&pen, &sp);
        }
        break;
    case Name::Bell:
        {
            GraphicsPath bp;
            bp.AddArc(sx(6), sy(2), sw(12), sw(12), 180, 180);
            bp.AddBezier(sx(18), sy(8), sx(18), sy(15), sx(21), sy(16), sx(21), sy(16));
            bp.AddLine(sx(21), sy(16), sx(3), sy(16));
            bp.AddBezier(sx(3), sy(16), sx(3), sy(15), sx(6), sy(15), sx(6), sy(8));
            g.DrawPath(&pen, &bp);
        }
        // 振铃
        {
            GraphicsPath cp;
            cp.AddArc(sx(10), sy(19), sw(4), sw(4), 0, 180);
            g.DrawPath(&pen, &cp);
        }
        break;
    case Name::Reply:
        // 弯箭头
        g.DrawLine(&pen, sx(9), sy(7), sx(4), sy(12));
        g.DrawLine(&pen, sx(4), sy(12), sx(9), sy(17));
        {
            GraphicsPath rp;
            rp.AddBezier(sx(4), sy(12), sx(12), sy(12), sx(20), sy(13), sx(20), sy(20));
            g.DrawPath(&pen, &rp);
        }
        break;
    case Name::At:
        g.DrawEllipse(&pen, sx(3), sy(3), sw(18), sw(18));
        g.DrawEllipse(&pen, sx(8), sy(8), sw(8), sw(8));
        g.DrawLine(&pen, sx(16), sy(8), sx(16), sy(14));
        {
            GraphicsPath ap;
            ap.AddBezier(sx(16), sy(14), sx(18), sy(14), sx(20), sy(13), sx(20), sy(11));
            g.DrawPath(&pen, &ap);
        }
        break;
    case Name::Link:
        // 两个相扣的圆角矩形
        {
            GraphicsPath lp;
            lp.AddArc(sx(3), sy(9), sw(6), sw(6), 90, 180);
            lp.AddLine(sx(6), sy(9), sx(11), sy(9));
            lp.AddLine(sx(11), sy(15), sx(6), sy(15));
            g.DrawPath(&pen, &lp);
        }
        {
            GraphicsPath lp;
            lp.AddArc(sx(15), sy(9), sw(6), sw(6), 270, 180);
            lp.AddLine(sx(13), sy(9), sx(18), sy(9));
            lp.AddLine(sx(18), sy(15), sx(13), sy(15));
            g.DrawPath(&pen, &lp);
        }
        g.DrawLine(&pen, sx(8), sy(12), sx(16), sy(12));
        break;
    case Name::Play:
        {
            GraphicsPath pp;
            pp.AddLine(sx(7), sy(5), sx(20), sy(12));
            pp.AddLine(sx(20), sy(12), sx(7), sy(19));
            pp.CloseFigure();
            g.FillPath(&fill, &pp);
        }
        break;
    case Name::Trash:
        g.DrawLine(&pen, sx(4), sy(7), sx(20), sy(7));
        {
            GraphicsPath rp;
            rp.AddLine(sx(6), sy(7), sx(7), sy(20));
            rp.AddArc(sx(7), sy(18), sw(4), sw(4), 180, -90);
            rp.AddLine(sx(11), sy(20), sx(13), sy(20));
            rp.AddArc(sx(13), sy(18), sw(4), sw(4), 270, -90);
            rp.AddLine(sx(17), sy(20), sx(18), sy(7));
            g.DrawPath(&pen, &rp);
        }
        g.DrawLine(&pen, sx(9), sy(7), sx(9), sy(4));
        g.DrawLine(&pen, sx(15), sy(7), sx(15), sy(4));
        g.DrawLine(&pen, sx(9), sy(4), sx(15), sy(4));
        break;
    case Name::Edit:
        {
            GraphicsPath ep;
            ep.AddLine(sx(4), sy(20), sx(20), sy(4));
            ep.AddLine(sx(20), sy(4), sx(16), sy(8));
            ep.AddLine(sx(16), sy(8), sx(8), sy(16));
            ep.AddLine(sx(8), sy(16), sx(4), sy(20));
            g.DrawPath(&pen, &ep);
        }
        break;
    case Name::Plus:
        g.DrawLine(&pen, sx(12), sy(5), sx(12), sy(19));
        g.DrawLine(&pen, sx(5), sy(12), sx(19), sy(12));
        break;
    case Name::Hash:
        g.DrawLine(&pen, sx(9), sy(4), sx(7), sy(20));
        g.DrawLine(&pen, sx(17), sy(4), sx(15), sy(20));
        g.DrawLine(&pen, sx(4), sy(9), sx(20), sy(9));
        g.DrawLine(&pen, sx(4), sy(15), sx(20), sy(15));
        break;
    case Name::Cart:
        // Lucide shopping-cart 风：把手 + 篮筐 + 两个轮子
        // 把手：(2,3)→(5,3)→(6,7)；篮筐顶弧 (6,7)→(21,7) 平直，底回 (8,17)→(19,17)
        {
            // 把手 (左上斜把)
            g.DrawLine(&pen, sx(3), sy(3), sx(6), sy(3));
            g.DrawLine(&pen, sx(6), sy(3), sx(7), sy(7));
            // 篮筐上沿 — 直线 (7,7)→(21,7)
            g.DrawLine(&pen, sx(7), sy(7), sx(21), sy(7));
            // 篮筐右斜下 (21,7)→(18,15)
            g.DrawLine(&pen, sx(21), sy(7), sx(18), sy(15));
            // 篮筐左斜下 (7,7)→(8.5,15)
            g.DrawLine(&pen, sx(7), sy(7), sx(8), sy(15));
            // 篮筐底沿 (8,15)→(18,15)
            g.DrawLine(&pen, sx(8), sy(15), sx(18), sy(15));
            // 两轮子
            g.DrawEllipse(&pen, sx(8) - sw(1.5f), sy(19) - sw(1.5f), sw(3), sw(3));
            g.DrawEllipse(&pen, sx(17) - sw(1.5f), sy(19) - sw(1.5f), sw(3), sw(3));
        }
        break;
    }
}

}  // namespace icons
