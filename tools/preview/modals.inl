// 各种 modal — CS2 详情 (点 game-card 弹) + History redo (460 wide row-item) + UserMenu status fold.
#pragma once

namespace modal {

bool g_cs2_open = false;
Tween g_cs2_t;

void openCS2() {
    g_cs2_open = true;
    g_cs2_t.start(g_cs2_t.value(), 1.0f, 0.30f, 0, curve::easeOutBack);
}
void closeCS2() {
    g_cs2_open = false;
    g_cs2_t.start(g_cs2_t.value(), 0.0f, 0.20f, 0, curve::easeOutCubic);
}

// CS2 thumb cache
Image* g_cs2_thumb = nullptr;
bool g_cs2_thumb_loaded = false;
void ensureCS2Thumb() {
    if (g_cs2_thumb_loaded) return;
    g_cs2_thumb_loaded = true;
    wchar_t exe_dir[MAX_PATH] = {0};
    GetModuleFileNameW(nullptr, exe_dir, MAX_PATH);
    std::wstring path = exe_dir;
    auto p = path.find_last_of(L'\\');
    if (p != std::wstring::npos) path = path.substr(0, p);
    // 找 ../../assets/images/games/cs2_header.jpg
    std::wstring candidate = path + L"\\..\\..\\assets\\images\\games\\cs2_header.jpg";
    g_cs2_thumb = Image::FromFile(candidate.c_str());
    if (!g_cs2_thumb || g_cs2_thumb->GetLastStatus() != Ok) {
        delete g_cs2_thumb; g_cs2_thumb = nullptr;
        // 备选：当前目录
        candidate = path + L"\\cs2_header.jpg";
        g_cs2_thumb = Image::FromFile(candidate.c_str());
        if (g_cs2_thumb && g_cs2_thumb->GetLastStatus() != Ok) {
            delete g_cs2_thumb; g_cs2_thumb = nullptr;
        }
    }
}

// 画 CS2 详情 modal — 比 V 社官网更好看
void paintCS2Modal(Graphics& g, int Wpx, int Hpx) {
    float t = g_cs2_t.value();
    if (t < 0.001f && !g_cs2_open) return;
    const Palette& pal = palette();
    Color dim((BYTE)(170 * t), 0, 0, 0);
    SolidBrush bg(dim);
    g.FillRectangle(&bg, 0, 0, Wpx, Hpx);

    auto fade = [&](Color c) { return Color((BYTE)(c.GetA() * t), c.GetR(), c.GetG(), c.GetB()); };
    float mw = std::min(720.0f, (float)Wpx - 80.0f);
    float mh = std::min(560.0f, (float)Hpx - 80.0f);
    float mx = (Wpx - mw) / 2;
    float my = (Hpx - mh) / 2 + 12 * (1.0f - t);

    drawShadow(g, mx, my, mw, mh, 16, fade(Color(160, 0, 0, 0)), 8, 6);
    fillRR(g, mx, my, mw, mh, 16, fade(pal.card));

    // 顶部封面 — 优先用图，否则渐变
    float cover_h = 220;
    GraphicsPath cover_clip; buildRoundRect(cover_clip, mx, my, mw, cover_h + 16, 16);
    g.SetClip(&cover_clip);

    ensureCS2Thumb();
    if (g_cs2_thumb) {
        // 等比 cover
        float iw = (float)g_cs2_thumb->GetWidth();
        float ih = (float)g_cs2_thumb->GetHeight();
        float scale = std::max(mw / iw, cover_h / ih);
        float dw = iw * scale, dh = ih * scale;
        float dx = mx + (mw - dw) / 2;
        float dy = my + (cover_h - dh) / 2;
        ImageAttributes attr;
        ColorMatrix mat = {
            1,0,0,0,0,
            0,1,0,0,0,
            0,0,1,0,0,
            0,0,0, t,0,
            0,0,0,0,1
        };
        attr.SetColorMatrix(&mat, ColorMatrixFlagsDefault, ColorAdjustTypeBitmap);
        g.DrawImage(g_cs2_thumb, RectF(dx, dy, dw, dh), 0, 0, iw, ih, UnitPixel, &attr);
    } else {
        LinearGradientBrush base(PointF(mx, my), PointF(mx + mw, my + cover_h),
            Color((BYTE)(255 * t), 0x2C, 0x28, 0x25),
            Color((BYTE)(255 * t), 0x1F, 0x1C, 0x19));
        GraphicsPath cp; buildRoundRect(cp, mx, my, mw, cover_h + 16, 16);
        g.FillPath(&base, &cp);
        Font cf(kFontFace, 60.0f, FontStyleBold, UnitPoint);
        SolidBrush csb(fade(Color(255, 0xF5, 0xC4, 0x4C)));
        StringFormat csf; csf.SetAlignment(StringAlignmentCenter); csf.SetLineAlignment(StringAlignmentCenter);
        g.DrawString(L"CS", -1, &cf, RectF(mx, my, mw, cover_h), &csf, &csb);
    }

    // 黑色渐变蒙版（底部 → 透明）
    LinearGradientBrush vmask(PointF(mx, my + cover_h - 80), PointF(mx, my + cover_h),
        Color(0, 0, 0, 0), Color((BYTE)(180 * t), 0, 0, 0));
    g.FillRectangle(&vmask, mx, my + cover_h - 80, mw, 80.0f);

    g.ResetClip();

    // play btn 中央
    float pby = my + cover_h / 2 - 28;
    float pbx = mx + mw / 2 - 28;
    bool play_hov = inRect(g_mouse, RectF(pbx, pby, 56.0f, 56.0f));
    SolidBrush pbg(fade(Color(200, 0, 0, 0)));
    g.FillEllipse(&pbg, pbx, pby, 56.0f, 56.0f);
    Pen pring(fade(Color(255, 255, 255, 255)), 2.0f);
    g.DrawEllipse(&pring, pbx, pby, 56.0f, 56.0f);
    icons::drawSvg(g, icons::Name::Play, pbx + 16, pby + 16, 24, fade(Color(255, 255, 255, 255)));
    if (play_hov) {
        SolidBrush hbg(fade(Color(50, 255, 255, 255)));
        g.FillEllipse(&hbg, pbx, pby, 56.0f, 56.0f);
    }
    // 点击 play — 暂不真打开外部，避免外部 handler 异常 / focus 抖动；先关 modal
    hit(RectF(pbx, pby, 56.0f, 56.0f), [](){ closeCS2(); }, true);

    // 关闭 ✕ 右上
    RectF xr(mx + mw - 36, my + 12, 26, 26);
    bool xhov = inRect(g_mouse, xr);
    if (xhov) fillRR(g, xr.X, xr.Y, xr.Width, xr.Height, 6, fade(Color(60, 0, 0, 0)));
    icons::drawSvg(g, icons::Name::X, xr.X + 4, xr.Y + 4, 18, fade(Color(255, 255, 255, 255)));
    hit(xr, [](){ closeCS2(); }, true);

    // 内容区
    float bx = mx + 28;
    float by = my + cover_h + 18;
    drawText_(g, L"Counter-Strike 2", bx, by, mw - 56,
              18.0f, fade(pal.text), StringAlignmentNear, FontStyleBold);
    drawText_(g, L"Valve · 1.40.1.5  ·  CS2 (csgo 迷徒重集 你已被剧导调路)",
              bx, by + 28, mw - 56, 9.0f, fade(pal.text_muted));

    // 三段 stat
    float sty = by + 56;
    struct Stat { const wchar_t* k; const wchar_t* v; };
    Stat stats[] = {
        { L"上次启动", L"05-02 10:32" },
        { L"游戏时长", L"1284 小时" },
        { L"已订阅",   L"1 周" },
    };
    float sw = (mw - 56) / 3;
    for (int i = 0; i < 3; ++i) {
        float sx = bx + i * sw;
        drawText_(g, stats[i].k, sx, sty, sw, 8.0f, fade(pal.text_muted));
        drawText_(g, stats[i].v, sx, sty + 14, sw, 11.0f, fade(pal.text),
                  StringAlignmentNear, FontStyleBold);
    }

    // 描述
    drawText_(g, L"经典战术 FPS 已升级至 Source 2 引擎。本订阅含完整启动器接管 + cfg 管控。\n点击播放按钮观看官方宣传片，或通过下方按钮直接启动 Steam 上的 CS2。",
              bx, sty + 56, mw - 56, 8.5f, fade(pal.text_muted));

    // 底部按钮
    float btny = my + mh - 60;
    RectF launch(bx, btny, 140, 40);
    bool lhov = inRect(g_mouse, launch);
    Color lbg = lhov ? fade(pal.primary_hover) : fade(pal.primary);
    Color lglow(80, pal.primary.GetR(), pal.primary.GetG(), pal.primary.GetB());
    drawShadow(g, launch.X, launch.Y, launch.Width, launch.Height, 10, lglow, 4, 3);
    fillRR(g, launch.X, launch.Y, launch.Width, launch.Height, 10, lbg);
    icons::drawSvg(g, icons::Name::Play, launch.X + 14, launch.Y + 11, 18,
                   Color(255, 255, 255, 255));
    drawText_(g, L"启动", launch.X + 36, launch.Y + 13, launch.Width - 36, 10.0f,
              Color((BYTE)(255 * t), 255, 255, 255), StringAlignmentNear, FontStyleBold);
    hit(launch, [](){
        // 真启动用 ShellExecute 打开 steam:// 协议
        ShellExecuteW(nullptr, L"open", L"steam://run/730", nullptr, nullptr, SW_SHOWNORMAL);
    }, true);

    RectF stm(bx + 152, btny, 160, 40);
    bool shov = inRect(g_mouse, stm);
    Color sbgC = shov ? fade(pal.bg) : fade(pal.card);
    fillRR(g, stm.X, stm.Y, stm.Width, stm.Height, 10, sbgC);
    strokeRR(g, stm.X, stm.Y, stm.Width, stm.Height, 10, fade(pal.divider));
    icons::drawSvg(g, icons::Name::Link, stm.X + 14, stm.Y + 11, 18, fade(pal.text));
    drawText_(g, L"Steam 商店页", stm.X + 36, stm.Y + 13, stm.Width - 36, 9.5f, fade(pal.text),
              StringAlignmentNear, FontStyleBold);
    hit(stm, [](){
        ShellExecuteW(nullptr, L"open",
            L"https://store.steampowered.com/app/730/CounterStrike_2/",
            nullptr, nullptr, SW_SHOWNORMAL);
    }, true);

    // 点 modal 外关闭
    hit(RectF(0, 0, (REAL)Wpx, (REAL)Hpx), [Wpx, Hpx, mx, my, mw, mh](){
        if (!inRect(g_mouse, RectF(mx, my, mw, mh))) closeCS2();
    }, true);
}

// ============================================================
// History modal redo — 460 wide design row-item
// ============================================================
struct HistRow {
    const wchar_t* device;
    const wchar_t* ip;
    const wchar_t* loc;
    const wchar_t* when;
};
const HistRow kHist[] = {
    { L"Windows · ThinkBook", L"118.112.34.6",  L"成都, 中国",     L"05-02 10:32" },
    { L"Windows · ThinkBook", L"118.112.34.6",  L"成都, 中国",     L"05-01 22:08" },
    { L"Windows · Office",    L"203.45.67.89",  L"北京, 中国",     L"04-29 14:22" },
    { L"Windows · ThinkBook", L"118.112.34.6",  L"成都, 中国",     L"04-28 09:15" },
    { L"macOS · MBP",         L"192.168.1.87",  L"本地",           L"04-25 18:51" },
    { L"Windows · ThinkBook", L"118.112.34.6",  L"成都, 中国",     L"04-23 11:04" },
    { L"iPad · Safari",       L"118.112.34.10", L"成都, 中国",     L"04-21 20:30" },
    { L"Windows · Office",    L"203.45.67.89",  L"北京, 中国",     L"04-19 09:48" },
    { L"Windows · ThinkBook", L"118.112.34.6",  L"成都, 中国",     L"04-17 14:12" },
    { L"Linux · WSL",         L"118.112.34.6",  L"成都, 中国",     L"04-15 22:01" },
    { L"Windows · ThinkBook", L"118.112.34.6",  L"成都, 中国",     L"04-13 08:55" },
    { L"Android · Pixel",     L"117.140.22.5",  L"上海, 中国",     L"04-10 16:39" },
    { L"Windows · Office",    L"203.45.67.89",  L"北京, 中国",     L"04-08 10:01" },
};
const int kHistTotal = sizeof(kHist) / sizeof(kHist[0]);
const int kHistPer = 5;
int g_hist_page = 0;

void paintHistoryModalNew(Graphics& g, int Wpx, int Hpx) {
    const Palette& pal = palette();
    float t = g_overlay_t.value();
    if (t < 0.001f) return;
    auto fade = [&](Color c) { return Color((BYTE)(c.GetA() * t), c.GetR(), c.GetG(), c.GetB()); };

    Color dim((BYTE)(180 * t), 0, 0, 0);
    SolidBrush bg(dim);
    g.FillRectangle(&bg, 0, 0, Wpx, Hpx);

    float mw = 460.0f;
    if (mw > Wpx - 40) mw = Wpx - 40.0f;
    float mh = std::min(520.0f, (float)Hpx - 60.0f);
    float mx = (Wpx - mw) / 2;
    float my = (Hpx - mh) / 2 + 12 * (1.0f - t);

    drawShadow(g, mx, my, mw, mh, 16, fade(Color(180, 0, 0, 0)), 8, 6);
    fillRR(g, mx, my, mw, mh, 16, fade(pal.card));

    // h3 + sub
    drawText_(g, L"登录历史", mx + 24, my + 22, mw - 48, 13.0f,
              fade(pal.text), StringAlignmentNear, FontStyleBold);
    wchar_t sub[64]; swprintf_s(sub, 64, L"%d 条记录", kHistTotal);
    drawText_(g, sub, mx + 24, my + 46, mw - 48, 9.0f, fade(pal.text_muted));

    // x
    RectF xr(mx + mw - 38, my + 16, 26, 26);
    bool xhov = inRect(g_mouse, xr);
    if (xhov) fillRR(g, xr.X, xr.Y, xr.Width, xr.Height, 6, fade(pal.bg));
    icons::drawSvg(g, icons::Name::X, xr.X + 4, xr.Y + 4, 18, fade(pal.text_muted));
    hit(xr, [](){
        g_overlay = Overlay::None;
        g_overlay_t.start(g_overlay_t.value(), 0, 0.20f, 0, curve::easeOutCubic);
    }, true);

    // rows — 5/page
    int pages = (kHistTotal + kHistPer - 1) / kHistPer;
    if (g_hist_page >= pages) g_hist_page = pages - 1;
    int start = g_hist_page * kHistPer;
    int end = std::min(start + kHistPer, kHistTotal);

    float ry = my + 80;
    for (int i = start; i < end; ++i) {
        // top border between rows (除第一行)
        if (i != start) {
            Pen sep(fade(pal.divider), 1.0f);
            g.DrawLine(&sep, mx + 24, ry, mx + mw - 24, ry);
        }
        // device + ip line + when 右
        const auto& r = kHist[i];
        drawText_(g, r.device, mx + 24, ry + 12, mw - 168, 9.5f,
                  fade(pal.text), StringAlignmentNear, FontStyleBold);
        wchar_t ipline[128]; swprintf_s(ipline, 128, L"%ls · %ls", r.ip, r.loc);
        drawText_(g, ipline, mx + 24, ry + 30, mw - 168, 8.0f, fade(pal.text_muted));
        // when (mono)
        Font mf(L"DejaVu Sans Mono", 9.0f, FontStyleRegular, UnitPoint);
        SolidBrush mb(fade(pal.text_muted));
        StringFormat mfmt; mfmt.SetAlignment(StringAlignmentFar);
        g.DrawString(r.when, -1, &mf, RectF(mx + mw - 140, ry + 18, 116, 16), &mfmt, &mb);
        ry += 56;
    }

    // pager
    float pgy = my + mh - 80;
    float btnw = 28, gap = 6;
    float total_w = btnw * (pages + 2) + gap * (pages + 1);
    float pgx = mx + (mw - total_w) / 2;
    {
        // ‹
        bool can_prev = g_hist_page > 0;
        fillRR(g, pgx, pgy, btnw, btnw, 6, fade(Color(0, 0, 0, 0)));
        strokeRR(g, pgx, pgy, btnw, btnw, 6, fade(pal.divider));
        Color prevC = can_prev ? fade(pal.text) : Color((BYTE)(80 * t), pal.text.GetR(), pal.text.GetG(), pal.text.GetB());
        drawText_(g, L"‹", pgx, pgy + 6, btnw, 10.0f, prevC, StringAlignmentCenter, FontStyleBold);
        if (can_prev) hit(RectF(pgx, pgy, btnw, btnw), [](){ if (g_hist_page > 0) g_hist_page--; }, true);
        pgx += btnw + gap;
        for (int p = 0; p < pages; ++p) {
            bool on = (p == g_hist_page);
            Color bgC = on ? fade(pal.primary) : fade(Color(0, 0, 0, 0));
            Color fg = on ? Color((BYTE)(255 * t), 255, 255, 255) : fade(pal.text);
            Color bd = on ? fade(pal.primary) : fade(pal.divider);
            fillRR(g, pgx, pgy, btnw, btnw, 6, bgC);
            strokeRR(g, pgx, pgy, btnw, btnw, 6, bd);
            wchar_t num[8]; swprintf_s(num, 8, L"%d", p + 1);
            drawText_(g, num, pgx, pgy + 6, btnw, 9.5f, fg, StringAlignmentCenter, FontStyleBold);
            int target = p;
            hit(RectF(pgx, pgy, btnw, btnw), [target](){ g_hist_page = target; }, true);
            pgx += btnw + gap;
        }
        bool can_next = g_hist_page < pages - 1;
        fillRR(g, pgx, pgy, btnw, btnw, 6, fade(Color(0, 0, 0, 0)));
        strokeRR(g, pgx, pgy, btnw, btnw, 6, fade(pal.divider));
        Color nextC = can_next ? fade(pal.text) : Color((BYTE)(80 * t), pal.text.GetR(), pal.text.GetG(), pal.text.GetB());
        drawText_(g, L"›", pgx, pgy + 6, btnw, 10.0f, nextC, StringAlignmentCenter, FontStyleBold);
        if (can_next) hit(RectF(pgx, pgy, btnw, btnw), [pages](){ if (g_hist_page < pages - 1) g_hist_page++; }, true);
    }

    // 关闭按钮 (Ghost)
    RectF cb(mx + mw - 92, my + mh - 50, 70, 32);
    bool chov = inRect(g_mouse, cb);
    fillRR(g, cb.X, cb.Y, cb.Width, cb.Height, 8, chov ? fade(pal.bg) : fade(Color(0,0,0,0)));
    strokeRR(g, cb.X, cb.Y, cb.Width, cb.Height, 8, fade(pal.divider));
    drawText_(g, L"关闭", cb.X, cb.Y + 9, cb.Width, 9.5f, fade(pal.text),
              StringAlignmentCenter, FontStyleBold);
    hit(cb, [](){
        g_overlay = Overlay::None;
        g_overlay_t.start(g_overlay_t.value(), 0, 0.20f, 0, curve::easeOutCubic);
    }, true);

    // 点外部关闭
    hit(RectF(0, 0, (REAL)Wpx, (REAL)Hpx), [mx, my, mw, mh](){
        if (!inRect(g_mouse, RectF(mx, my, mw, mh))) {
            g_overlay = Overlay::None;
            g_overlay_t.start(g_overlay_t.value(), 0, 0.20f, 0, curve::easeOutCubic);
        }
    }, true);
}

}  // namespace modal
