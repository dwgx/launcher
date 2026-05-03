// 各种 modal — CS2 详情 (点 game-card 弹) + History redo (460 wide row-item)
//   + 修改密码 modal (3 field 老/新/确认 → POST /api/profile/password)
#pragma once

namespace modal {

// ============================================================
// 修改密码 modal
// ============================================================
struct ChangePw {
    bool open = false;
    tx::Slide t;          // modal 入场：translateY 16→0 + op 0↔1
    InputBox old_pw, new_pw, confirm_pw;
    int focus = 0;
    std::wstring error;
    bool busy = false;
};
inline ChangePw& g_pw() { static ChangePw s; return s; }

void openChangePw() {
    auto& p = g_pw();
    p.open = true;
    p.t.enter(0.0f, 16.0f, 0.28f);
    p.old_pw.text.clear();   p.old_pw.cursor = 0;   p.old_pw.clearSel();   p.old_pw.password = true;
    p.new_pw.text.clear();   p.new_pw.cursor = 0;   p.new_pw.clearSel();   p.new_pw.password = true;
    p.confirm_pw.text.clear(); p.confirm_pw.cursor = 0; p.confirm_pw.clearSel(); p.confirm_pw.password = true;
    p.focus = 0;
    p.error.clear();
    p.busy = false;
}
void closeChangePw() {
    auto& p = g_pw();
    p.open = false;
    p.t.exit(0.0f, 16.0f, 0.20f);
}

void paintChangePwModal(Graphics& g, int Wpx, int Hpx) {
    auto& p = g_pw();
    if (p.t.value() < 0.001f && !p.open) return;
    const Palette& pal = palette();
    float t = p.t.value();
    if (t < 0.001f) return;

    auto fade = [&](Color c) { return Color((BYTE)(c.GetA() * t), c.GetR(), c.GetG(), c.GetB()); };

    SolidBrush dim(Color((BYTE)(170 * t), 0, 0, 0));
    g.FillRectangle(&dim, 0, 0, Wpx, Hpx);

    float mw = 420.0f, mh = 380.0f;
    float mx = (Wpx - mw) / 2.0f;
    float my = (Hpx - mh) / 2.0f + p.t.dy();

    drawShadow(g, mx, my, mw, mh, 16.0f, fade(Color(160, 0, 0, 0)), 6.0f, 4);
    fillRR(g, mx, my, mw, mh, 16.0f, fade(pal.card));

    drawText_(g, L"修改密码", mx + 28, my + 24, mw - 56, 16.0f,
              fade(pal.text), StringAlignmentNear, FontStyleBold);
    drawText_(g, L"修改后所有设备都会自动下线，需重新登录", mx + 28, my + 50, mw - 56,
              8.5f, fade(pal.text_muted));

    // 关闭 ✕
    RectF xr(mx + mw - 38, my + 16, 26, 26);
    bool xhov = inRect(g_mouse, xr);
    if (xhov) fillRR(g, xr.X, xr.Y, xr.Width, xr.Height, 6.0f, fade(pal.bg));
    icons::drawSvg(g, icons::Name::X, xr.X + 4, xr.Y + 4, 18.0f, fade(pal.text_muted));
    hit(xr, [](){ closeChangePw(); }, true);

    // 3 个 password field
    auto field = [&](InputBox& box, float fy, const wchar_t* label, int idx) {
        bool focused = (p.focus == idx);
        RectF fr(mx + 28, fy, mw - 56, 44);
        box.bounds = fr;
        fillRR(g, fr.X, fr.Y, fr.Width, fr.Height, 10.0f, fade(pal.bg));
        strokeRR(g, fr.X, fr.Y, fr.Width, fr.Height, 10.0f,
                 focused ? fade(pal.primary) : fade(pal.divider),
                 focused ? 1.4f : 1.0f);
        if (focused) {
            Color halo((BYTE)(38 * t), pal.primary.GetR(), pal.primary.GetG(), pal.primary.GetB());
            strokeRR(g, fr.X - 2, fr.Y - 2, fr.Width + 4, fr.Height + 4, 12.0f, halo, 4.0f);
        }
        // label 浮上 / 居中
        bool floating = focused || !box.text.empty();
        float lab_y = floating ? (fr.Y + 6.0f) : (fr.Y + 14.0f);
        float lab_size = floating ? 8.0f : 11.0f;
        Color lab_c = floating ? fade(pal.primary) : fade(pal.text_muted);
        drawText_(g, label, fr.X + 14, lab_y, fr.Width - 28, lab_size, lab_c,
                  StringAlignmentNear, floating ? FontStyleBold : FontStyleRegular);

        // text + 选区
        if (!box.text.empty()) {
            Font* f = fontcache::get(10.0f);
            // 选区高亮
            if (focused && box.hasSelection()) {
                RectF bp, bi;
                g.MeasureString(box.displaySlice(0, box.selStart()).c_str(), -1, f,
                                PointF(0, 0), &bp);
                g.MeasureString(box.displaySlice(box.selStart(), box.selEnd()).c_str(), -1, f,
                                PointF(0, 0), &bi);
                Color sb(96, pal.primary.GetR(), pal.primary.GetG(), pal.primary.GetB());
                SolidBrush sbR(sb);
                g.FillRectangle(&sbR, fr.X + 14 + bp.Width, fr.Y + 22, bi.Width, 16.0f);
            }
            drawText_(g, box.display().c_str(), fr.X + 14, fr.Y + 22, fr.Width - 28,
                      10.0f, fade(pal.text));
        }
        // caret
        if (focused && !box.hasSelection()) {
            Font* f = fontcache::get(10.0f);
            RectF bb;
            g.MeasureString(box.displaySlice(0, box.cursor).c_str(), -1, f, PointF(0, 0), &bb);
            int phase = (int)(g_time_in_stage * 1000) % 1000;
            if (phase < 500) {
                Pen pen(fade(pal.primary), 1.5f);
                float cx_ = fr.X + 14 + bb.Width;
                g.DrawLine(&pen, cx_, fr.Y + 22, cx_, fr.Y + 38);
            }
        }
        int idx_capt = idx;
        hit(fr, [idx_capt](){ g_pw().focus = idx_capt; }, true);
    };
    field(p.old_pw,     my + 80,  L"旧密码",   0);
    field(p.new_pw,     my + 138, L"新密码",   1);
    field(p.confirm_pw, my + 196, L"确认新密码", 2);

    // error
    if (!p.error.empty()) {
        Color ebg((BYTE)(36 * t), 0xE3, 0x4B, 0x4B);
        Color efg((BYTE)(255 * t), 0xFF, 0x8A, 0x80);
        fillRR(g, mx + 28, my + 250, mw - 56, 28, 8.0f, ebg);
        drawText_(g, p.error.c_str(), mx + 38, my + 257, mw - 76, 9.0f, efg);
    }

    // 按钮 — 取消 / 保存
    float by = my + mh - 56;
    RectF cb(mx + 28, by, 100, 36);
    bool ch = inRect(g_mouse, cb);
    fillRR(g, cb.X, cb.Y, cb.Width, cb.Height, 8.0f, ch ? fade(pal.bg) : fade(pal.card));
    strokeRR(g, cb.X, cb.Y, cb.Width, cb.Height, 8.0f, fade(pal.divider));
    drawText_(g, L"取消", cb.X, cb.Y + 12, cb.Width, 9.5f, fade(pal.text),
              StringAlignmentCenter, FontStyleBold);
    hit(cb, [](){ closeChangePw(); }, true);

    RectF sb(mx + mw - 28 - 120, by, 120, 36);
    bool sh = inRect(g_mouse, sb);
    Color sbg = p.busy ? fade(Color(255, 0x6B, 0x6A, 0x67))
                       : (sh ? fade(pal.primary_hover) : fade(pal.primary));
    fillRR(g, sb.X, sb.Y, sb.Width, sb.Height, 8.0f, sbg);
    drawText_(g, p.busy ? L"提交中…" : L"保存", sb.X, sb.Y + 12, sb.Width, 9.5f,
              Color((BYTE)(255 * t), 255, 255, 255), StringAlignmentCenter, FontStyleBold);
    if (!p.busy) {
        hit(sb, [](){
            auto& pp = g_pw();
            pp.error.clear();
            if (pp.new_pw.text.size() < 8) { pp.error = L"新密码至少 8 字"; return; }
            if (pp.new_pw.text != pp.confirm_pw.text) { pp.error = L"两次密码不一致"; return; }
            if (g_session_token.empty()) { pp.error = L"未登录，无法修改"; return; }
            pp.busy = true;
            // 异步 POST
            struct A { std::wstring oldp, newp; HWND h; };
            A* a = new A{pp.old_pw.text, pp.new_pw.text, g_hwnd};
            CreateThread(nullptr, 0, [](LPVOID lp) -> DWORD {
                auto* a = (A*)lp;
                std::string body = std::string("{\"session_token\":\"") + g_session_token
                    + "\",\"old_password\":\"" + net::jsonEscape(a->oldp)
                    + "\",\"new_password\":\"" + net::jsonEscape(a->newp) + "\"}";
                auto r = net::postJson(L"/api/profile/password", body);
                PostMessageW(a->h, WM_APP + 4, r.ok() ? 1 : 0,
                             (LPARAM)(intptr_t)(r.status));
                delete a;
                return 0;
            }, a, 0, nullptr);
        }, true);
    }

    // 点外 4 环形关闭（避免阻断内部 hit）
    hit(RectF(0, 0, (REAL)Wpx, my), [](){ closeChangePw(); }, true);
    hit(RectF(0, my + mh, (REAL)Wpx, Hpx - (my + mh)), [](){ closeChangePw(); }, true);
    hit(RectF(0, my, mx, mh), [](){ closeChangePw(); }, true);
    hit(RectF(mx + mw, my, Wpx - (mx + mw), mh), [](){ closeChangePw(); }, true);
}

// ============================================================
// 添加个人标签 modal — 接 /api/profile/tags/add
// ============================================================
struct AddTag {
    bool open = false;
    tx::Slide t;
    InputBox input;
    std::wstring error;
    bool busy = false;
};
inline AddTag& g_tag() { static AddTag s; return s; }

void openAddTag() {
    auto& p = g_tag();
    p.open = true;
    p.t.enter(0.0f, 14.0f, 0.26f);
    p.input.text.clear(); p.input.cursor = 0; p.input.clearSel();
    p.error.clear();
    p.busy = false;
}
void closeAddTag() {
    auto& p = g_tag();
    p.open = false;
    p.t.exit(0.0f, 14.0f, 0.18f);
}

// 异步 POST /api/profile/tags/add — 实现在主程序（全局命名空间），这里 forward。

void paintAddTagModal(Graphics& g, int Wpx, int Hpx) {
    auto& p = g_tag();
    if (p.t.value() < 0.001f && !p.open) return;
    const Palette& pal = palette();
    float t = p.t.value();
    if (t < 0.001f) return;
    auto fade = [&](Color c) { return Color((BYTE)(c.GetA() * t), c.GetR(), c.GetG(), c.GetB()); };

    SolidBrush dim(Color((BYTE)(170 * t), 0, 0, 0));
    g.FillRectangle(&dim, 0, 0, Wpx, Hpx);

    float mw = 380.0f, mh = 230.0f;
    float mx = (Wpx - mw) / 2.0f;
    float my = (Hpx - mh) / 2.0f + p.t.dy();

    drawShadow(g, mx, my, mw, mh, 16.0f, fade(Color(160, 0, 0, 0)), 6.0f, 4);
    fillRR(g, mx, my, mw, mh, 16.0f, fade(pal.card));

    drawText_(g, L"添加标签", mx + 26, my + 22, mw - 52, 14.0f,
              fade(pal.text), StringAlignmentNear, FontStyleBold);
    drawText_(g, L"展示在主页个人卡片上 — 例如 \"CS2\" / \"东京机房\"",
              mx + 26, my + 48, mw - 52, 8.5f, fade(pal.text_muted));

    // 关闭 ✕
    RectF xr(mx + mw - 38, my + 16, 26, 26);
    bool xhov = inRect(g_mouse, xr);
    if (xhov) fillRR(g, xr.X, xr.Y, xr.Width, xr.Height, 6.0f, fade(pal.bg));
    icons::drawSvg(g, icons::Name::X, xr.X + 4, xr.Y + 4, 18.0f, fade(pal.text_muted));
    hit(xr, [](){ closeAddTag(); }, true);

    // input field
    RectF fr(mx + 26, my + 80, mw - 52, 44);
    p.input.bounds = fr;
    fillRR(g, fr.X, fr.Y, fr.Width, fr.Height, 10.0f, fade(pal.bg));
    strokeRR(g, fr.X, fr.Y, fr.Width, fr.Height, 10.0f, fade(pal.primary), 1.4f);
    Color halo((BYTE)(38 * t), pal.primary.GetR(), pal.primary.GetG(), pal.primary.GetB());
    strokeRR(g, fr.X - 2, fr.Y - 2, fr.Width + 4, fr.Height + 4, 12.0f, halo, 4.0f);

    if (p.input.text.empty()) {
        drawText_(g, L"标签内容（最多 24 字）", fr.X + 14, fr.Y + 14, fr.Width - 28,
                  10.0f, fade(pal.text_muted));
    } else {
        Font* f = fontcache::get(10.0f);
        drawText_(g, p.input.text.c_str(), fr.X + 14, fr.Y + 14, fr.Width - 28,
                  10.0f, fade(pal.text));
        // caret
        if (!p.input.hasSelection()) {
            RectF bb;
            g.MeasureString(p.input.displaySlice(0, p.input.cursor).c_str(), -1, f, PointF(0, 0), &bb);
            int phase = (int)(g_time_in_stage * 1000) % 1000;
            if (phase < 500) {
                Pen pen(fade(pal.primary), 1.5f);
                float cx_ = fr.X + 14 + bb.Width;
                g.DrawLine(&pen, cx_, fr.Y + 14, cx_, fr.Y + 30);
            }
        }
    }
    hit(fr, [](){}, true);   // 阻断外部 dismiss

    if (!p.error.empty()) {
        Color ebg((BYTE)(36 * t), 0xE3, 0x4B, 0x4B);
        Color efg((BYTE)(255 * t), 0xFF, 0x8A, 0x80);
        fillRR(g, mx + 26, my + 132, mw - 52, 24, 6.0f, ebg);
        drawText_(g, p.error.c_str(), mx + 36, my + 137, mw - 72, 8.5f, efg);
    }

    // 按钮
    float by = my + mh - 52;
    RectF cb(mx + 26, by, 100, 32);
    bool ch = inRect(g_mouse, cb);
    fillRR(g, cb.X, cb.Y, cb.Width, cb.Height, 8.0f, ch ? fade(pal.bg) : fade(pal.card));
    strokeRR(g, cb.X, cb.Y, cb.Width, cb.Height, 8.0f, fade(pal.divider));
    drawText_(g, L"取消", cb.X, cb.Y + 9, cb.Width, 9.5f, fade(pal.text),
              StringAlignmentCenter, FontStyleBold);
    hit(cb, [](){ closeAddTag(); }, true);

    RectF sb(mx + mw - 26 - 110, by, 110, 32);
    bool sh = inRect(g_mouse, sb);
    Color sbg = p.busy ? fade(Color(255, 0x6B, 0x6A, 0x67))
                       : (sh ? fade(pal.primary_hover) : fade(pal.primary));
    fillRR(g, sb.X, sb.Y, sb.Width, sb.Height, 8.0f, sbg);
    drawText_(g, p.busy ? L"提交中…" : L"添加", sb.X, sb.Y + 9, sb.Width, 9.5f,
              Color((BYTE)(255 * t), 255, 255, 255), StringAlignmentCenter, FontStyleBold);
    if (!p.busy) {
        hit(sb, [](){ ::submitAddTag(); }, true);
    }

    // 4 环形外部 dismiss
    hit(RectF(0, 0, (REAL)Wpx, my), [](){ closeAddTag(); }, true);
    hit(RectF(0, my + mh, (REAL)Wpx, Hpx - (my + mh)), [](){ closeAddTag(); }, true);
    hit(RectF(0, my, mx, mh), [](){ closeAddTag(); }, true);
    hit(RectF(mx + mw, my, Wpx - (mx + mw), mh), [](){ closeAddTag(); }, true);
}

// ============================================================
// 创建表情包分组 modal — 接 POST /api/sticker/pack
// ============================================================
struct CreatePack {
    bool open = false;
    tx::Slide t;
    InputBox input;
    std::wstring error;
    bool busy = false;
};
inline CreatePack& g_create_pack() { static CreatePack s; return s; }

void openCreatePack() {
    auto& p = g_create_pack();
    p.open = true;
    p.t.enter(0.0f, 14.0f, 0.26f);
    p.input.text.clear(); p.input.cursor = 0; p.input.clearSel();
    p.error.clear();
    p.busy = false;
}
void closeCreatePack() {
    auto& p = g_create_pack();
    p.open = false;
    p.t.exit(0.0f, 14.0f, 0.18f);
}

void paintCreatePackModal(Graphics& g, int Wpx, int Hpx) {
    auto& p = g_create_pack();
    if (p.t.value() < 0.001f && !p.open) return;
    const Palette& pal = palette();
    float t = p.t.value();
    if (t < 0.001f) return;
    auto fade = [&](Color c) { return Color((BYTE)(c.GetA() * t), c.GetR(), c.GetG(), c.GetB()); };

    SolidBrush dim(Color((BYTE)(170 * t), 0, 0, 0));
    g.FillRectangle(&dim, 0, 0, Wpx, Hpx);

    float mw = 380.0f, mh = 230.0f;
    float mx = (Wpx - mw) / 2.0f;
    float my = (Hpx - mh) / 2.0f + p.t.dy();

    drawShadow(g, mx, my, mw, mh, 16.0f, fade(Color(160, 0, 0, 0)), 6.0f, 4);
    fillRR(g, mx, my, mw, mh, 16.0f, fade(pal.card));

    drawText_(g, L"创建表情包分组", mx + 26, my + 22, mw - 52, 14.0f,
              fade(pal.text), StringAlignmentNear, FontStyleBold);
    drawText_(g, L"每组最多 25 张表情；可在分组里加图片 / GIF / 文件夹",
              mx + 26, my + 48, mw - 52, 8.5f, fade(pal.text_muted));

    RectF xr(mx + mw - 38, my + 16, 26, 26);
    bool xhov = inRect(g_mouse, xr);
    if (xhov) fillRR(g, xr.X, xr.Y, xr.Width, xr.Height, 6.0f, fade(pal.bg));
    icons::drawSvg(g, icons::Name::X, xr.X + 4, xr.Y + 4, 18.0f, fade(pal.text_muted));
    hit(xr, [](){ closeCreatePack(); }, true);

    RectF fr(mx + 26, my + 80, mw - 52, 44);
    p.input.bounds = fr;
    fillRR(g, fr.X, fr.Y, fr.Width, fr.Height, 10.0f, fade(pal.bg));
    strokeRR(g, fr.X, fr.Y, fr.Width, fr.Height, 10.0f, fade(pal.primary), 1.4f);
    Color halo((BYTE)(38 * t), pal.primary.GetR(), pal.primary.GetG(), pal.primary.GetB());
    strokeRR(g, fr.X - 2, fr.Y - 2, fr.Width + 4, fr.Height + 4, 12.0f, halo, 4.0f);
    if (p.input.text.empty()) {
        drawText_(g, L"分组名（最多 24 字）", fr.X + 14, fr.Y + 14, fr.Width - 28,
                  10.0f, fade(pal.text_muted));
    } else {
        Font* f = fontcache::get(10.0f);
        drawText_(g, p.input.text.c_str(), fr.X + 14, fr.Y + 14, fr.Width - 28,
                  10.0f, fade(pal.text));
        if (!p.input.hasSelection()) {
            RectF bb;
            g.MeasureString(p.input.displaySlice(0, p.input.cursor).c_str(), -1, f, PointF(0, 0), &bb);
            int phase = (int)(g_time_in_stage * 1000) % 1000;
            if (phase < 500) {
                Pen pen(fade(pal.primary), 1.5f);
                float cx_ = fr.X + 14 + bb.Width;
                g.DrawLine(&pen, cx_, fr.Y + 14, cx_, fr.Y + 30);
            }
        }
    }
    hit(fr, [](){}, true);

    if (!p.error.empty()) {
        Color ebg((BYTE)(36 * t), 0xE3, 0x4B, 0x4B);
        Color efg((BYTE)(255 * t), 0xFF, 0x8A, 0x80);
        fillRR(g, mx + 26, my + 132, mw - 52, 24, 6.0f, ebg);
        drawText_(g, p.error.c_str(), mx + 36, my + 137, mw - 72, 8.5f, efg);
    }

    float by = my + mh - 52;
    RectF cb(mx + 26, by, 100, 32);
    bool ch = inRect(g_mouse, cb);
    fillRR(g, cb.X, cb.Y, cb.Width, cb.Height, 8.0f, ch ? fade(pal.bg) : fade(pal.card));
    strokeRR(g, cb.X, cb.Y, cb.Width, cb.Height, 8.0f, fade(pal.divider));
    drawText_(g, L"取消", cb.X, cb.Y + 9, cb.Width, 9.5f, fade(pal.text),
              StringAlignmentCenter, FontStyleBold);
    hit(cb, [](){ closeCreatePack(); }, true);

    RectF sb(mx + mw - 26 - 110, by, 110, 32);
    bool sh = inRect(g_mouse, sb);
    Color sbg = p.busy ? fade(Color(255, 0x6B, 0x6A, 0x67))
                       : (sh ? fade(pal.primary_hover) : fade(pal.primary));
    fillRR(g, sb.X, sb.Y, sb.Width, sb.Height, 8.0f, sbg);
    drawText_(g, p.busy ? L"提交中…" : L"创建", sb.X, sb.Y + 9, sb.Width, 9.5f,
              Color((BYTE)(255 * t), 255, 255, 255), StringAlignmentCenter, FontStyleBold);
    if (!p.busy) {
        hit(sb, [](){ ::submitCreatePack(); }, true);
    }

    hit(RectF(0, 0, (REAL)Wpx, my), [](){ closeCreatePack(); }, true);
    hit(RectF(0, my + mh, (REAL)Wpx, Hpx - (my + mh)), [](){ closeCreatePack(); }, true);
    hit(RectF(0, my, mx, mh), [](){ closeCreatePack(); }, true);
    hit(RectF(mx + mw, my, Wpx - (mx + mw), mh), [](){ closeCreatePack(); }, true);
}

// ============================================================
// 重命名表情包分组 modal — 接 POST /api/sticker/pack/rename
// ============================================================
struct RenamePack {
    bool open = false;
    tx::Slide t;
    InputBox input;
    std::wstring error;
    bool busy = false;
    int  pack_idx = -1;
    std::string pack_id;
};
inline RenamePack& g_rename_pack() { static RenamePack s; return s; }

void openRenamePack(int pack_idx, const std::string& pack_id, const std::wstring& cur_name) {
    auto& p = g_rename_pack();
    p.open = true;
    p.t.enter(0.0f, 14.0f, 0.26f);
    p.input.text = cur_name;
    p.input.cursor = (int)cur_name.size();
    p.input.clearSel();
    p.error.clear();
    p.busy = false;
    p.pack_idx = pack_idx;
    p.pack_id = pack_id;
}
void closeRenamePack() {
    auto& p = g_rename_pack();
    p.open = false;
    p.t.exit(0.0f, 14.0f, 0.18f);
}

void paintRenamePackModal(Graphics& g, int Wpx, int Hpx) {
    auto& p = g_rename_pack();
    if (p.t.value() < 0.001f && !p.open) return;
    const Palette& pal = palette();
    float t = p.t.value();
    if (t < 0.001f) return;
    auto fade = [&](Color c) { return Color((BYTE)(c.GetA() * t), c.GetR(), c.GetG(), c.GetB()); };

    SolidBrush dim(Color((BYTE)(170 * t), 0, 0, 0));
    g.FillRectangle(&dim, 0, 0, Wpx, Hpx);

    float mw = 380.0f, mh = 210.0f;
    float mx = (Wpx - mw) / 2.0f;
    float my = (Hpx - mh) / 2.0f + p.t.dy();

    drawShadow(g, mx, my, mw, mh, 16.0f, fade(Color(160, 0, 0, 0)), 6.0f, 4);
    fillRR(g, mx, my, mw, mh, 16.0f, fade(pal.card));

    drawText_(g, L"重命名表情包分组", mx + 26, my + 22, mw - 52, 14.0f,
              fade(pal.text), StringAlignmentNear, FontStyleBold);

    RectF xr(mx + mw - 38, my + 16, 26, 26);
    bool xhov = inRect(g_mouse, xr);
    if (xhov) fillRR(g, xr.X, xr.Y, xr.Width, xr.Height, 6.0f, fade(pal.bg));
    icons::drawSvg(g, icons::Name::X, xr.X + 4, xr.Y + 4, 18.0f, fade(pal.text_muted));
    hit(xr, [](){ closeRenamePack(); }, true);

    RectF fr(mx + 26, my + 60, mw - 52, 44);
    p.input.bounds = fr;
    fillRR(g, fr.X, fr.Y, fr.Width, fr.Height, 10.0f, fade(pal.bg));
    strokeRR(g, fr.X, fr.Y, fr.Width, fr.Height, 10.0f, fade(pal.primary), 1.4f);
    Color halo((BYTE)(38 * t), pal.primary.GetR(), pal.primary.GetG(), pal.primary.GetB());
    strokeRR(g, fr.X - 2, fr.Y - 2, fr.Width + 4, fr.Height + 4, 12.0f, halo, 4.0f);
    if (p.input.text.empty()) {
        drawText_(g, L"新名字", fr.X + 14, fr.Y + 14, fr.Width - 28, 10.0f, fade(pal.text_muted));
    } else {
        Font* f = fontcache::get(10.0f);
        drawText_(g, p.input.text.c_str(), fr.X + 14, fr.Y + 14, fr.Width - 28, 10.0f, fade(pal.text));
        if (!p.input.hasSelection()) {
            RectF bb;
            g.MeasureString(p.input.displaySlice(0, p.input.cursor).c_str(), -1, f, PointF(0, 0), &bb);
            int phase = (int)(g_time_in_stage * 1000) % 1000;
            if (phase < 500) {
                Pen pen(fade(pal.primary), 1.5f);
                float cx_ = fr.X + 14 + bb.Width;
                g.DrawLine(&pen, cx_, fr.Y + 14, cx_, fr.Y + 30);
            }
        }
    }
    hit(fr, [](){}, true);

    if (!p.error.empty()) {
        Color ebg((BYTE)(36 * t), 0xE3, 0x4B, 0x4B);
        Color efg((BYTE)(255 * t), 0xFF, 0x8A, 0x80);
        fillRR(g, mx + 26, my + 112, mw - 52, 24, 6.0f, ebg);
        drawText_(g, p.error.c_str(), mx + 36, my + 117, mw - 72, 8.5f, efg);
    }

    float by = my + mh - 52;
    RectF cb(mx + 26, by, 100, 32);
    bool ch = inRect(g_mouse, cb);
    fillRR(g, cb.X, cb.Y, cb.Width, cb.Height, 8.0f, ch ? fade(pal.bg) : fade(pal.card));
    strokeRR(g, cb.X, cb.Y, cb.Width, cb.Height, 8.0f, fade(pal.divider));
    drawText_(g, L"取消", cb.X, cb.Y + 9, cb.Width, 9.5f, fade(pal.text),
              StringAlignmentCenter, FontStyleBold);
    hit(cb, [](){ closeRenamePack(); }, true);

    RectF sb(mx + mw - 26 - 110, by, 110, 32);
    bool sh = inRect(g_mouse, sb);
    Color sbg = p.busy ? fade(Color(255, 0x6B, 0x6A, 0x67))
                       : (sh ? fade(pal.primary_hover) : fade(pal.primary));
    fillRR(g, sb.X, sb.Y, sb.Width, sb.Height, 8.0f, sbg);
    drawText_(g, p.busy ? L"提交中…" : L"保存", sb.X, sb.Y + 9, sb.Width, 9.5f,
              Color((BYTE)(255 * t), 255, 255, 255), StringAlignmentCenter, FontStyleBold);
    if (!p.busy) hit(sb, [](){ ::submitRenamePack(); }, true);

    hit(RectF(0, 0, (REAL)Wpx, my), [](){ closeRenamePack(); }, true);
    hit(RectF(0, my + mh, (REAL)Wpx, Hpx - (my + mh)), [](){ closeRenamePack(); }, true);
    hit(RectF(0, my, mx, mh), [](){ closeRenamePack(); }, true);
    hit(RectF(mx + mw, my, Wpx - (mx + mw), mh), [](){ closeRenamePack(); }, true);
}

// ============================================================
// 简易 "确认" 对话框（删除 pack 用）
// ============================================================
struct ConfirmDlg {
    bool open = false;
    tx::Slide t;
    std::wstring title, message, primary_label;
    std::function<void()> on_confirm;
    bool danger = false;
};
inline ConfirmDlg& g_confirm() { static ConfirmDlg s; return s; }

void openConfirm(const wchar_t* title, const wchar_t* msg,
                 const wchar_t* primary, bool danger,
                 std::function<void()> on_confirm) {
    auto& p = g_confirm();
    p.open = true;
    p.t.enter(0.0f, 14.0f, 0.26f);
    p.title = title; p.message = msg; p.primary_label = primary;
    p.danger = danger;
    p.on_confirm = std::move(on_confirm);
}
void closeConfirm() {
    auto& p = g_confirm();
    p.open = false;
    p.t.exit(0.0f, 14.0f, 0.18f);
}

void paintConfirmModal(Graphics& g, int Wpx, int Hpx) {
    auto& p = g_confirm();
    if (p.t.value() < 0.001f && !p.open) return;
    const Palette& pal = palette();
    float t = p.t.value();
    if (t < 0.001f) return;
    auto fade = [&](Color c) { return Color((BYTE)(c.GetA() * t), c.GetR(), c.GetG(), c.GetB()); };

    SolidBrush dim(Color((BYTE)(170 * t), 0, 0, 0));
    g.FillRectangle(&dim, 0, 0, Wpx, Hpx);

    float mw = 360.0f, mh = 180.0f;
    float mx = (Wpx - mw) / 2.0f;
    float my = (Hpx - mh) / 2.0f + p.t.dy();
    drawShadow(g, mx, my, mw, mh, 16.0f, fade(Color(160, 0, 0, 0)), 6.0f, 4);
    fillRR(g, mx, my, mw, mh, 16.0f, fade(pal.card));

    drawText_(g, p.title.c_str(), mx + 26, my + 22, mw - 52, 14.0f,
              fade(pal.text), StringAlignmentNear, FontStyleBold);
    drawText_(g, p.message.c_str(), mx + 26, my + 56, mw - 52, 9.5f, fade(pal.text_muted));

    float by = my + mh - 52;
    RectF cb(mx + 26, by, 100, 32);
    bool ch = inRect(g_mouse, cb);
    fillRR(g, cb.X, cb.Y, cb.Width, cb.Height, 8.0f, ch ? fade(pal.bg) : fade(pal.card));
    strokeRR(g, cb.X, cb.Y, cb.Width, cb.Height, 8.0f, fade(pal.divider));
    drawText_(g, L"取消", cb.X, cb.Y + 9, cb.Width, 9.5f, fade(pal.text),
              StringAlignmentCenter, FontStyleBold);
    hit(cb, [](){ closeConfirm(); }, true);

    RectF sb(mx + mw - 26 - 110, by, 110, 32);
    bool sh = inRect(g_mouse, sb);
    Color base = p.danger ? Color(255, 0xE3, 0x4B, 0x4B) : pal.primary;
    Color hover = p.danger ? Color(255, 0xFF, 0x6B, 0x6B) : pal.primary_hover;
    fillRR(g, sb.X, sb.Y, sb.Width, sb.Height, 8.0f, fade(sh ? hover : base));
    drawText_(g, p.primary_label.c_str(), sb.X, sb.Y + 9, sb.Width, 9.5f,
              Color((BYTE)(255 * t), 255, 255, 255), StringAlignmentCenter, FontStyleBold);
    hit(sb, [](){
        auto& pp = g_confirm();
        auto fn = pp.on_confirm;
        closeConfirm();
        if (fn) fn();
    }, true);

    hit(RectF(0, 0, (REAL)Wpx, my), [](){ closeConfirm(); }, true);
    hit(RectF(0, my + mh, (REAL)Wpx, Hpx - (my + mh)), [](){ closeConfirm(); }, true);
    hit(RectF(0, my, mx, mh), [](){ closeConfirm(); }, true);
    hit(RectF(mx + mw, my, Wpx - (mx + mw), mh), [](){ closeConfirm(); }, true);
}

// ============================================================
// CS2 modal
// ============================================================


bool g_cs2_open = false;
tx::Slide g_cs2_t;            // modal 入场：translateY 12→0 + op 0→1

void openCS2() {
    g_cs2_open = true;
    g_cs2_t.enter(0.0f, 12.0f, 0.30f);
}
void closeCS2() {
    g_cs2_open = false;
    g_cs2_t.exit(0.0f, 12.0f, 0.20f);
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
    float my = (Hpx - mh) / 2 + g_cs2_t.dy();

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

    // 中央 play btn 改为打开 Steam 商店页（带视频自动播放）—
    // 异步 ShellExecute 不阻塞主线程，失败 silent
    hit(RectF(pbx, pby, 56.0f, 56.0f), [](){
        ShellExecuteW(nullptr, L"open",
            L"https://store.steampowered.com/app/730/CounterStrike_2/",
            nullptr, nullptr, SW_SHOWNORMAL);
    }, true);

    // 内容区
    readSteamInfo();
    float bx = mx + 28;
    float by = my + cover_h + 22;
    drawText_(g, L"Counter-Strike 2", bx, by, mw - 56,
              18.0f, fade(pal.text), StringAlignmentNear, FontStyleBold);
    drawText_(g, L"Valve · Source 2 引擎",
              bx, by + 28, mw - 56, 9.0f, fade(pal.text_muted));

    // 三段 stat — 真值从 Steam HKCU reg 读
    float sty = by + 60;
    struct Stat { const wchar_t* k; std::wstring v; };
    Stat stats[] = {
        { L"Steam 账号", g_steam.persona },
        { L"最近玩的",   g_steam.last_played },
        { L"总时长",     g_steam.playtime_label },
    };
    float sw = (mw - 56) / 3;
    for (int i = 0; i < 3; ++i) {
        float sx = bx + i * sw;
        drawText_(g, stats[i].k, sx, sty, sw, 8.0f, fade(pal.text_muted));
        drawText_(g, stats[i].v.c_str(), sx, sty + 16, sw - 12, 10.5f, fade(pal.text),
                  StringAlignmentNear, FontStyleBold);
    }

    // 描述
    drawText_(g, L"经典战术 FPS。订阅含 cfg 管控、灵敏度同步与启动器接管。点击中央播放按钮观看官方宣传片。",
              bx, sty + 50, mw - 56, 8.5f, fade(pal.text_muted));

    // 底部按钮 — 重新排版避免重叠：每个按钮 icon (18) + gap 10 + label 文字
    float btny = my + mh - 64;
    float btn_h = 42.0f;
    auto drawBtn = [&](float bx_, float bw_, bool primary, icons::Name icon,
                       const wchar_t* label, std::function<void()> click) {
        bool hov = inRect(g_mouse, RectF(bx_, btny, bw_, btn_h));
        Color bg, fg;
        if (primary) {
            bg = hov ? fade(pal.primary_hover) : fade(pal.primary);
            fg = Color((BYTE)(255 * t), 255, 255, 255);
            Color glow(80, pal.primary.GetR(), pal.primary.GetG(), pal.primary.GetB());
            drawShadow(g, bx_, btny, bw_, btn_h, 10, glow, 4, 3);
        } else {
            bg = hov ? fade(pal.bg) : fade(pal.card);
            fg = fade(pal.text);
        }
        fillRR(g, bx_, btny, bw_, btn_h, 10, bg);
        if (!primary) strokeRR(g, bx_, btny, bw_, btn_h, 10, fade(pal.divider));

        // 测算 label 宽度（用 caching font）
        Font* f = fontcache::get(10.0f, FontStyleBold);
        RectF mb;
        g.MeasureString(label, -1, f, PointF(0, 0), &mb);
        const float ico_w = 18.0f, gap = 10.0f;
        float total = ico_w + gap + mb.Width;
        float content_x = bx_ + (bw_ - total) / 2.0f;
        float content_y = btny + (btn_h - 18.0f) / 2.0f;
        icons::drawSvg(g, icon, content_x, content_y, ico_w, fg);
        // 文字 baseline 与 icon 中心对齐
        SolidBrush brush(fg);
        StringFormat fmt; fmt.SetAlignment(StringAlignmentNear);
        g.DrawString(label, -1, f,
                     RectF(content_x + ico_w + gap, btny + (btn_h - mb.Height) / 2.0f,
                           mb.Width + 4.0f, mb.Height + 2.0f), &fmt, &brush);

        hit(RectF(bx_, btny, bw_, btn_h), click, true);
    };

    // 两个等宽按钮 + 中间 gap
    float gap = 12.0f;
    float total_w = mw - 56.0f;
    float btn_w = (total_w - gap) / 2.0f;
    drawBtn(bx, btn_w, true, icons::Name::Play, L"启动 CS2", [](){
        ShellExecuteW(nullptr, L"open", L"steam://run/730", nullptr, nullptr, SW_SHOWNORMAL);
    });
    drawBtn(bx + btn_w + gap, btn_w, false, icons::Name::Link, L"Steam 商店页", [](){
        ShellExecuteW(nullptr, L"open",
            L"https://store.steampowered.com/app/730/CounterStrike_2/",
            nullptr, nullptr, SW_SHOWNORMAL);
    });

    // 点 modal 外关闭 — 拆成 modal 外的 4 个环形 hit（不覆盖 modal 内部，
    // 否则会阻断 ✕ / 启动 / play 等按钮的点击）
    hit(RectF(0, 0, (REAL)Wpx, my), [](){ closeCS2(); }, true);                                 // 上
    hit(RectF(0, my + mh, (REAL)Wpx, Hpx - (my + mh)), [](){ closeCS2(); }, true);              // 下
    hit(RectF(0, my, mx, mh), [](){ closeCS2(); }, true);                                       // 左
    hit(RectF(mx + mw, my, Wpx - (mx + mw), mh), [](){ closeCS2(); }, true);                    // 右
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
    float my = (Hpx - mh) / 2 + g_overlay_t.dy();

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
        g_overlay_t.exit(0.0f, 8.0f, 0.20f);
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
        g_overlay_t.exit(0.0f, 8.0f, 0.20f);
    }, true);

    // 点外部关闭 — 4 环形 hit（避开 modal 内部，否则会阻断 ✕ / 关闭 / 翻页按钮）
    auto closeModal = [](){
        g_overlay = Overlay::None;
        g_overlay_t.exit(0.0f, 8.0f, 0.20f);
    };
    hit(RectF(0, 0, (REAL)Wpx, my), closeModal, true);                              // 上
    hit(RectF(0, my + mh, (REAL)Wpx, Hpx - (my + mh)), closeModal, true);            // 下
    hit(RectF(0, my, mx, mh), closeModal, true);                                    // 左
    hit(RectF(mx + mw, my, Wpx - (mx + mw), mh), closeModal, true);                  // 右
}

}  // namespace modal
