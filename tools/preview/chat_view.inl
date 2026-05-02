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

// 官方频道 — 没有 touhou/vrchat（按用户要求剔除）
const Channel kChannels[] = {
    { L"announcements", L"announcements", L"IMPORTANT", 2, false },
    { L"rules",         L"rules",         L"IMPORTANT", 0, false },
    { L"general",       L"general",       L"GENERAL",   3, false },
    { L"random",        L"random",        L"GENERAL",   0, false },
    { L"helpdesk",      L"helpdesk",      L"GENERAL",   1, false },
    { L"cs2",           L"cs2",           L"GAMES",    12, false },
    { L"market",        L"market",        L"SHOP",      0, true  },
    { L"trades",        L"trades",        L"SHOP",      0, false },
};
const wchar_t* kGroups[] = { L"IMPORTANT", L"GENERAL", L"GAMES", L"SHOP" };

enum class MsgKind { Text, Sticker, Gif, System, DayDivider, Typing, LinkCard, Video };

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

// 各频道默认 sample
std::vector<Msg>& streamFor(const wchar_t* chid) {
    static std::vector<Msg> g_general = {
        {MsgKind::DayDivider,L"",L"",L"",L"今天"},
        {MsgKind::Text, L"yuki", L"yuki", L"online", L"早！谁今晚有空开把 cs2 啊", L"09:42"},
        {MsgKind::Text, L"yuki", L"yuki", L"online", L"我打 premier 单挑掉分到一个怀疑人生", L"09:42"},
        {MsgKind::Text, L"reimu",L"博丽灵梦",L"away", L"我！但是 8 点之后", L"09:50"},
        {MsgKind::Text, L"me",   L"",       L"online",L"+1 等我下班", L"10:01", true},
        {MsgKind::Text, L"yuki", L"yuki", L"online", L"晚上一起开 cs 吗？", L"10:42"},
        {MsgKind::Sticker,L"sakuya",L"十六夜咲夜",L"busy",L"🎮", L"10:43"},
        {MsgKind::Text, L"me",   L"",       L"online",L"开开开 8 点 disc 见", L"10:44", true},
        // 引用气泡 (Discord 风)
        {MsgKind::Text, L"reimu",L"博丽灵梦",L"away", L"那我先去打两把热身", L"10:45",
         false, L"yuki", L"晚上一起开 cs 吗？"},
        // 链接卡
        {MsgKind::LinkCard, L"yuki", L"yuki", L"online",
         L"https://store.steampowered.com/app/730/CounterStrike_2/", L"10:46",
         false, L"", L"",
         L"Counter-Strike 2 — Steam", L"store.steampowered.com"},
        // 视频消息
        {MsgKind::Video, L"sakuya", L"十六夜咲夜", L"busy",
         L"highlight_clutch.mp4", L"10:48", false, L"", L"", L"", L"", 14},
        {MsgKind::Typing, L"yuki", L"yuki", L"online"},
    };
    static std::vector<Msg> g_announcements = {
        {MsgKind::DayDivider,L"",L"",L"",L"今天"},
        {MsgKind::System, L"system",L"",L"",L"📢 Launcher v0.1.0 已发布"},
        {MsgKind::Text, L"reimu", L"博丽灵梦", L"away", L"周六晚 8 点联机，记得报名", L"08:00"},
    };
    static std::vector<Msg> g_rules = {
        {MsgKind::DayDivider,L"",L"",L"",L"04-01"},
        {MsgKind::System, L"system",L"",L"",L"请阅读社区规则"},
    };
    static std::vector<Msg> g_random = {
        {MsgKind::DayDivider,L"",L"",L"",L"今天"},
        {MsgKind::Text, L"yuki", L"yuki", L"online", L"今天天气不错", L"08:30"},
    };
    static std::vector<Msg> g_helpdesk = {
        {MsgKind::DayDivider,L"",L"",L"",L"今天"},
        {MsgKind::Text, L"flandre", L"芙兰朵露", L"offline", L"我登录不上 ，help", L"02:14"},
    };
    static std::vector<Msg> g_cs2 = {
        {MsgKind::DayDivider,L"",L"",L"",L"今天"},
        {MsgKind::Text, L"reimu", L"博丽灵梦", L"away", L"matchmaking 又寄了…", L"09:18"},
        {MsgKind::Text, L"me",    L"",       L"online", L"steam 又抽风", L"09:20", true},
    };
    static std::vector<Msg> g_trades = {
        {MsgKind::DayDivider,L"",L"",L"",L"04-25"},
        {MsgKind::Text, L"flandre", L"芙兰朵露", L"offline", L"出 awp ｜印花集 价好", L"03:14"},
    };
    static std::vector<Msg> g_empty;

    if (wcscmp(chid, L"general")       == 0) return g_general;
    if (wcscmp(chid, L"announcements") == 0) return g_announcements;
    if (wcscmp(chid, L"rules")         == 0) return g_rules;
    if (wcscmp(chid, L"random")        == 0) return g_random;
    if (wcscmp(chid, L"helpdesk")      == 0) return g_helpdesk;
    if (wcscmp(chid, L"cs2")           == 0) return g_cs2;
    if (wcscmp(chid, L"trades")        == 0) return g_trades;
    return g_empty;
}

