// Chat view — Discord 风 240/1fr。
// 设计参考：design/styles.css .chat-shell 起，design/views.jsx ChatView。
// 频道写死官方（不要 touhou/vrchat）；后端 admin 可在 /admin/channels 改名/清理。
#pragma once

namespace chatv {

// ============== 数据 ==============
struct Channel {
    const wchar_t* id;
    const wchar_t* name;
    const wchar_t* group;       // IMPORTANT / GENERAL / GAMES / SHOP
    int unread{0};
    bool is_market{false};
};

// 官方频道 — 没有 touhou/vrchat（按用户要求剔除）。unread 置 0，等真消息流接通
const Channel kChannels[] = {
    { L"announcements", L"announcements", L"IMPORTANT", 0, false },
    { L"rules",         L"rules",         L"IMPORTANT", 0, false },
    { L"general",       L"general",       L"GENERAL",   0, false },
    { L"random",        L"random",        L"GENERAL",   0, false },
    { L"helpdesk",      L"helpdesk",      L"GENERAL",   0, false },
    { L"cs2",           L"cs2",           L"GAMES",     0, false },
    { L"market",        L"market",        L"SHOP",      0, true  },
    { L"trades",        L"trades",        L"SHOP",      0, false },
};
const wchar_t* kGroups[] = { L"IMPORTANT", L"GENERAL", L"GAMES", L"SHOP" };

enum class MsgKind { Text, Sticker, Gif, System, DayDivider, Typing, LinkCard, Video, Image };

struct Msg {
    MsgKind kind{MsgKind::Text};
    const wchar_t* from{L"yuki"};       // "me" 表示自己
    const wchar_t* author{L""};         // 显示名
    const wchar_t* status{L"online"};   // online/busy/away/sleep/offline
    const wchar_t* body{L""};           // text 内容 / sticker emoji / gif title / day text / link url / video file
    const wchar_t* time{L""};
    bool read{false};
    // 引用 (Discord 风)
    const wchar_t* reply_author{L""};
    const wchar_t* reply_excerpt{L""};
    // 链接卡
    const wchar_t* link_title{L""};
    const wchar_t* link_host{L""};
    // 视频
    int video_seconds{0};
};

// 频道消息流 — 一律从空开始；后端真正接通后由 WS 推送填充
std::vector<Msg>& streamFor(const wchar_t* chid) {
    static std::unordered_map<std::wstring, std::vector<Msg>> g_streams;
    auto it = g_streams.find(chid);
    if (it == g_streams.end()) {
        it = g_streams.emplace(std::wstring(chid), std::vector<Msg>{}).first;
    }
    return it->second;
}

// ============== 媒体附件存储 ==============
struct Media {
    enum Kind { KImage, KGif, KVideo, KFile } kind{KImage};
    std::wstring path;
    Gdiplus::Image* img{nullptr};   // GDI+ image (image/gif 解码后)
    int width{0}, height{0};
};
inline std::unordered_map<std::wstring, Media>& mediaCache() {
    static std::unordered_map<std::wstring, Media> m;
    return m;
}

// 从路径推 kind + 解码（image/gif）
const Media* loadMedia(const std::wstring& path) {
    auto& cache = mediaCache();
    auto it = cache.find(path);
    if (it != cache.end()) return &it->second;
    Media m; m.path = path;
    std::wstring s = path;
    auto dot = s.find_last_of(L'.');
    std::wstring suf = (dot != std::wstring::npos) ? s.substr(dot) : L"";
    for (auto& c : suf) c = (wchar_t)towlower(c);
    if (suf == L".png" || suf == L".jpg" || suf == L".jpeg" || suf == L".webp" || suf == L".bmp") {
        m.kind = Media::KImage;
        m.img = Gdiplus::Image::FromFile(path.c_str());
    } else if (suf == L".gif") {
        m.kind = Media::KGif;
        m.img = Gdiplus::Image::FromFile(path.c_str());
    } else if (suf == L".mp4" || suf == L".webm" || suf == L".mov" || suf == L".avi" || suf == L".mkv") {
        m.kind = Media::KVideo;
    } else {
        m.kind = Media::KFile;
    }
    if (m.img && m.img->GetLastStatus() == Gdiplus::Ok) {
        m.width = m.img->GetWidth();
        m.height = m.img->GetHeight();
    } else if (m.img) {
        delete m.img; m.img = nullptr;
    }
    auto [ins, _] = cache.emplace(path, std::move(m));
    return &ins->second;
}

// 文件名（路径最后一段）
inline std::wstring basename(const std::wstring& p) {
    auto pos = p.find_last_of(L"\\/");
    return (pos == std::wstring::npos) ? p : p.substr(pos + 1);
}

// 长生命周期 path 字符串
inline std::vector<std::wstring>& mediaPathStore() {
    static std::vector<std::wstring> v; return v;
}

// ============== 状态 ==============
const wchar_t* g_active{L"general"};
InputBox       g_composer;          // 完整 InputBox：选区 + Ctrl+A/C/V/X
bool           g_picker_open{false};
inline std::unordered_map<std::wstring, bool>& groupCollapsed() {
    static std::unordered_map<std::wstring, bool> m;
    return m;
}
int            g_picker_tab{0};   // 0=emoji 1=sticker 2=gif
Tween          g_picker_t;
int            g_streams_dirty_index{-1};   // 上次 active 切换的标记，触发 fade
bool           g_focus_composer{false};
int            g_at_menu_index{-1};   // 显示头像右键菜单的 message index, -1 关闭
RectF          g_at_menu_rect{};

// 计时驱动 typing dots
float g_typing_t = 0.0f;

void switchChannel(const wchar_t* id) {
    g_active = id;
    g_picker_open = false;
}

// 把一个文件路径作为 media message 加到当前频道
void appendMedia(const std::wstring& path) {
    if (path.empty()) return;
    const Media* m = loadMedia(path);
    if (!m) return;
    auto& store = mediaPathStore();
    store.push_back(path);
    Msg msg; msg.from = L"me"; msg.author = L""; msg.status = L"online";
    msg.read = false; msg.time = L"now";
    msg.body = store.back().c_str();
    switch (m->kind) {
        case Media::KImage: msg.kind = MsgKind::Image; break;
        case Media::KGif:   msg.kind = MsgKind::Gif;   break;
        case Media::KVideo: msg.kind = MsgKind::Video; break;
        case Media::KFile:  msg.kind = MsgKind::Text;  break;
    }
    streamFor(g_active).push_back(msg);
}

// 保存 HBITMAP 到临时 PNG，返回路径
std::wstring saveBitmapToTempPng(HBITMAP hbm) {
    if (!hbm) return L"";
    Gdiplus::Bitmap b(hbm, nullptr);
    wchar_t tmp[MAX_PATH], file[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    GetTempFileNameW(tmp, L"lpv", 0, file);
    std::wstring out = file; out += L".png";
    DeleteFileW(file);
    CLSID clsid;
    UINT num = 0, sz = 0;
    Gdiplus::GetImageEncodersSize(&num, &sz);
    if (sz == 0) return L"";
    std::vector<BYTE> buf(sz);
    auto* enc = (Gdiplus::ImageCodecInfo*)buf.data();
    Gdiplus::GetImageEncoders(num, sz, enc);
    for (UINT i = 0; i < num; ++i) {
        if (wcscmp(enc[i].MimeType, L"image/png") == 0) {
            clsid = enc[i].Clsid; break;
        }
    }
    if (b.Save(out.c_str(), &clsid, nullptr) == Gdiplus::Ok) return out;
    return L"";
}

// 从剪贴板尝试粘贴媒体（图片 / 文件 drop）。返回 true 表示已消费。
bool tryPasteMedia(HWND hwnd) {
    if (!OpenClipboard(hwnd)) return false;
    bool consumed = false;
    // 先看 HDROP（拖拽 / 复制文件）
    if (HANDLE h = GetClipboardData(CF_HDROP)) {
        HDROP drop = (HDROP)h;
        UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < n; ++i) {
            wchar_t buf[MAX_PATH];
            if (DragQueryFileW(drop, i, buf, MAX_PATH)) {
                appendMedia(buf);
                consumed = true;
            }
        }
    }
    // 再看图片 bitmap
    if (!consumed) {
        if (HANDLE h = GetClipboardData(CF_BITMAP)) {
            std::wstring tmp = saveBitmapToTempPng((HBITMAP)h);
            if (!tmp.empty()) { appendMedia(tmp); consumed = true; }
        }
    }
    CloseClipboard();
    return consumed;
}

bool isMarketChannel() {
    for (auto& c : kChannels) if (wcscmp(c.id, g_active) == 0) return c.is_market;
    return false;
}
const Channel* activeChannel() {
    for (auto& c : kChannels) if (wcscmp(c.id, g_active) == 0) return &c;
    return &kChannels[2];   // general
}

// ============== 渲染辅助 ==============
Color statusColor(const Palette& pal, const wchar_t* status) {
    if (wcscmp(status, L"online") == 0) return pal.status_online;
    if (wcscmp(status, L"busy")   == 0) return pal.status_busy;
    if (wcscmp(status, L"away")   == 0) return pal.status_away;
    if (wcscmp(status, L"sleep")  == 0) return pal.status_sleep;
    return pal.status_offline;
}

void drawAvatar(Graphics& g, float x, float y, float r, const wchar_t* name,
                const wchar_t* status, const Palette& pal) {
    SolidBrush bg(pal.primary);
    g.FillEllipse(&bg, x, y, r*2, r*2);
    Font f(kFontFace, r * 0.78f, FontStyleBold, UnitPixel);
    SolidBrush ft(Color(255, 255, 255, 255));
    StringFormat fmt; fmt.SetAlignment(StringAlignmentCenter); fmt.SetLineAlignment(StringAlignmentCenter);
    wchar_t init[2] = { (wchar_t)towupper(name && name[0] ? name[0] : L'?'), 0 };
    g.DrawString(init, -1, &f, RectF(x, y, r*2, r*2), &fmt, &ft);
    if (status && status[0]) {
        float dr = r * 0.28f;
        SolidBrush sb(statusColor(pal, status));
        g.FillEllipse(&sb, x + r*2 - dr*2, y + r*2 - dr*2, dr*2, dr*2);
        Pen ring(pal.bg, 2.0f);
        g.DrawEllipse(&ring, x + r*2 - dr*2, y + r*2 - dr*2, dr*2, dr*2);
    }
}

// ============== 频道列表 ==============
void paintChatList(Graphics& g, RectF area) {
    const Palette& pal = palette();
    fillRR(g, area.X, area.Y, area.Width, area.Height, 0, pal.bg);
    Pen sep(pal.divider, 1.0f);
    g.DrawLine(&sep, area.X + area.Width, area.Y, area.X + area.Width, area.Y + area.Height);

    // header 14 + glyph 26 + label
    float hy = area.Y + 12;
    drawText_(g, L"Launcher Server", area.X + 50, hy + 2, 200,
              11.0f, pal.text, StringAlignmentNear, FontStyleBold);
    SolidBrush gbg(pal.primary);
    fillRR(g, area.X + 14, hy - 2, 26, 26, 7.0f, pal.primary);
    icons::drawSvg(g, icons::Name::Logo, area.X + 14 + 5, hy - 2 + 5, 16,
                   Color(255, 255, 255, 255));
    g.DrawLine(&sep, area.X + 8, area.Y + 44, area.X + area.Width - 8, area.Y + 44);

    // groups
    float row_y = area.Y + 50;
    for (auto* gname : kGroups) {
        // group head — 11px uppercase tracking-1 muted
        RectF ghead(area.X + 6, row_y, area.Width - 12, 22);
        bool ghov = inRect(g_mouse, ghead);
        if (ghov) {
            Color hc(g_dark ? 14 : 10, pal.text.GetR(), pal.text.GetG(), pal.text.GetB());
            fillRR(g, ghead.X, ghead.Y, ghead.Width, ghead.Height, 4.0f, hc);
        }
        bool collapsed = groupCollapsed()[gname];
        drawText_(g, collapsed ? L"▸" : L"▾", area.X + 10, row_y + 4, 12, 8.0f, pal.text_muted);
        drawText_(g, gname, area.X + 26, row_y + 4, 200,
                  8.0f, pal.text_muted, StringAlignmentNear, FontStyleBold);
        const wchar_t* gn = gname;
        hit(ghead, [gn](){ groupCollapsed()[gn] = !groupCollapsed()[gn]; }, true);
        row_y += 24;

        if (collapsed) { row_y += 6; continue; }

        // channels in group
        for (auto& c : kChannels) {
            if (wcscmp(c.group, gname) != 0) continue;
            bool active = (wcscmp(c.id, g_active) == 0);
            RectF cr(area.X + 6, row_y, area.Width - 12, 28);
            bool hov = inRect(g_mouse, cr);
            if (active) {
                Color a(36, pal.primary.GetR(), pal.primary.GetG(), pal.primary.GetB());
                fillRR(g, cr.X, cr.Y, cr.Width, cr.Height, 6.0f, a);
            } else if (hov) {
                Color h(g_dark ? 10 : 8, pal.text.GetR(), pal.text.GetG(), pal.text.GetB());
                fillRR(g, cr.X, cr.Y, cr.Width, cr.Height, 6.0f, h);
            }
            // # 前缀
            drawText_(g, L"#", cr.X + 12, cr.Y + 6, 14, 10.0f,
                      active ? pal.primary : pal.text_muted,
                      StringAlignmentNear, FontStyleBold);
            drawText_(g, c.name, cr.X + 26, cr.Y + 7, cr.Width - 60,
                      9.5f, active ? pal.text : pal.text_muted,
                      StringAlignmentNear, active ? FontStyleBold : FontStyleRegular);
            // unread badge
            if (c.unread > 0) {
                wchar_t buf[16]; swprintf_s(buf, 16, L"%d", c.unread);
                float bw = c.unread > 9 ? 22.0f : 16.0f;
                RectF br(cr.X + cr.Width - bw - 8, cr.Y + 7, bw, 14);
                fillRR(g, br.X, br.Y, br.Width, br.Height, 7.0f, pal.primary);
                drawText_(g, buf, br.X, br.Y + 2, br.Width, 7.5f,
                          Color(255, 255, 255, 255), StringAlignmentCenter, FontStyleBold);
            }
            const wchar_t* tgt = c.id;
            hit(cr, [tgt]() { switchChannel(tgt); }, true);
            row_y += 30;
        }
        row_y += 6;
    }
}

// ============== 单条气泡 ==============
// 返回这条占用的高度
float paintBubble(Graphics& g, const Msg& m, float x, float y, float maxw,
                  const Palette& pal, bool prev_same_author, int msg_index) {
    if (m.kind == MsgKind::DayDivider) {
        std::wstring s = m.body;
        float tw = measureText(g, s.c_str(), 8.0f).Width + 24;
        float bx = x + (maxw - tw) / 2;
        Color pillC(g_dark ? 12 : 10, pal.text.GetR(), pal.text.GetG(), pal.text.GetB());
        fillRR(g, bx, y + 6, tw, 18, 9.0f, pillC);
        drawText_(g, s.c_str(), bx, y + 9, tw, 7.5f, pal.text_muted, StringAlignmentCenter);
        return 30;
    }
    if (m.kind == MsgKind::System) {
        std::wstring s = m.body;
        float tw = measureText(g, s.c_str(), 8.5f).Width + 24;
        float bx = x + (maxw - tw) / 2;
        Color pillC(g_dark ? 12 : 10, pal.text.GetR(), pal.text.GetG(), pal.text.GetB());
        fillRR(g, bx, y + 4, tw, 22, 11.0f, pillC);
        drawText_(g, s.c_str(), bx, y + 8, tw, 8.5f, pal.text_muted, StringAlignmentCenter);
        return 32;
    }
    if (m.kind == MsgKind::Typing) {
        // 三点弹动
        float dot_y = y + 14;
        for (int i = 0; i < 3; ++i) {
            float phase = g_typing_t * 1.5f + i * 0.18f;
            float dy = sinf(phase * 6.28f) * 3.0f;
            if (dy < 0) dy = 0;
            Color dc((BYTE)(120 + 80 * (dy / 3.0f)), pal.text_muted.GetR(),
                     pal.text_muted.GetG(), pal.text_muted.GetB());
            SolidBrush b(dc);
            g.FillEllipse(&b, x + 18 + 28 + i * 12.0f, dot_y - dy, 6.0f, 6.0f);
        }
        Color cardC(g_dark ? 200 : 240, pal.card.GetR(), pal.card.GetG(), pal.card.GetB());
        fillRR(g, x + 18 + 18, y + 6, 60, 24, 12.0f, cardC);
        // avatar
        if (!prev_same_author) drawAvatar(g, x + 4, y + 4, 14, m.from, m.status, pal);
        return 38;
    }

    bool me = (wcscmp(m.from, L"me") == 0);
    bool isImage = (m.kind == MsgKind::Image);
    bool isGif   = (m.kind == MsgKind::Gif);
    bool isVideo = (m.kind == MsgKind::Video);

    // ---------- Image / GIF / Video 媒体气泡 ----------
    if (isImage || isGif || isVideo) {
        const Media* mm = loadMedia(m.body ? m.body : L"");
        const float bub_max_w = std::min(maxw * 0.55f, 320.0f);
        float bub_w = 240.0f, bub_h = 180.0f;
        if (mm && mm->img && mm->width > 0 && mm->height > 0) {
            float aspect = (float)mm->height / (float)mm->width;
            bub_w = std::min(bub_max_w, (float)mm->width);
            bub_h = bub_w * aspect;
            if (bub_h > 240.0f) { bub_h = 240.0f; bub_w = bub_h / aspect; }
        } else if (isVideo) {
            bub_w = 260.0f; bub_h = 160.0f;
        }
        const float gutter_m = 38.0f;
        float bub_x = me ? (x + maxw - 14.0f - bub_w) : (x + gutter_m);

        // 头像（非自己 + 非连续）
        if (!me && !prev_same_author) {
            drawAvatar(g, x, y + bub_h - 28.0f, 14.0f, m.author, m.status, palette());
        }

        // 圆角裁剪 + 画图 / 视频封面
        GraphicsPath cp; buildRoundRect(cp, bub_x, y, bub_w, bub_h, 12.0f);
        g.SetClip(&cp);
        if (mm && mm->img) {
            g.DrawImage(mm->img, RectF(bub_x, y, bub_w, bub_h));
        } else {
            // video 没缩略 / 图片解码失败 — 纯色占位
            SolidBrush bg(Color(255, 0x28, 0x24, 0x20));
            g.FillRectangle(&bg, bub_x, y, bub_w, bub_h);
        }
        // 视频底部渐变蒙版让 ▶ 可读
        if (isVideo) {
            LinearGradientBrush vmask(PointF(bub_x, y + bub_h * 0.5f), PointF(bub_x, y + bub_h),
                                      Color(0, 0, 0, 0), Color(160, 0, 0, 0));
            g.FillRectangle(&vmask, bub_x, y + bub_h * 0.5f, bub_w, bub_h * 0.5f);
        }
        g.ResetClip();

        // 视频中央播放按钮 + 文件名
        if (isVideo) {
            float btnr = 24.0f;
            float btnx = bub_x + bub_w / 2 - btnr;
            float btny = y + bub_h / 2 - btnr;
            SolidBrush pbg(Color(190, 0, 0, 0));
            g.FillEllipse(&pbg, btnx, btny, btnr * 2, btnr * 2);
            icons::drawSvg(g, icons::Name::Play, btnx + 12.0f, btny + 12.0f, 24.0f,
                           Color(255, 255, 255, 255));
            // 文件名（底部 padding）
            std::wstring fn = basename(m.body ? m.body : L"");
            drawText_(g, fn.c_str(), bub_x + 10.0f, y + bub_h - 22.0f, bub_w - 20.0f,
                      8.0f, Color(255, 255, 255, 255), StringAlignmentNear, FontStyleBold);
            // 整个气泡点击 → ShellExecute 默认播放器
            std::wstring path_copy = m.body ? m.body : L"";
            hit(RectF(bub_x, y, bub_w, bub_h), [path_copy](){
                if (!path_copy.empty()) {
                    ShellExecuteW(nullptr, L"open", path_copy.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                }
            }, true);
        } else if (isImage || isGif) {
            // 图片点击 → 用默认查看器打开（暂不做内嵌大图）
            std::wstring path_copy = m.body ? m.body : L"";
            hit(RectF(bub_x, y, bub_w, bub_h), [path_copy](){
                if (!path_copy.empty()) {
                    ShellExecuteW(nullptr, L"open", path_copy.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                }
            }, true);
        }
        // time + 双勾
        const Palette& palc = palette();
        Color metaC(220, 255, 255, 255);
        drawText_(g, m.time, bub_x, y + bub_h - 14.0f, bub_w - 10.0f,
                  7.0f, metaC, StringAlignmentFar);
        if (me) {
            Color tickC = m.read ? Color(255, 0x7D, 0xD3, 0xFC) : metaC;
            icons::drawSvg(g, icons::Name::Check2,
                           bub_x + bub_w - 24.0f, y + bub_h - 16.0f, 12.0f, tickC);
        }
        (void)palc;
        return bub_h + 10.0f;
    }

    // 用 GDI+ MeasureString 精确测算
    const float pad_l = 14.0f, pad_r = 14.0f;
    const float pad_t = 9.0f,  pad_b = 8.0f;
    const float content_w_max = maxw * 0.62f;
    const float min_w = 60.0f;
    std::wstring body_w = m.body ? m.body : L"";

    Font body_font(kFontFace, 9.5f, FontStyleRegular, UnitPoint);
    StringFormat body_fmt;
    body_fmt.SetAlignment(StringAlignmentNear);

    // 第一遍：单行宽度
    RectF unbounded(0, 0, 4096.0f, 4096.0f);
    RectF measured;
    g.MeasureString(body_w.c_str(), -1, &body_font, unbounded, &body_fmt, &measured);
    float wanted_w = measured.Width + pad_l + pad_r;

    bool has_reply = m.reply_excerpt && m.reply_excerpt[0];
    bool show_author = !me && m.author && m.author[0] && !prev_same_author;

    // 时间 meta 估算
    Font meta_font(kFontFace, 7.5f, FontStyleRegular, UnitPoint);
    RectF meta_box;
    g.MeasureString(m.time ? m.time : L"", -1, &meta_font, unbounded, &body_fmt, &meta_box);
    float meta_w = meta_box.Width + (me ? 16.0f : 0.0f);   // me 多留双勾空间
    // 文字 + meta 不换行能塞下时
    if (wanted_w + meta_w + 8.0f <= content_w_max) {
        wanted_w += meta_w + 8.0f;
    }
    float bubble_w = std::max(min_w, std::min(wanted_w, content_w_max));

    // 第二遍：限定宽度后实际行高
    RectF inner_layout(0, 0, bubble_w - pad_l - pad_r, 4096.0f);
    g.MeasureString(body_w.c_str(), -1, &body_font, inner_layout, &body_fmt, &measured);
    float text_h = measured.Height;

    float bubble_h = pad_t + (show_author ? 14.0f : 0.0f)
                          + (has_reply ? 24.0f : 0.0f)
                          + text_h
                          + 16.0f   /* meta line */
                          + pad_b;

    const float gutter = 38.0f;
    float bubble_x;
    if (me) {
        bubble_x = x + maxw - pad_r - bubble_w;
    } else {
        bubble_x = x + gutter;
    }

    // 头像（仅非自己 + 非连续）
    if (!me && !prev_same_author) {
        drawAvatar(g, x, y + bubble_h - 28.0f, 14.0f, m.author, m.status, pal);
        int idx = msg_index;
        hit(RectF(x, y + bubble_h - 28.0f, 28.0f, 28.0f), [idx](){
            auto& s = chatv::streamFor(g_active);
            if (idx >= 0 && idx < (int)s.size() && s[idx].author && s[idx].author[0]) {
                std::wstring at = std::wstring(L"@") + s[idx].author + L" ";
                g_composer.replaceSelection(at);
                g_focus_composer = true;
            }
        }, true);
    }

    // 气泡背景
    Color cardC = me ? pal.primary : pal.card;
    fillRR(g, bubble_x, y, bubble_w, bubble_h, 14.0f, cardC);

    float ty = y + pad_t;

    // 引用条
    if (has_reply) {
        Color repBar = me ? Color(255, 255, 255, 255)
                          : Color(255, pal.primary.GetR(), pal.primary.GetG(), pal.primary.GetB());
        fillRR(g, bubble_x + pad_l, ty + 1.0f, 3.0f, 18.0f, 1.5f, repBar);
        Color repNameC = me ? Color(255, 255, 255, 255) : pal.primary;
        Color repTextC = me ? Color(220, 255, 255, 255) : pal.text_muted;
        drawText_(g, m.reply_author, bubble_x + pad_l + 8.0f, ty, bubble_w - pad_l - pad_r - 8.0f,
                  7.5f, repNameC, StringAlignmentNear, FontStyleBold);
        drawText_(g, m.reply_excerpt, bubble_x + pad_l + 8.0f, ty + 10.0f,
                  bubble_w - pad_l - pad_r - 8.0f,
                  7.5f, repTextC, StringAlignmentNear);
        ty += 24.0f;
    }

    // 作者名
    if (show_author) {
        drawText_(g, m.author, bubble_x + pad_l, ty, bubble_w - pad_l - pad_r,
                  8.0f, pal.primary, StringAlignmentNear, FontStyleBold);
        ty += 14.0f;
    }

    // 正文
    Color textC = me ? Color(255, 255, 255, 255) : pal.text;
    SolidBrush textB(textC);
    RectF text_rect(bubble_x + pad_l, ty, bubble_w - pad_l - pad_r, text_h + 4.0f);
    g.DrawString(body_w.c_str(), -1, &body_font, text_rect, &body_fmt, &textB);

    // meta（时间 + 双勾）右下角
    Color metaC = me ? Color(220, 255, 255, 255) : pal.text_muted;
    float meta_y = y + bubble_h - 14.0f;
    drawText_(g, m.time, bubble_x, meta_y, bubble_w - pad_r - (me ? 16.0f : 0.0f),
              7.0f, metaC, StringAlignmentFar);
    if (me) {
        Color tickC = m.read ? Color(255, 0x7D, 0xD3, 0xFC) : metaC;
        icons::drawSvg(g, icons::Name::Check2,
                       bubble_x + bubble_w - pad_r - 14.0f, meta_y - 2.0f, 12.0f, tickC);
    }

    return bubble_h + 8.0f;
}

// (旧版 sticker/gif/link/video 分支已移除 — sample 数据清空后用不到。
//  后端真正接通后按 message_type 重新加。)
#if 0
static float paintBubble_legacy_unused(Graphics& g, const Msg& m, float x, float y, float maxw,
                                       const Palette& pal, bool prev_same_author, int msg_index) {
    bool me = false; (void)g; (void)m; (void)x; (void)y; (void)maxw; (void)pal; (void)prev_same_author; (void)msg_index;
    if (false) {
        Color cardC = pal.card;
        if (false) {
            Color tickC = m.read ? Color(255, 0x7D, 0xD3, 0xFC) : metaC;
            icons::drawSvg(g, icons::Name::Check2,
                           bubble_x + bubble_w - 22, y + bubble_h - 16, 14, tickC);
        }
    }

    return 0.0f;
}
#endif

// ============== Composer ==============
// 只保留 emoji + textarea + send 三件套；左下三个杂图标全部删掉。
void paintComposer(Graphics& g, RectF area) {
    const Palette& pal = palette();
    SolidBrush bg(pal.bg);
    g.FillRectangle(&bg, area.X, area.Y, area.Width, area.Height);
    Pen sep(pal.divider, 1.0f);
    g.DrawLine(&sep, area.X, area.Y, area.X + area.Width, area.Y);

    // emoji 圆角图标按钮
    const float ico_sz = 30.0f;
    float ix = area.X + 14.0f;
    float iy = area.Y + (area.Height - ico_sz) / 2.0f;
    bool ehov = inRect(g_mouse, RectF(ix, iy, ico_sz, ico_sz));
    if (ehov) fillRR(g, ix, iy, ico_sz, ico_sz, 8.0f, pal.card);
    icons::drawSvg(g, icons::Name::Smile, ix + 6.0f, iy + 6.0f, 18.0f,
                   ehov ? pal.text : pal.text_muted);
    hit(RectF(ix, iy, ico_sz, ico_sz), [](){
        g_picker_open = !g_picker_open;
        g_picker_tab = 0;
        g_picker_t.start(g_picker_t.value(), g_picker_open ? 1.0f : 0.0f, 0.22f, 0, curve::easeOutBack);
    }, true);

    // textarea — 居中精确，placeholder 与文字垂直对齐
    float fx = ix + ico_sz + 10.0f;
    float send_w = 38.0f;
    float fw = area.Width - (fx - area.X) - 14.0f - send_w - 10.0f;
    float fh = ico_sz;
    float fy = iy;
    fillRR(g, fx, fy, fw, fh, fh / 2.0f, pal.card);
    strokeRR(g, fx, fy, fw, fh, fh / 2.0f,
             g_focus_composer ? pal.primary : pal.divider, g_focus_composer ? 1.4f : 1.0f);
    if (g_focus_composer) {
        Color halo(22, pal.primary.GetR(), pal.primary.GetG(), pal.primary.GetB());
        strokeRR(g, fx - 2.0f, fy - 2.0f, fw + 4.0f, fh + 4.0f, fh / 2.0f + 2.0f, halo, 3.0f);
    }
    // 文字 / placeholder：垂直居中（fy + (fh - line_h)/2，line_h 约 14px @ 9.5pt）
    const float pad_l = 16.0f;
    const float text_y = fy + (fh - 14.0f) / 2.0f;
    g_composer.bounds = RectF(fx, fy, fw, fh);
    if (g_composer.text.empty()) {
        drawText_(g, L"写点什么…", fx + pad_l, text_y, fw - pad_l * 2.0f,
                  9.5f, pal.text_muted);
    } else {
        // 选区高亮
        Font* f = fontcache::get(9.5f);
        if (g_focus_composer && g_composer.hasSelection()) {
            RectF bb_pre, bb_in;
            g.MeasureString(g_composer.displaySlice(0, g_composer.selStart()).c_str(), -1, f,
                            PointF(0, 0), &bb_pre);
            g.MeasureString(g_composer.displaySlice(g_composer.selStart(), g_composer.selEnd()).c_str(), -1, f,
                            PointF(0, 0), &bb_in);
            Color sel_bg(96, pal.primary.GetR(), pal.primary.GetG(), pal.primary.GetB());
            SolidBrush sel_b(sel_bg);
            g.FillRectangle(&sel_b, fx + pad_l + bb_pre.Width, text_y - 1.0f,
                            bb_in.Width, 16.0f);
        }
        drawText_(g, g_composer.text.c_str(), fx + pad_l, text_y, fw - pad_l * 2.0f,
                  9.5f, pal.text);
    }
    // caret blink
    if (g_focus_composer && !g_composer.hasSelection()) {
        Font* fnt = fontcache::get(9.5f);
        std::wstring sub = g_composer.displaySlice(0, g_composer.cursor);
        RectF bb; g.MeasureString(sub.c_str(), -1, fnt, PointF(0, 0), &bb);
        int phase = (int)(g_time_in_stage * 1000) % 1000;
        if (phase < 500) {
            Pen p(pal.primary, 1.5f);
            float cx_ = fx + pad_l + bb.Width;
            g.DrawLine(&p, cx_, fy + 7.0f, cx_, fy + fh - 7.0f);
        }
    }
    hit(RectF(fx, fy, fw, fh), [](){ g_focus_composer = true; }, true);

    // send btn 圆形主色
    float sx = area.X + area.Width - 14.0f - send_w;
    float sy = iy + (fh - send_w) / 2.0f;
    bool can_send = !g_composer.text.empty();
    bool sh = inRect(g_mouse, RectF(sx, sy, send_w, send_w));
    Color sbg = !can_send ? Color(140, pal.primary.GetR(), pal.primary.GetG(), pal.primary.GetB())
                          : (sh ? pal.primary_hover : pal.primary);
    SolidBrush sbgB(sbg);
    g.FillEllipse(&sbgB, sx, sy, send_w, send_w);
    icons::drawSvg(g, icons::Name::Send, sx + 10.0f, sy + 10.0f, 18.0f,
                   Color(255, 255, 255, 255));
    if (can_send) {
        hit(RectF(sx, sy, send_w, send_w), [](){
            auto& s = streamFor(g_active);
            Msg m; m.kind = MsgKind::Text; m.from = L"me"; m.author = L"";
            m.status = L"online"; m.read = false; m.time = L"now";
            static std::vector<std::wstring> g_my_msgs;
            g_my_msgs.push_back(g_composer.text);
            m.body = g_my_msgs.back().c_str();
            s.push_back(m);
            g_composer.text.clear();
            g_composer.cursor = 0;
            g_composer.clearSel();
            g_focus_composer = true;
        }, true);
    }
}

// ============== Picker ==============
const wchar_t* kEmoji[] = {
    L"😀",L"😁",L"😂",L"🤣",L"😄",L"😅",L"😉",L"😊",
    L"😎",L"😍",L"🥰",L"🙃",L"🙂",L"🤩",L"🤔",L"😐",
    L"😴",L"😌",L"😜",L"🤪",L"🥳",L"🥺",L"😢",L"😭",
    L"💀",L"👻",L"🤖",L"👍",L"👎",L"👏",L"🙏",L"💪",
    L"🔥",L"💯",L"🎮",L"🍣",L"🌸",L"⭐",L"🚀",L"💖",
};
const wchar_t* kSticker[] = { L"🎮",L"🔥",L"💀",L"🎉",L"💯",L"✨",L"🚀",L"⭐",L"🌸",L"💖",L"👏",L"🙏" };
const wchar_t* kGif[] = {
    L"杏仁糖蹦迪",L"柴犬狂笑",L"恭喜发财",L"姐姐看我",L"摔倒猫猫",
    L"王境泽真香",L"贴贴.gif",L"老板说的对"
};

void paintPicker(Graphics& g, float anchor_x, float anchor_y) {
    if (g_picker_t.value() < 0.001f && !g_picker_open) return;
    const Palette& pal = palette();
    float t = g_picker_t.value();
    float pw = 320, ph = 360;
    float px = anchor_x;
    float py = anchor_y - ph - 8;
    BYTE a = (BYTE)(255 * t);
    if (a == 0) return;

    auto fade = [&](Color c) { return Color((BYTE)(c.GetA() * t), c.GetR(), c.GetG(), c.GetB()); };
    Color cardC(a, pal.card.GetR(), pal.card.GetG(), pal.card.GetB());
    drawShadow(g, px, py, pw, ph, 12, fade(pal.shadow_card_hover), 6, 5);
    fillRR(g, px, py, pw, ph, 12, cardC);
    strokeRR(g, px, py, pw, ph, 12, fade(pal.divider));

    // tabs
    const wchar_t* tabs[] = { L"Emoji", L"贴纸", L"GIF" };
    float tx = px + 10;
    for (int i = 0; i < 3; ++i) {
        float tw = 56;
        bool on = (i == g_picker_tab);
        RectF tr(tx, py + 8, tw, 26);
        if (on) {
            Color on_bg(28, pal.primary.GetR(), pal.primary.GetG(), pal.primary.GetB());
            fillRR(g, tr.X, tr.Y, tw, 26, 6, on_bg);
        }
        drawText_(g, tabs[i], tr.X, tr.Y + 6, tw, 9.0f,
                  on ? fade(pal.primary) : fade(pal.text_muted),
                  StringAlignmentCenter, FontStyleBold);
        int idx = i;
        hit(tr, [idx](){ g_picker_tab = idx; }, true);
        tx += tw + 4;
    }
    Pen sep(fade(pal.divider), 1.0f);
    g.DrawLine(&sep, px + 10, py + 38, px + pw - 10, py + 38);

    // search box
    fillRR(g, px + 10, py + 46, pw - 20, 30, 8, fade(pal.bg));
    strokeRR(g, px + 10, py + 46, pw - 20, 30, 8, fade(pal.divider));
    drawText_(g, L"搜索…", px + 22, py + 54, pw - 40, 9.0f, fade(pal.text_muted));

    // grid
    float gx = px + 10, gy = py + 88;
    if (g_picker_tab == 2) {
        // GIF — 2 col 4:3
        int n = (int)(sizeof(kGif) / sizeof(kGif[0]));
        float cell_w = (pw - 28) / 2;
        float cell_h = cell_w * 0.75f;
        for (int i = 0; i < n; ++i) {
            int row = i / 2, col = i % 2;
            float cx = gx + col * (cell_w + 8);
            float cy = gy + row * (cell_h + 8);
            LinearGradientBrush lg(PointF(cx, cy), PointF(cx + cell_w, cy + cell_h),
                                   Color((BYTE)(255 * t), 0x7C, 0x9D, 0xD9),
                                   fade(pal.primary));
            GraphicsPath gp; buildRoundRect(gp, cx, cy, cell_w, cell_h, 8);
            g.FillPath(&lg, &gp);
            drawText_(g, kGif[i], cx, cy + cell_h / 2 - 6, cell_w, 8.5f,
                      fade(Color(255, 255, 255, 255)), StringAlignmentCenter, FontStyleBold);
            const wchar_t* label = kGif[i];
            hit(RectF(cx, cy, cell_w, cell_h), [label]() {
                auto& s = streamFor(g_active);
                static std::vector<std::wstring> g_my_gifs;
                Msg m; m.kind = MsgKind::Gif; m.from = L"me"; m.status = L"online";
                m.read = false; m.time = L"now";
                g_my_gifs.push_back(label);
                m.body = g_my_gifs.back().c_str();
                s.push_back(m);
                g_picker_open = false;
                g_picker_t.start(g_picker_t.value(), 0, 0.18f, 0, curve::easeOutCubic);
            }, true);
        }
    } else {
        // emoji / sticker — 8 col grid
        const wchar_t** items = (g_picker_tab == 1) ? kSticker : kEmoji;
        int n = (g_picker_tab == 1) ? (int)(sizeof(kSticker)/sizeof(kSticker[0]))
                                     : (int)(sizeof(kEmoji)/sizeof(kEmoji[0]));
        float cell = (pw - 28) / 8;
        for (int i = 0; i < n; ++i) {
            int row = i / 8, col = i % 8;
            float cx = gx + col * cell;
            float cy = gy + row * cell;
            bool hov = inRect(g_mouse, RectF(cx, cy, cell, cell));
            if (hov) fillRR(g, cx, cy, cell, cell, 6, fade(pal.bg));
            Font f(kFontFace, cell * 0.5f, FontStyleRegular, UnitPixel);
            SolidBrush b(fade(pal.text));
            StringFormat fmt; fmt.SetAlignment(StringAlignmentCenter); fmt.SetLineAlignment(StringAlignmentCenter);
            g.DrawString(items[i], -1, &f, RectF(cx, cy, cell, cell), &fmt, &b);
            const wchar_t* val = items[i];
            int tab = g_picker_tab;
            hit(RectF(cx, cy, cell, cell), [val, tab]() {
                if (tab == 1) {
                    // 贴纸 → 直接发
                    auto& s = streamFor(g_active);
                    Msg m; m.kind = MsgKind::Sticker; m.from = L"me"; m.status = L"online";
                    m.read = false; m.body = val; m.time = L"now";
                    s.push_back(m);
                    g_picker_open = false;
                    g_picker_t.start(g_picker_t.value(), 0, 0.18f, 0, curve::easeOutCubic);
                } else {
                    g_composer.replaceSelection(val);
                    g_focus_composer = true;
                }
            }, true);
        }
    }
}

// ============== chat header + main ==============
void paintChatPane(Graphics& g, RectF area) {
    const Palette& pal = palette();
    fillRR(g, area.X, area.Y, area.Width, area.Height, 0, pal.bg);

    // header 56
    float hdr_h = 56;
    RectF hdr(area.X, area.Y, area.Width, hdr_h);
    Pen sep(pal.divider, 1.0f);
    g.DrawLine(&sep, hdr.X, hdr.Y + hdr_h, hdr.X + hdr.Width, hdr.Y + hdr_h);
    // # + name + sub + actions
    auto* ch = activeChannel();
    drawText_(g, L"#", hdr.X + 18, hdr.Y + 16, 16, 14.0f, pal.text_muted, StringAlignmentNear);
    drawText_(g, ch->name, hdr.X + 36, hdr.Y + 14, 200, 11.0f, pal.text,
              StringAlignmentNear, FontStyleBold);
    drawText_(g, ch->is_market ? L"社区交易市场（出售 .cfg / 灵敏度配置）" : L"官方频道",
              hdr.X + 36, hdr.Y + 32, 300, 8.5f, pal.text_muted);
    // actions: search / more
    {
        float ax = hdr.X + hdr.Width - 14 - 34 * 2 - 4;
        for (int i = 0; i < 2; ++i) {
            RectF ar(ax, hdr.Y + 11, 34, 34);
            bool hov = inRect(g_mouse, ar);
            if (hov) fillRR(g, ar.X, ar.Y, ar.Width, ar.Height, 8, pal.card);
            icons::Name n = (i == 0) ? icons::Name::Search : icons::Name::More;
            icons::drawSvg(g, n, ar.X + 8, ar.Y + 8, 18, hov ? pal.text : pal.text_muted);
            ax += 38;
        }
    }

    // 如果 market 频道：market view 顶到 stream 区
    if (ch->is_market) {
        ::paintMarketView(g, RectF(area.X, area.Y + hdr_h, area.Width, area.Height - hdr_h));
        return;
    }

    // stream
    float comp_h = 64;
    RectF stream(area.X, area.Y + hdr_h, area.Width, area.Height - hdr_h - comp_h);

    auto& msgs = streamFor(g_active);
    float my = stream.Y + 12;
    float maxw = stream.Width - 32;
    for (size_t i = 0; i < msgs.size(); ++i) {
        const Msg& m = msgs[i];
        const Msg* prev = (i > 0) ? &msgs[i - 1] : nullptr;
        bool prev_same = prev && prev->kind == MsgKind::Text && m.kind == MsgKind::Text
                         && wcscmp(prev->from, m.from) == 0
                         && wcscmp(m.from, L"me") != 0;
        if (my > stream.Y + stream.Height) break;
        float h = paintBubble(g, m, stream.X + 16, my, maxw, pal, prev_same, (int)i);
        my += h;
    }

    // composer
    paintComposer(g, RectF(area.X, area.Y + area.Height - comp_h, area.Width, comp_h));

    // picker
    if (g_picker_open || g_picker_t.value() > 0.001f) {
        paintPicker(g, area.X + 14, area.Y + area.Height - comp_h);
    }
}

// ============== 顶层 entry ==============
void paintChatViewTop(Graphics& g, RectF area) {
    // view 切换 fade（共用 g_view_fade）
    float op = g_view_fade.started ? g_view_fade.value() : 1.0f;
    if (op < 0.999f) {
        // 平移 + 透明 — 简单做法：偏 8px 上 + 全局 alpha 控不住，所以直接改 area.Y
        // 让用户感受到切换；alpha 影响子调用复杂，暂只做 translate
        area.Y += (1.0f - op) * 8.0f;
    }
    float lw = 240;
    paintChatList(g, RectF(area.X, area.Y, lw, area.Height));
    paintChatPane(g, RectF(area.X + lw + 1, area.Y, area.Width - lw - 1, area.Height));
}

}  // namespace chatv