// ============== 状态 ==============
const wchar_t* g_active{L"general"};
std::wstring   g_draft;
int            g_draft_caret{0};
bool           g_picker_open{false};
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
        drawText_(g, L"▾", area.X + 10, row_y + 4, 12, 8.0f, pal.text_muted);
        drawText_(g, gname, area.X + 26, row_y + 4, 200,
                  8.0f, pal.text_muted, StringAlignmentNear, FontStyleBold);
        row_y += 24;

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
    bool isSticker = (m.kind == MsgKind::Sticker);
    bool isGif = (m.kind == MsgKind::Gif);
    bool isLink = (m.kind == MsgKind::LinkCard);
    bool isVideo = (m.kind == MsgKind::Video);

    // 测算文字宽 (text only)
    float content_w_max = maxw * 0.6f;
    if (isLink || isVideo) content_w_max = std::min(maxw * 0.72f, 280.0f);

    float bubble_h = 0;
    float bubble_w = 0;
    std::wstring body_w = m.body;

    if (isSticker) {
        bubble_h = 64; bubble_w = 64;
    } else if (isGif) {
        bubble_h = 140; bubble_w = 220;
    } else if (isLink) {
        bubble_h = 76; bubble_w = 280;
    } else if (isVideo) {
        bubble_h = 160; bubble_w = 240;
    } else {
        // 简单换行：按宽度估算行数
        float meas = measureText(g, body_w.c_str(), 9.5f).Width + 50; // +meta
        bubble_w = std::min(meas + 24, content_w_max);
        int approx_chars_per_row = (int)((bubble_w - 24) / 7.0f);
        if (approx_chars_per_row < 8) approx_chars_per_row = 8;
        int rows = std::max(1, (int)body_w.size() / approx_chars_per_row + 1);
        bubble_h = (float)(rows * 18 + 16);
    }

    bool has_reply = m.reply_excerpt && m.reply_excerpt[0];
    if (has_reply) bubble_h += 26;

    // 作者名（群聊 + 不是连续相同作者 + 不是自己 + 非贴纸gif）
    bool show_author = !me && m.author && m.author[0] && !prev_same_author && !isSticker && !isGif;
    if (show_author) bubble_h += 16;

    float gutter = 36.0f;
    float bubble_x;
    if (me) {
        bubble_x = x + maxw - 18 - bubble_w;
    } else {
        bubble_x = x + 4 + gutter;
    }

    // 头像（只在第一条画）
    if (!me && !prev_same_author) {
        drawAvatar(g, x + 4, y + bubble_h - 28, 14, m.author, m.status, pal);
        // 头像右键 hit area → at menu
        int idx = msg_index;
        hit(RectF(x + 4, y + bubble_h - 28, 28, 28), [idx]() {
            // 左键打开 popover, 这里映射成插入 @
            std::wstring at = std::wstring(L"@") + chatv::streamFor(g_active)[idx].author + L" ";
            g_draft += at;
            g_draft_caret = (int)g_draft.size();
        }, true);
    }

    // 气泡
    if (isSticker) {
        Font f(kFontFace, 36.0f, FontStyleRegular, UnitPixel);
        SolidBrush b(pal.text);
        StringFormat fmt; fmt.SetAlignment(StringAlignmentCenter); fmt.SetLineAlignment(StringAlignmentCenter);
        g.DrawString(m.body, -1, &f, RectF(bubble_x, y, bubble_w, bubble_h), &fmt, &b);
    } else if (isGif) {
        // 渐变方块 + GIF 标
        LinearGradientBrush lg(PointF(bubble_x, y), PointF(bubble_x + bubble_w, y + bubble_h),
                               Color(255, 0x7C, 0x9D, 0xD9), pal.primary);
        GraphicsPath gp; buildRoundRect(gp, bubble_x, y, bubble_w, bubble_h, 12);
        g.FillPath(&lg, &gp);
        drawText_(g, m.body, bubble_x, y + bubble_h / 2 - 8, bubble_w, 11.0f,
                  Color(255, 255, 255, 255), StringAlignmentCenter, FontStyleBold);
        // GIF 标
        Color tag_bg(140, 0, 0, 0);
        fillRR(g, bubble_x + 6, y + 6, 28, 14, 4.0f, tag_bg);
        drawText_(g, L"GIF", bubble_x + 6, y + 6 + 1, 28, 7.5f,
                  Color(255, 255, 255, 255), StringAlignmentCenter, FontStyleBold);
    } else if (isLink) {
        Color cardC = me ? pal.primary : pal.card;
        fillRR(g, bubble_x, y, bubble_w, bubble_h, 12, cardC);
        Color barC(255, pal.primary.GetR(), pal.primary.GetG(), pal.primary.GetB());
        fillRR(g, bubble_x, y, 3, bubble_h, 1.5f, barC);
        // host + title + url
        Color textC = me ? Color(255, 255, 255, 255) : pal.text;
        Color mutedC = me ? Color(220, 255, 255, 255) : pal.text_muted;
        drawText_(g, m.link_host, bubble_x + 14, y + 8, bubble_w - 28,
                  7.5f, mutedC, StringAlignmentNear);
        drawText_(g, m.link_title, bubble_x + 14, y + 22, bubble_w - 28,
                  9.0f, textC, StringAlignmentNear, FontStyleBold);
        drawText_(g, m.body, bubble_x + 14, y + 44, bubble_w - 28,
                  8.0f, mutedC, StringAlignmentNear);
        // 链接 icon
        icons::drawSvg(g, icons::Name::Link, bubble_x + bubble_w - 30, y + bubble_h - 28, 18,
                       me ? Color(220, 255, 255, 255) : pal.text_muted);
        // meta
        drawText_(g, m.time, bubble_x, y + bubble_h - 14, bubble_w - 8,
                  7.0f, mutedC, StringAlignmentFar);
    } else if (isVideo) {
        // 视频缩略 + 时长 + 播放
        LinearGradientBrush vg(PointF(bubble_x, y), PointF(bubble_x + bubble_w, y + bubble_h),
                               Color(255, 0x40, 0x36, 0x32), Color(255, 0x18, 0x14, 0x12));
        GraphicsPath vp; buildRoundRect(vp, bubble_x, y, bubble_w, bubble_h, 12);
        g.FillPath(&vg, &vp);
        // CS 文字 cover (假视频)
        Font cf(kFontFace, 16.0f, FontStyleBold, UnitPoint);
        SolidBrush csb(Color(80, 0xD9, 0x77, 0x57));
        StringFormat csf; csf.SetAlignment(StringAlignmentCenter); csf.SetLineAlignment(StringAlignmentCenter);
        g.DrawString(L"CS2", -1, &cf, RectF(bubble_x, y, bubble_w, bubble_h - 30), &csf, &csb);
        // 中央 play 按钮
        SolidBrush pbg(Color(180, 0, 0, 0));
        g.FillEllipse(&pbg, bubble_x + bubble_w/2 - 22.0f, y + (bubble_h - 30)/2 - 22.0f, 44.0f, 44.0f);
        icons::drawSvg(g, icons::Name::Play,
                       bubble_x + bubble_w/2 - 12, y + (bubble_h - 30)/2 - 12, 24,
                       Color(255, 255, 255, 255));
        // 底部 meta bar
        Color metaBar(120, 0, 0, 0);
        fillRR(g, bubble_x + 8, y + bubble_h - 24, bubble_w - 16, 16, 5.0f, metaBar);
        drawText_(g, m.body, bubble_x + 14, y + bubble_h - 22, bubble_w - 60,
                  8.0f, Color(255, 255, 255, 255), StringAlignmentNear);
        wchar_t dur[16]; swprintf_s(dur, 16, L"%d:%02d", m.video_seconds / 60, m.video_seconds % 60);
        drawText_(g, dur, bubble_x + bubble_w - 50, y + bubble_h - 22, 40,
                  8.0f, Color(255, 255, 255, 255), StringAlignmentFar, FontStyleBold);
        // time
        drawText_(g, m.time, bubble_x, y + bubble_h - 6, bubble_w - 8,
                  7.0f, Color(180, 255, 255, 255), StringAlignmentFar);
    } else {
        // text bubble
        Color cardC = me ? pal.primary : pal.card;
        fillRR(g, bubble_x, y, bubble_w, bubble_h, 14, cardC);
        if (prev_same_author) {
            // tail-stack：尾部圆角拉直 (design)
            // 这里简化，靠默认 14 半径，文字直接堆叠
        } else if (me) {
            // 右下角拉直 (design .bubble.me border-bottom-right-radius:4)
            fillRR(g, bubble_x + bubble_w - 14, y + bubble_h - 14, 14, 14, 0.0f, cardC);
            fillRR(g, bubble_x + bubble_w - 14, y + bubble_h - 14, 14, 14, 4.0f, cardC);
        } else {
            // 左下角拉直
            fillRR(g, bubble_x, y + bubble_h - 14, 14, 14, 0.0f, cardC);
            fillRR(g, bubble_x, y + bubble_h - 14, 14, 14, 4.0f, cardC);
        }

        float ty = y + 8;
        // reply (引用条)
        if (has_reply) {
            Color repBar(255, pal.primary.GetR(), pal.primary.GetG(), pal.primary.GetB());
            fillRR(g, bubble_x + 8, ty + 2, 3, 18, 1.5f, repBar);
            Color repNameC = me ? Color(255, 255, 255, 255) : pal.primary;
            Color repTextC = me ? Color(220, 255, 255, 255) : pal.text_muted;
            drawText_(g, m.reply_author, bubble_x + 16, ty + 1, bubble_w - 24,
                      7.5f, repNameC, StringAlignmentNear, FontStyleBold);
            drawText_(g, m.reply_excerpt, bubble_x + 16, ty + 11, bubble_w - 24,
                      7.5f, repTextC, StringAlignmentNear);
            ty += 24;
        }
        // 作者名
        if (show_author) {
            drawText_(g, m.author, bubble_x + 12, ty, bubble_w - 24,
                      8.0f, pal.primary, StringAlignmentNear, FontStyleBold);
            ty += 14;
        }
        // 正文
        Color textC = me ? Color(255, 255, 255, 255) : pal.text;
        // 简单 word-wrap：让 GDI+ 自动换行 — 给一个限定宽度的矩形
        Font f(kFontFace, 9.5f, FontStyleRegular, UnitPoint);
        SolidBrush bb(textC);
        StringFormat fmt; fmt.SetAlignment(StringAlignmentNear);
        RectF tr(bubble_x + 12, ty, bubble_w - 24, bubble_h - (ty - y) - 4);
        g.DrawString(body_w.c_str(), -1, &f, tr, &fmt, &bb);
        // meta (time + 双勾)
        Color metaC = me ? Color(220, 255, 255, 255) : pal.text_muted;
        drawText_(g, m.time, bubble_x, y + bubble_h - 14, bubble_w - 24,
                  7.0f, metaC, StringAlignmentFar);
        if (me) {
            Color tickC = m.read ? Color(255, 0x7D, 0xD3, 0xFC) : metaC;
            icons::drawSvg(g, icons::Name::Check2,
                           bubble_x + bubble_w - 22, y + bubble_h - 16, 14, tickC);
        }
    }

    return bubble_h + 6;
}

// ============== Composer ==============
void paintComposer(Graphics& g, RectF area) {
    const Palette& pal = palette();
    SolidBrush bg(pal.bg);
    g.FillRectangle(&bg, area.X, area.Y, area.Width, area.Height);
    Pen sep(pal.divider, 1.0f);
    g.DrawLine(&sep, area.X, area.Y, area.X + area.Width, area.Y);

    // 4 个图标 + textarea + send
    float icon_size = 28;
    float ix = area.X + 14;
    float iy = area.Y + (area.Height - icon_size) / 2;
    auto iconBtn = [&](icons::Name n, std::function<void()> click) {
        bool hov = inRect(g_mouse, RectF(ix, iy, icon_size, icon_size));
        if (hov) fillRR(g, ix, iy, icon_size, icon_size, 6, pal.card);
        icons::drawSvg(g, n, ix + 5, iy + 5, 18, hov ? pal.text : pal.text_muted);
        hit(RectF(ix, iy, icon_size, icon_size), click, true);
        ix += icon_size + 4;
    };
    iconBtn(icons::Name::Smile, [](){
        g_picker_open = !g_picker_open;
        g_picker_tab = 0;
        g_picker_t.start(g_picker_t.value(), g_picker_open ? 1.0f : 0.0f, 0.22f, 0, curve::easeOutBack);
    });
    iconBtn(icons::Name::More, [](){   // sticker tab via more icon
        g_picker_open = !g_picker_open;
        g_picker_tab = 1;
        g_picker_t.start(g_picker_t.value(), g_picker_open ? 1.0f : 0.0f, 0.22f, 0, curve::easeOutBack);
    });
    iconBtn(icons::Name::Video, [](){
        g_picker_open = !g_picker_open;
        g_picker_tab = 2;
        g_picker_t.start(g_picker_t.value(), g_picker_open ? 1.0f : 0.0f, 0.22f, 0, curve::easeOutBack);
    });
    iconBtn(icons::Name::Paperclip, [](){});

    // textarea
    float fx = ix + 4;
    float fw = area.Width - (fx - area.X) - 14 - 38 - 8;
    float fh = area.Height - 16;
    float fy = area.Y + 8;
    fillRR(g, fx, fy, fw, fh, 18, pal.card);
    strokeRR(g, fx, fy, fw, fh, 18,
             g_focus_composer ? pal.primary : pal.divider, g_focus_composer ? 1.5f : 1.0f);
    if (g_focus_composer) {
        Color halo(28, pal.primary.GetR(), pal.primary.GetG(), pal.primary.GetB());
        strokeRR(g, fx - 2, fy - 2, fw + 4, fh + 4, 20, halo, 4.0f);
    }
    // 文字 / placeholder
    if (g_draft.empty()) {
        drawText_(g, L"写点什么…", fx + 14, fy + 9, fw - 28, 9.5f, pal.text_muted);
    } else {
        drawText_(g, g_draft.c_str(), fx + 14, fy + 9, fw - 28, 9.5f, pal.text);
    }
    // caret
    if (g_focus_composer) {
        Font fnt(kFontFace, 9.5f, FontStyleRegular, UnitPoint);
        std::wstring sub = g_draft.substr(0, std::min((size_t)g_draft_caret, g_draft.size()));
        RectF bb; g.MeasureString(sub.c_str(), -1, &fnt, PointF(0, 0), &bb);
        int phase = (int)(g_time_in_stage * 1000) % 1000;
        if (phase < 500) {
            Pen p(pal.primary, 1.5f);
            float cx_ = fx + 14 + bb.Width;
            g.DrawLine(&p, cx_, fy + 9, cx_, fy + fh - 9);
        }
    }
    hit(RectF(fx, fy, fw, fh), [](){ g_focus_composer = true; }, true);

    // send btn
    float sx = area.X + area.Width - 14 - 38;
    float sy = area.Y + (area.Height - 38) / 2;
    bool can_send = !g_draft.empty();
    bool sh = inRect(g_mouse, RectF(sx, sy, 38.0f, 38.0f));
    Color sbg = !can_send ? Color(140, pal.primary.GetR(), pal.primary.GetG(), pal.primary.GetB())
                          : (sh ? pal.primary_hover : pal.primary);
    Color sglow(80, pal.primary.GetR(), pal.primary.GetG(), pal.primary.GetB());
    drawShadow(g, sx, sy, 38.0f, 38.0f, 19.0f, sglow, 4.0f, 3);
    SolidBrush sbgB(sbg);
    g.FillEllipse(&sbgB, sx, sy, 38.0f, 38.0f);
    icons::drawSvg(g, icons::Name::Send, sx + 10, sy + 10, 18, Color(255, 255, 255, 255));
    if (can_send) {
        hit(RectF(sx, sy, 38.0f, 38.0f), [](){
            // 发消息：append 到当前流
            auto& s = streamFor(g_active);
            Msg m; m.kind = MsgKind::Text; m.from = L"me"; m.author = L"";
            m.status = L"online"; m.read = false; m.time = L"now";
            // body 必须长生命周期 — 用静态 vector
            static std::vector<std::wstring> g_my_msgs;
            g_my_msgs.push_back(g_draft);
            m.body = g_my_msgs.back().c_str();
            s.push_back(m);
            g_draft.clear(); g_draft_caret = 0;
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
                    // emoji → 插入 draft
                    g_draft += val;
                    g_draft_caret = (int)g_draft.size();
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

    // 背景 radial 装饰
    Color rad1(20, pal.primary.GetR(), pal.primary.GetG(), pal.primary.GetB());
    Color rad2(15, 0x7C, 0x9D, 0xD9);
    {
        GraphicsPath cl; cl.AddRectangle(RectF(stream.X, stream.Y, stream.Width, stream.Height));
        g.SetClip(&cl);
        SolidBrush b1(rad1);
        g.FillEllipse(&b1, stream.X + stream.Width * 0.6f, stream.Y - 100.0f, 600.0f, 400.0f);
        SolidBrush b2(rad2);
        g.FillEllipse(&b2, stream.X - 100.0f, stream.Y + stream.Height * 0.6f, 500.0f, 400.0f);
        g.ResetClip();
    }

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
    float lw = 240;
    paintChatList(g, RectF(area.X, area.Y, lw, area.Height));
    paintChatPane(g, RectF(area.X + lw + 1, area.Y, area.Width - lw - 1, area.Height));
}

}  // namespace chatv
