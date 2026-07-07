// chat_paint.cpp — 聊天视图渲染层：文本排版度量、气泡、频道列表、输入区、
// 表情/贴纸 picker、公告弹窗。全部是"读状态→画 D2D"的纯渲染函数，
// 不修改业务状态（hit 表除外，paintBubble/paintChatPane 帧首重建供事件层命中）。
//
// 从 chat.cpp 拆出。事件处理(onMouse*/onChar/onKey)、tick、appendMedia 留在
// chat.cpp。所有 render helper 与状态声明见 chat_internal.h。
#include "chat.h"
#include "chat_internal.h"
#include "icons.h"
#include "palette.h"
#include "user_state.h"
#include "net.h"
#include "fetch.h"
#include "hit.h"
#include "stages.h"
#include "sticker.h"
#include "modals.h"
#include "render/primitives.h"
#include "toast.h"
#include "i18n.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace launcher::d2d::chat {

// ============== 文本排版度量 + 回复预览 helper ==============
float measureW(D2DApp& app, std::wstring_view s, IDWriteTextFormat* fmt) {
    if (s.empty() || !fmt) return 0.0f;
    DWRITE_TEXT_METRICS m{};
    if (!app.texts().measure(fmt, s, 8192.0f, 256.0f, &m)) return 0.0f;
    return m.width;
}

float spaceW(D2DApp& app, IDWriteTextFormat* fmt) {
    float w = measureW(app, L"x x", fmt) - measureW(app, L"xx", fmt);
    return w > 0.5f ? w : 4.0f;
}

float caretMeasureW(D2DApp& app, std::wstring_view s, IDWriteTextFormat* fmt) {
    if (s.empty() || !fmt) return 0.0f;
    size_t end = s.size();
    while (end > 0 && s[end - 1] == L' ') --end;
    float w = measureW(app, s.substr(0, end), fmt);
    if (end < s.size()) w += (float)(s.size() - end) * spaceW(app, fmt);
    return w;
}

std::wstring fitTextOneLine(D2DApp& app, const std::wstring& s,
                                   IDWriteTextFormat* fmt, float max_w) {
    if (s.empty() || max_w <= 4.0f || measureW(app, s, fmt) <= max_w) return s;
    const std::wstring ell = L"...";
    float ell_w = measureW(app, ell, fmt);
    if (ell_w >= max_w) return ell;
    size_t lo = 0, hi = s.size();
    while (lo < hi) {
        size_t mid = (lo + hi + 1) / 2;
        std::wstring candidate = s.substr(0, mid) + ell;
        if (measureW(app, candidate, fmt) <= max_w) lo = mid;
        else hi = mid - 1;
    }
    return s.substr(0, lo) + ell;
}

void drawTextOneLine(ID2D1DeviceContext* ctx, std::wstring_view text,
                            IDWriteTextFormat* fmt, float x, float y, float w, float h,
                            ID2D1Brush* b,
                            DWRITE_TEXT_ALIGNMENT halign,
                            DWRITE_PARAGRAPH_ALIGNMENT valign) {
    // 统一走 prim::drawTextNoWrap（promote 到 primitives.h，避免各文件重复实现）
    prim::drawTextNoWrap(ctx, text, fmt, x, y, w, h, b, halign, valign);
}

int cursorFromComposerPoint(float x) {
    if (g_composer_caret_xs.empty()) return 0;
    int best = 0;
    float best_dist = std::numeric_limits<float>::max();
    for (int i = 0; i < (int)g_composer_caret_xs.size(); ++i) {
        float d = std::abs(g_composer_caret_xs[i] - x);
        if (d < best_dist) {
            best_dist = d;
            best = i;
        }
    }
    return best;
}

struct WrappedText;  // 定义见 chat_internal.h

float dwriteMaxLineWidth(D2DApp& app, const std::wstring& text,
                                IDWriteTextFormat* fmt, float max_w, float max_h) {
    if (!fmt || text.empty()) return 0.0f;
    auto layout = app.texts().layout(fmt, text, max_w, max_h);
    if (!layout) return 0.0f;
    UINT32 hit_count = 0;
    HRESULT hr = layout->HitTestTextRange(0, (UINT32)text.size(), 0, 0,
                                          nullptr, 0, &hit_count);
    if (hr != E_NOT_SUFFICIENT_BUFFER || hit_count == 0) return 0.0f;
    std::vector<DWRITE_HIT_TEST_METRICS> hits(hit_count);
    if (FAILED(layout->HitTestTextRange(0, (UINT32)text.size(), 0, 0,
                                        hits.data(), hit_count, &hit_count))) {
        return 0.0f;
    }
    float widest = 0.0f;
    for (UINT32 i = 0; i < hit_count; ++i) {
        widest = (std::max)(widest, hits[i].left + hits[i].width);
    }
    return widest;
}

WrappedText wrapTextForWidth(D2DApp& app, std::wstring_view src,
                                    IDWriteTextFormat* fmt, float max_w) {
    WrappedText out;
    if (!fmt || src.empty() || max_w <= 1.0f) return out;
    float line_w = 0.0f;
    size_t i = 0;
    while (i < src.size()) {
        wchar_t c = src[i];
        if (c == L'\r') { ++i; continue; }
        if (c == L'\n') {
            out.text.push_back(c);
            line_w = 0.0f;
            ++i;
            continue;
        }
        if (c == L' ' || c == L'\t') {
            float cw = spaceW(app, fmt);
            if (line_w <= 0.5f || line_w + cw > max_w) {
                if (line_w > 0.5f) {
                    out.text.push_back(L'\n');
                    line_w = 0.0f;
                }
                ++i;
                continue;
            }
            out.text.push_back(L' ');
            line_w += cw;
            ++i;
            continue;
        }

        size_t start = i;
        while (i < src.size()
               && src[i] != L'\r'
               && src[i] != L'\n'
               && src[i] != L' '
               && src[i] != L'\t') {
            ++i;
        }
        std::wstring_view run = src.substr(start, i - start);
        float run_w = measureW(app, run, fmt);
        if (run_w <= max_w) {
            if (line_w > 0.5f && line_w + run_w > max_w) {
                out.text.push_back(L'\n');
                line_w = 0.0f;
            }
            out.text.append(run.data(), run.size());
            line_w += run_w;
            continue;
        }

        if (line_w > 0.5f) {
            out.text.push_back(L'\n');
            line_w = 0.0f;
        }
        for (size_t j = 0; j < run.size(); ++j) {
            wchar_t rc = run[j];
            float cw = measureW(app, std::wstring_view(&rc, 1), fmt);
            if (line_w > 0.5f && line_w + cw > max_w) {
                out.text.push_back(L'\n');
                line_w = 0.0f;
            }
            out.text.push_back(rc);
            line_w += cw;
        }
    }
    app.texts().measure(fmt, out.text, max_w, 8192.0f, &out.metrics);
    out.line_count = out.text.empty() ? 0 : 1;
    for (wchar_t c : out.text) {
        if (c == L'\n') ++out.line_count;
    }
    out.text_h = (std::max)(out.metrics.height, out.line_count * 18.0f);
    float dwrite_w = dwriteMaxLineWidth(app, out.text, fmt, max_w, 8192.0f);
    size_t line_start = 0;
    while (line_start <= out.text.size()) {
        size_t line_end = out.text.find(L'\n', line_start);
        if (line_end == std::wstring::npos) line_end = out.text.size();
        std::wstring_view line(out.text.data() + line_start, line_end - line_start);
        out.max_line_w = (std::max)(out.max_line_w, caretMeasureW(app, line, fmt));
        if (line_end >= out.text.size()) break;
        line_start = line_end + 1;
    }
    out.max_line_w = (std::max)(out.max_line_w, dwrite_w);
    return out;
}

std::wstring authorKeyFor(const Msg& m) {
    if (!m.author_key.empty()) return m.author_key;
    if (!m.peer_key.empty()) return m.peer_key;
    if (m.from == L"me") return L"me";
    return m.from;
}

bool sameGroupedAuthor(const Msg& prev, const Msg& cur) {
    if (prev.kind != MsgKind::Text || cur.kind != MsgKind::Text) return false;
    bool prev_self = isSelfMessage(prev);
    bool cur_self  = isSelfMessage(cur);
    if (prev_self != cur_self) return false;
    if (prev_self && cur_self) return true;
    return authorKeyFor(prev) == authorKeyFor(cur);
}

bool messageHasReplyPreview(const Msg& m) {
    return m.reply_to_id > 0 || !m.reply_client_msg_id.empty()
        || !m.reply_author.empty() || !m.reply_preview.empty();
}

std::wstring replyPreviewLine(const Msg& m) {
    std::wstring author = m.reply_author.empty() ? L"message" : m.reply_author;
    std::wstring preview = m.reply_preview.empty() ? L"[unavailable]" : m.reply_preview;
    std::wstring line = author + L": " + preview;
    if (line.size() > 96) line = line.substr(0, 96) + L"...";
    return line;
}

float replyPreviewHeight(const Msg& m) {
    return messageHasReplyPreview(m) ? 28.0f : 0.0f;
}

// ============== 公告弹窗 ==============
void paintAnnouncementModal(D2DApp& app, float W, float H) {
    if (!g_popup_open && g_popup_t.value() < 0.001f) return;
    float t = g_popup_t.value();
    if (t < 0.001f) return;

    const Palette& pal = palette();
    auto* ctx = app.ctx();
    auto& br = app.brushes();
    hit({ 0, 0, W, H }, [](){}, false);
    prim::fillRect(ctx, 0, 0, W, H, br.solidA(0x000000, 0.42f * t));

    float mw = (std::min)(420.0f, W - 48.0f);
    float mh = 260.0f;
    float mx = (W - mw) * 0.5f;
    float my = (H - mh) * 0.5f + (1.0f - t) * 12.0f;
    prim::drawShadow(ctx, br, mx, my, mw, mh, 12.0f, pal.shadow_card_hover, 0.75f * t, 6.0f, 5);
    prim::fillRR(ctx, mx, my, mw, mh, 12.0f, br.solidA(pal.card, t));
    prim::strokeRR(ctx, mx, my, mw, mh, 12.0f,
                   br.solidA(g_popup_announcement.severity == "critical" ? pal.primary : pal.divider, t),
                   g_popup_announcement.severity == "critical" ? 1.5f : 1.0f);

    auto* title_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(13.0f), DWRITE_FONT_WEIGHT_BOLD);
    auto* body_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(9.5f));
    auto* meta_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(8.0f), DWRITE_FONT_WEIGHT_BOLD);
    std::wstring severity = g_popup_announcement.severity == "critical" ? trW("announce.severity_critical") : trW("announce.label");
    // 徽章宽度按文案实测（英 "Critical notice" / 日「重要なお知らせ」比中「重要公告」宽）。
    float sev_w = (std::max)(72.0f, std::ceil(measureW(app, severity, meta_fmt)) + 22.0f);
    prim::fillRR(ctx, mx + 18, my + 18, sev_w, 22, 11.0f, br.solidA(pal.primary, 0.18f * t));
    prim::drawTextNoWrap(ctx, severity, meta_fmt, mx + 18, my + 22, sev_w, 14,
                    br.solidA(pal.primary, t), DWRITE_TEXT_ALIGNMENT_CENTER);

    prim::drawText_(ctx,
                    g_popup_announcement.title.empty() ? trW("announce.label") : g_popup_announcement.title,
                    title_fmt, mx + 18, my + 52, mw - 36, 28, br.solidA(pal.text, t));
    std::wstring body = g_popup_announcement.body.empty() ? trW("announce.popup_default_body") : g_popup_announcement.body;
    prim::drawText_(ctx, body, body_fmt, mx + 18, my + 86, mw - 36, 92,
                    br.solidA(pal.text_muted, t));

    LayoutRect view_btn{ mx + 18, my + mh - 56, 132, 34 };
    bool view_h = view_btn.contains(g_mouse);
    prim::fillRR(ctx, view_btn.x, view_btn.y, view_btn.w, view_btn.h, 8.0f,
                 br.solidA(view_h ? pal.bg : pal.card, t));
    prim::strokeRR(ctx, view_btn.x, view_btn.y, view_btn.w, view_btn.h, 8.0f,
                   br.solidA(pal.divider, t));
    prim::drawText_(ctx, trW("announce.view"), body_fmt, view_btn.x, view_btn.y + 8, view_btn.w, 18,
                    br.solidA(pal.text, t), DWRITE_TEXT_ALIGNMENT_CENTER);
    hit(view_btn, [](){
        std::string id = g_popup_announcement.id;
        g_popup_open = false;
        g_popup_t.start(g_popup_t.value(), 0.0f, 0.14f, 0, curve::easeOutCubic);
        markAnnouncementsReadLocal(true, id);
        switchChannel(L"announcements");
        stages::g_view = stages::View::Chat;
    }, true);

    LayoutRect ok_btn{ mx + mw - 18 - 112, my + mh - 56, 112, 34 };
    bool ok_h = ok_btn.contains(g_mouse);
    prim::fillRR(ctx, ok_btn.x, ok_btn.y, ok_btn.w, ok_btn.h, 8.0f,
                 br.solidA(ok_h ? pal.primary_hover : pal.primary, t));
    prim::drawText_(ctx, trW("announce.got_it"), body_fmt, ok_btn.x, ok_btn.y + 8, ok_btn.w, 18,
                    br.solidA(0xFFFFFF, t), DWRITE_TEXT_ALIGNMENT_CENTER);
    hit(ok_btn, [](){
        std::string id = g_popup_announcement.id;
        g_popup_open = false;
        g_popup_t.start(g_popup_t.value(), 0.0f, 0.14f, 0, curve::easeOutCubic);
        markAnnouncementsReadLocal(true, id);
    }, true);
}
// ============== 频道列表 ==============
void paintChatList(D2DApp& app, float ax, float ay, float aw, float ah) {
    const Palette& pal = palette();
    auto* ctx = app.ctx();
    auto& br = app.brushes();

    prim::fillRect(ctx, ax, ay, aw, ah, br.solid(pal.bg));
    prim::drawLine(ctx, ax + aw, ay, ax + aw, ay + ah,
                   br.solid(pal.divider), 1.0f);

    auto* hdr_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(11.0f),
                                       DWRITE_FONT_WEIGHT_BOLD);
    auto* grp_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(8.0f),
                                       DWRITE_FONT_WEIGHT_BOLD);
    auto* ch_fmt  = app.texts().format(L"Microsoft YaHei UI", ptToDip(9.5f));
    auto* ch_active = app.texts().format(L"Microsoft YaHei UI", ptToDip(9.5f),
                                         DWRITE_FONT_WEIGHT_BOLD);

    // header
    float hy = ay + 12;
    prim::fillRR(ctx, ax + 14, hy - 2, 26, 26, 7.0f, br.solid(pal.primary));
    icons::drawIcon(app, icons::Name::Logo, ax + 14 + 5, hy - 2 + 5, 16, 0xFFFFFFFF);
    prim::drawText_(ctx, L"Launcher Server", hdr_fmt,
                    ax + 50, hy + 2, 200, 22, br.solid(pal.text));
    prim::drawLine(ctx, ax + 8, ay + 44, ax + aw - 8, ay + 44,
                   br.solid(pal.divider), 1.0f);

    std::vector<std::wstring> groups;
    std::set<std::wstring> seen_groups;
    for (auto* g : kGroups) {
        for (auto& c : g_channels) {
            if (c.group && wcscmp(c.group, g) == 0 && seen_groups.insert(g).second) {
                groups.emplace_back(g);
                break;
            }
        }
    }
    for (auto& c : g_channels) {
        std::wstring group = c.group ? c.group : L"GENERAL";
        if (seen_groups.insert(group).second) groups.push_back(std::move(group));
    }

    float row_y = ay + 50;
    for (auto& group_name : groups) {
        const wchar_t* gname = group_name.c_str();
        LayoutRect ghead{ ax + 6, row_y, aw - 12, 22 };
        bool ghov = ghead.contains(g_mouse);
        if (ghov) {
            prim::fillRR(ctx, ghead.x, ghead.y, ghead.w, ghead.h, 4.0f,
                         br.solidA(pal.text, 0.05f));
        }
        // v: 0=完全折叠 1=完全展开。静止态由 bool 派生（默认展开=1），
        // 只在被点击过的分组才有 tween，保证 smoke 屏 05 逐像素不变。
        float v = groupAnimValue(group_name);
        prim::drawText_(ctx, v > 0.5f ? L"▾" : L"▸", grp_fmt,
                        ax + 10, row_y + 4, 12, 14,
                        br.solid(pal.text_muted));
        prim::drawText_(ctx, gname, grp_fmt,
                        ax + 26, row_y + 4, 200, 14,
                        br.solid(pal.text_muted));
        std::wstring gn = group_name;
        hit(ghead, [gn]() { toggleGroupCollapsed(gn); }, true);
        row_y += 24;

        // 子频道带：高度按 v 插值裁剪，下方分组随之平滑滑动。
        // +6 尾 pad 在两态都恒定（今天展开 +30*n+6 / 折叠 +6），只插值子带。
        int nchild = 0;
        for (auto& c : g_channels) if (wcscmp(c.group, gname) == 0) ++nchild;
        float full_h  = nchild * 30.0f;
        float drawn_h = full_h * v;
        if (drawn_h > 0.5f) {
            ctx->PushAxisAlignedClip(
                D2D1::RectF(ax, row_y, ax + aw, row_y + drawn_h),
                D2D1_ANTIALIAS_MODE_ALIASED);
            // 延迟一点起淡入：v=1 时 a=1（与今天完全一致），先揭示再显影。
            float a = v <= 0.15f ? 0.0f : (v - 0.15f) / 0.85f;
            float cy = row_y;
            for (auto& c : g_channels) {
                if (wcscmp(c.group, gname) != 0) continue;
                bool active = (c.slug == g_active);
                LayoutRect cr{ ax + 6, cy, aw - 12, 28 };
                bool hov = cr.contains(g_mouse);
                if (active) {
                    prim::fillRR(ctx, cr.x, cr.y, cr.w, cr.h, 6.0f,
                                 br.solidA(pal.primary, 0.14f * a));
                } else if (hov) {
                    prim::fillRR(ctx, cr.x, cr.y, cr.w, cr.h, 6.0f,
                                 br.solidA(pal.text, 0.04f * a));
                }
                uint32_t tc = active ? pal.primary : pal.text_muted;
                prim::drawText_(ctx, L"#", grp_fmt,
                                cr.x + 12, cr.y + 6, 14, 16,
                                br.solidA(tc, a));
                prim::drawText_(ctx, c.name, active ? ch_active : ch_fmt,
                                cr.x + 26, cr.y + 7, cr.w - 60, 18,
                                br.solidA(active ? pal.text : pal.text_muted, a));
                if (c.notice) {
                    prim::fillCircle(ctx, cr.x + cr.w - 18, cr.y + 14, 4.0f,
                                     br.solidA(pal.primary, a));
                    prim::strokeCircle(ctx, cr.x + cr.w - 18, cr.y + 14, 4.0f,
                                       br.solidA(pal.bg, a), 1.5f);
                }
                // 命中门控：仅当展开够（v>0.5）且行中点落在裁剪带内才注册，
                // 避免半折叠时点到被裁掉一半的行误触。
                if (v > 0.5f && (cy + 15.0f) < row_y + drawn_h) {
                    std::wstring tgt = c.slug;
                    hit(cr, [tgt]() { switchChannel(tgt); }, true);
                }
                cy += 30.0f;
            }
            ctx->PopAxisAlignedClip();
        }
        row_y += drawn_h;   // 下方分组随子带高度平滑滑动
        row_y += 6;         // 恒定尾 pad，两态一致
    }
}

// 纯量高度 — 跟 paintBubble 完全镜像但不画任何 D2D / 不 push hit。
// 用来在真画之前一次过算 total，给 scroll offset 定位。
float measureBubbleHeight(D2DApp& app, const Msg& m, float maxw, bool prev_same_author) {
    auto* body_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(9.5f));
    // 空 body 直接占 0 高度（与 paintBubble 行为一致）
    if (m.kind == MsgKind::Text && m.body.empty()) return 0;
    if (m.kind == MsgKind::DayDivider) return 30;
    if (m.kind == MsgKind::System)     return 32;
    if (m.kind == MsgKind::Image || m.kind == MsgKind::Gif) {
        float bub_w = 240, bub_h = 180;
        D2D1_SIZE_F sz{ 0, 0 };
        // measure 绝不触发解码 —— 只读已知 intrinsic 尺寸（worker 完成后填充）。
        // 未知则用默认 240×180 框；真实尺寸到位后下一帧自然 reflow。
        std::optional<SIZE> isz;
        if (m.kind == MsgKind::Gif) isz = app.gifs().intrinsicSize(m.body);
        if (!isz)                   isz = app.images().intrinsicSize(m.body);
        if (isz) { sz.width = (float)isz->cx; sz.height = (float)isz->cy; }
        if (sz.width > 0 && sz.height > 0) {
            float aspect = sz.height / sz.width;
            float max_w = (std::min)(maxw * 0.55f, 320.0f);
            bub_w = (std::min)(max_w, sz.width);
            bub_h = bub_w * aspect;
            if (bub_h > 240) { bub_h = 240; bub_w = bub_h / aspect; }
        }
        return (prev_same_author ? bub_h : bub_h + 22) + replyPreviewHeight(m) + reactionRowHeight(m) + 6;
    }
    if (m.kind == MsgKind::Video) {
        return (prev_same_author ? 140.0f : 162.0f) + replyPreviewHeight(m) + reactionRowHeight(m) + 6;
    }
    if (m.kind == MsgKind::Sticker) {
        return (prev_same_author ? 100.0f : 122.0f) + replyPreviewHeight(m) + reactionRowHeight(m) + 6;
    }
    // text — 处理 launcher://pack/ link 卡片
    auto find_url = [](const std::wstring& s) -> std::wstring {
        size_t p = s.find(L"launcher://");
        if (p == std::wstring::npos) {
            p = s.find(L"https://");
            if (p == std::wstring::npos) p = s.find(L"http://");
        }
        if (p == std::wstring::npos) return {};
        size_t e = p;
        while (e < s.size() && s[e] > 0x20 && s[e] != L' ') e++;
        return s.substr(p, e - p);
    };
    std::wstring url = find_url(m.body);
    if (!url.empty() && url.compare(0, 15, L"launcher://pack/") == 0) {
        return (prev_same_author ? 88.0f : 110.0f) + replyPreviewHeight(m) + reactionRowHeight(m) + 6;
    }
    if (m.body.empty()) {
        // 空消息 — 不算高度（实际 paint 也跳过）
        return prev_same_author ? 0.0f : 22.0f + 6.0f;
    }
    float bub_max_w = (std::min)(maxw * 0.65f, 480.0f);
    float text_w = bub_max_w - 28;
    WrappedText layout = wrapTextForWidth(app, m.body, body_fmt, text_w);
    float bub_h = (std::max)(layout.text_h + 18.0f, 30.0f);
    // 发送状态（sending.../send failed）作为气泡下方的小 caption，
    // 不再塞进气泡内部 —— 否则短消息一发出气泡会突然撑大到 caption 宽度，
    // 确认后又缩回，产生"突然变很大"的跳动。caption 只占一行固定高度。
    if (isSelfMessage(m) && m.send_state != MsgSendState::Sent) {
        bub_h += 14.0f;
    }
    if (!url.empty()) bub_h += 4;
    return (prev_same_author ? bub_h : bub_h + 22) + replyPreviewHeight(m) + reactionRowHeight(m) + 6;
}

// ============== 单条气泡 ==============
// 自己消息靠右 / 别人靠左。idx = 在 streamFor(g_active) 里的位置，用于消息 hit 注册（右键菜单）。
float paintBubble(D2DApp& app, const Msg& m, int idx, float x, float y, float maxw,
                         bool prev_same_author) {
    const Palette& pal = palette();
    auto* ctx = app.ctx();
    auto& br = app.brushes();

    // Text 类型 + body 全空 — 整条直接跳过（避免画了头像/作者却没气泡的"幽灵行"）
    if (m.kind == MsgKind::Text && m.body.empty()) {
        return 0;
    }

    if (m.kind == MsgKind::DayDivider) {
        auto* pill_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(7.5f));
        float tw = measureW(app, m.body, pill_fmt) + 24;
        float bx = x + (maxw - tw) * 0.5f;
        prim::fillRR(ctx, bx, y + 6, tw, 18, 9.0f, br.solidA(pal.text, 0.05f));
        prim::drawText_(ctx, m.body, pill_fmt,
                        bx, y + 9, tw, 14,
                        br.solid(pal.text_muted),
                        DWRITE_TEXT_ALIGNMENT_CENTER);
        return 30;
    }
    if (m.kind == MsgKind::System) {
        auto* sys_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(8.5f));
        float tw = measureW(app, m.body, sys_fmt) + 24;
        float bx = x + (maxw - tw) * 0.5f;
        prim::fillRR(ctx, bx, y + 4, tw, 22, 11.0f, br.solidA(pal.text, 0.05f));
        prim::drawText_(ctx, m.body, sys_fmt,
                        bx, y + 8, tw, 16,
                        br.solid(pal.text_muted),
                        DWRITE_TEXT_ALIGNMENT_CENTER);
        return 32;
    }

    bool me = isSelfMessage(m);

    auto* author_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(8.0f),
                                          DWRITE_FONT_WEIGHT_BOLD);
    auto* time_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(7.0f));
    auto* body_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(9.5f));

    // 头像 28×28 — me 在右边，别人在左边
    constexpr float ar = 14.0f;
    constexpr float gap = 10.0f;
    float avatar_x = me ? (x + maxw - ar * 2) : x;
    float ay = y + 4;
    if (!prev_same_author) {
        bool drew_real = false;
        // me 用真头像（g_avatar_path BitmapBrush 圆形裁剪）
        if (me && !g_avatar_path.empty()) {
            auto* abmp = app.images().fromFile(g_avatar_path, 28);
            if (abmp) {
                D2D1_BITMAP_BRUSH_PROPERTIES bp = D2D1::BitmapBrushProperties(
                    D2D1_EXTEND_MODE_CLAMP, D2D1_EXTEND_MODE_CLAMP,
                    D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                ComPtr<ID2D1BitmapBrush> bb;
                if (SUCCEEDED(ctx->CreateBitmapBrush(abmp, bp, &bb))) {
                    D2D1_SIZE_F sz = abmp->GetSize();
                    if (sz.width > 0 && sz.height > 0) {
                        float sx = (ar * 2) / sz.width;
                        float sy = (ar * 2) / sz.height;
                        auto mt = D2D1::Matrix3x2F::Scale({sx, sy}, {0, 0})
                                * D2D1::Matrix3x2F::Translation(avatar_x, ay);
                        bb->SetTransform(mt);
                        ctx->FillEllipse(D2D1::Ellipse({avatar_x + ar, ay + ar}, ar, ar), bb.Get());
                        drew_real = true;
                    }
                }
            }
        }
        if (!drew_real) {
            prim::fillCircle(ctx, avatar_x + ar, ay + ar, ar, br.solid(pal.primary));
            // 自己用 nickname 首字，否则用 from 首字（对方 UUID 前 8 字的首字符）
            std::wstring avatar_label = displayAuthorFor(m, app.hwnd());
            wchar_t key = avatar_label.empty() ? L'?' : avatar_label[0];
            wchar_t initial[2] = { (wchar_t)towupper(key), 0 };
            auto* init_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(8.5f),
                                                DWRITE_FONT_WEIGHT_BOLD);
            prim::drawText_(ctx, initial, init_fmt,
                            avatar_x, ay, ar * 2, ar * 2,
                            br.solid(0xFFFFFFFF),
                            DWRITE_TEXT_ALIGNMENT_CENTER,
                            DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
        // 头像左键看主页（me 自己跳过）
        std::wstring profile_key = me ? selfAuthorKey() : (!m.peer_key.empty() ? m.peer_key : m.from);
        if (!profile_key.empty()) {
            g_avatar_hits.push_back({ { avatar_x, ay, ar * 2, ar * 2 }, profile_key });
            hit({ avatar_x, ay, ar * 2, ar * 2 }, [profile_key](){
                auto* p = new std::wstring(profile_key);
                PostMessageW(GetActiveWindow(), WM_APP + 37, 0, (LPARAM)p);
            }, true);
        }
    }
    // bub 起点：left/right
    float bub_inner_w_max = (std::min)(maxw - ar * 2 - gap, 480.0f);
    auto bub_x_for = [&](float bub_w) -> float {
        if (me) return avatar_x - gap - bub_w;
        return avatar_x + ar * 2 + gap;
    };
    float reply_h = replyPreviewHeight(m);
    float reply_y = y + (prev_same_author ? 0 : 22);
    float bub_y = reply_y + reply_h;
    if (reply_h > 0.0f) {
        float ref_w = (std::min)(maxw * 0.58f, 420.0f);
        float ref_x = me ? (avatar_x - gap - ref_w) : (avatar_x + ar * 2 + gap);
        bool clickable = m.reply_to_id > 0;
        prim::fillRR(ctx, ref_x, reply_y + 2, ref_w, 22, 7.0f,
                     br.solidA(me ? 0xFFFFFF : pal.primary, me ? 0.12f : 0.10f));
        prim::fillRR(ctx, ref_x + 8, reply_y + 6, 3, 14, 1.5f,
                     br.solidA(me ? 0xFFFFFF : pal.primary, clickable ? 0.80f : 0.42f));
        auto* ref_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(8.0f));
        std::wstring ref_line = fitTextOneLine(app, replyPreviewLine(m), ref_fmt, ref_w - 24);
        drawTextOneLine(ctx, ref_line, ref_fmt,
                        ref_x + 16, reply_y + 5, ref_w - 24, 14,
                        br.solidA(me ? 0xFFFFFF : pal.text_muted, clickable ? 0.92f : 0.68f));
        if (clickable) {
            int64_t target_id = m.reply_to_id;
            std::wstring target_slug = g_active;
            hit({ ref_x, reply_y + 2, ref_w, 22 }, [target_slug, target_id]() {
                focusMessage(target_slug, target_id);
            }, true);
        }
    }
    if (!prev_same_author) {
        // author + time 在气泡上方那一行
        // me：和气泡一样靠右；别人：和气泡靠左
        // 自己显示真昵称（不是 "me" 字面量）
        std::wstring author_disp;
        if (me) {
            author_disp = !m.author.empty() ? m.author
                        : (g_user.nickname.empty() ? trW("chat.me") : g_user.nickname);
        } else {
            author_disp = m.author.empty() ? m.from : m.author;
        }
        std::wstring author_display = displayAuthorFor(m, app.hwnd());
        float aw_ = measureW(app, author_display, author_fmt);
        float tw_ = m.time.empty() ? 0 : (measureW(app, m.time, time_fmt) + 8);
        float meta_w = aw_ + tw_;
        float meta_x = me ? (avatar_x - gap - meta_w) : (avatar_x + ar * 2 + gap);
        prim::drawText_(ctx, author_display, author_fmt,
                        meta_x, y + 2, aw_ + 4, 14,
                        br.solid(me ? pal.primary : pal.text));
        if (!m.time.empty()) {
            prim::drawText_(ctx, m.time, time_fmt,
                            meta_x + aw_ + 8, y + 4, tw_, 12,
                            br.solid(pal.text_muted));
        }
    }

    // ---------- 媒体气泡 (Image/Gif/Video) ----------
    if (m.kind == MsgKind::Image || m.kind == MsgKind::Gif) {
        float bub_w = 240, bub_h = 180;
        ID2D1Bitmap* draw_bmp = nullptr;
        D2D1_SIZE_F sz = { 0, 0 };
        float media_op = 1.0f;
        constexpr uint32_t kMediaTargetPx = 320;   // decode-to-display-size

        // 布局尺寸用 intrinsic（与 measureBubbleHeight 一致，避免缩放位图导致的抖动）。
        std::optional<SIZE> isz;
        if (m.kind == MsgKind::Gif) isz = app.gifs().intrinsicSize(m.body);
        if (!isz)                   isz = app.images().intrinsicSize(m.body);
        if (isz) { sz.width = (float)isz->cx; sz.height = (float)isz->cy; }

        if (m.kind == MsgKind::Gif) {
            // GIF 多帧 — IWICBitmapDecoder GetFrameCount + /grctlext/Delay
            auto* anim = app.gifs().fromFile(m.body, kMediaTargetPx);
            if (anim) {
                draw_bmp = app.gifs().frameAt(anim, stages::g_time_in_stage);
                if (sz.width <= 0) { sz.width = (float)anim->width; sz.height = (float)anim->height; }
            }
        }
        if (!draw_bmp) {
            // Image 或 GIF 解码失败 → 退到单帧 ID2D1Bitmap（带淡入不透明度）
            draw_bmp = app.images().fromFile(m.body, kMediaTargetPx, &m.blurhash, &media_op);
            if (draw_bmp && sz.width <= 0) sz = draw_bmp->GetSize();
        }

        // 尺寸用 intrinsic（sz 已由 intrinsicSize/位图填充）；占位框也据此定形，
        // 图到位后不跳变。未知 sz 时保持默认 240×180。
        if (sz.width > 0 && sz.height > 0) {
            float aspect = sz.height / sz.width;
            float max_w = (std::min)(maxw * 0.55f, 320.0f);
            bub_w = (std::min)(max_w, sz.width);
            bub_h = bub_w * aspect;
            if (bub_h > 240) { bub_h = 240; bub_w = bub_h / aspect; }
        }
        float bub_x = bub_x_for(bub_w);
        if (draw_bmp) {
            // 真圆角 mask（之前 PushAxisAlignedClip 只裁矩形 4 角是直的）
            prim::pushLayerRR(ctx, app.factory(), bub_x, bub_y, bub_w, bub_h, 12.0f);
            ctx->DrawBitmap(draw_bmp, D2D1::RectF(bub_x, bub_y, bub_x + bub_w, bub_y + bub_h),
                            media_op, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            prim::popLayer(ctx);
            prim::strokeRR(ctx, bub_x, bub_y, bub_w, bub_h, 12.0f,
                           br.solidA(pal.divider, 0.5f), 1.0f);
        } else {
            prim::fillRR(ctx, bub_x, bub_y, bub_w, bub_h, 12.0f,
                         br.solid(pal.surface));
            prim::drawText_(ctx, m.kind == MsgKind::Gif ? trW("chat.media_gif_fallback") : trW("chat.media_image_fallback"), body_fmt,
                            bub_x, bub_y + bub_h * 0.4f, bub_w, 22,
                            br.solid(pal.text_muted),
                            DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        if (m.kind == MsgKind::Gif) {
            prim::fillRR(ctx, bub_x + bub_w - 36, bub_y + 6, 30, 16, 4,
                         br.solidA(0x000000, 0.55f));
            auto* gif_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(7.0f),
                                               DWRITE_FONT_WEIGHT_BOLD);
            prim::drawText_(ctx, L"GIF", gif_fmt,
                            bub_x + bub_w - 36, bub_y + 7, 30, 14,
                            br.solid(0xFFFFFFFF),
                            DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        // 整行 hit — 让用户右键 row 任何位置（含 avatar / meta header）都弹菜单
        // 不只是 bubble 本体（sticker 100×100 太精确，用户难命中）
        {
            float _row_h = (prev_same_author ? bub_h : bub_h + 22) + reply_h;
            g_msg_hits.push_back({ { x, y, maxw, _row_h }, idx });
        }
        return (prev_same_author ? bub_h : bub_h + 22) + reply_h + 6;
    }

    if (m.kind == MsgKind::Video) {
        float bub_w = 240, bub_h = 140;
        float bub_x = bub_x_for(bub_w);
        prim::fillRR(ctx, bub_x, bub_y, bub_w, bub_h, 12.0f,
                     br.solid(pal.surface));
        prim::fillCircle(ctx, bub_x + bub_w * 0.5f, bub_y + bub_h * 0.5f, 28,
                         br.solidA(0x000000, 0.65f));
        prim::strokeCircle(ctx, bub_x + bub_w * 0.5f, bub_y + bub_h * 0.5f, 28,
                           br.solid(0xFFFFFFFF), 2.0f);
        icons::drawIcon(app, icons::Name::Play,
                        bub_x + bub_w * 0.5f - 12,
                        bub_y + bub_h * 0.5f - 12, 24,
                        0xFFFFFFFF);
        prim::drawText_(ctx, trW("chat.click_play_video"), body_fmt,
                        bub_x, bub_y + bub_h - 24, bub_w, 18,
                        br.solid(pal.text_muted),
                        DWRITE_TEXT_ALIGNMENT_CENTER);
        // 点击 → WebView2 内嵌播放器
        std::wstring src = m.body;
        hit({ bub_x, bub_y, bub_w, bub_h }, [src](){
            auto* payload = new std::wstring(src);
            PostMessageW(GetActiveWindow(), WM_APP + 46,
                         (WPARAM)payload, 0);
        }, true);
        // 整行 hit — 让用户右键 row 任何位置（含 avatar / meta header）都弹菜单
        // 不只是 bubble 本体（sticker 100×100 太精确，用户难命中）
        {
            float _row_h = (prev_same_author ? bub_h : bub_h + 22) + reply_h;
            g_msg_hits.push_back({ { x, y, maxw, _row_h }, idx });
        }
        return (prev_same_author ? bub_h : bub_h + 22) + reply_h + 6;
    }

    if (m.kind == MsgKind::Sticker) {
        float bub_w = 100, bub_h = 100;
        float bub_x = bub_x_for(bub_w);
        // sticker 也支持 GIF
        ID2D1Bitmap* sbmp = nullptr;
        auto sd = m.body.find_last_of(L'.');
        bool s_is_gif = (sd != std::wstring::npos
                         && (m.body.substr(sd) == L".gif"
                             || m.body.substr(sd) == L".GIF"));
        if (s_is_gif) {
            auto* sa = app.gifs().fromFile(m.body, 100);
            if (sa) sbmp = app.gifs().frameAt(sa, stages::g_time_in_stage);
        }
        if (!sbmp) sbmp = app.images().fromFile(m.body, 100);
        if (sbmp) {
            prim::pushLayerRR(ctx, app.factory(), bub_x, bub_y, bub_w, bub_h, 16.0f);
            ctx->DrawBitmap(sbmp, D2D1::RectF(bub_x, bub_y, bub_x + bub_w, bub_y + bub_h),
                            1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            prim::popLayer(ctx);
        } else {
            prim::fillRR(ctx, bub_x, bub_y, bub_w, bub_h, 16.0f,
                         br.solid(pal.surface));
        }
        // 整行 hit — 让用户右键 row 任何位置（含 avatar / meta header）都弹菜单
        // 不只是 bubble 本体（sticker 100×100 太精确，用户难命中）
        {
            float _row_h = (prev_same_author ? bub_h : bub_h + 22) + reply_h;
            g_msg_hits.push_back({ { x, y, maxw, _row_h }, idx });
        }
        return (prev_same_author ? bub_h : bub_h + 22) + reply_h + 6;
    }

    // ---------- Text bubble ----------
    // 空 body 直接跳过（避免后端 trim 后空白 + 错误 payload 显示成 28-px 小气泡）
    if (m.body.empty()) {
        return prev_same_author ? 0.0f : 22.0f + 6.0f;
    }
    // 如果文本里有 launcher://pack/<short>，特殊渲染为 pack 分享卡片（缩略图 + 标题 + 行动按钮）。
    auto find_url = [](const std::wstring& s) -> std::wstring {
        // launcher:// 优先
        size_t p = s.find(L"launcher://");
        if (p == std::wstring::npos) {
            p = s.find(L"https://");
            if (p == std::wstring::npos) p = s.find(L"http://");
        }
        if (p == std::wstring::npos) return {};
        size_t e = p;
        while (e < s.size() && s[e] > 0x20 && s[e] != L' ') e++;
        return s.substr(p, e - p);
    };
    std::wstring url = find_url(m.body);
    bool is_pack_link = !url.empty() && url.compare(0, 15, L"launcher://pack/") == 0;
    if (is_pack_link) {
        // 抽 short_name
        std::wstring short_w = url.substr(15);
        std::string short_a;
        for (wchar_t c : short_w) if (c) short_a.push_back((char)c);
        // pack-share 卡片：320×88，左边 64×64 缩略图（cache 后的 cover）+ 标题 + 提示
        float bub_w = 320, bub_h = 88;
        float bub_x = bub_x_for(bub_w);
        // 渐变 / 主色描边
        prim::fillRR(ctx, bub_x, bub_y, bub_w, bub_h, 12.0f, br.solid(pal.card));
        prim::strokeRR(ctx, bub_x, bub_y, bub_w, bub_h, 12.0f,
                       br.solidA(pal.primary, 0.4f), 1.5f);
        // 缩略图占位（如果 g_pack_preview 跟当前 short 匹配则用 cover；否则灰）
        ID2D1Bitmap* thumb = nullptr;
        {
            std::lock_guard<std::mutex> lk(sticker::g_pack_preview_mtx);
            if (sticker::g_pack_preview.short_name == short_a
                && !sticker::g_pack_preview.cover_path.empty()) {
                thumb = app.images().fromFile(sticker::g_pack_preview.cover_path, 64);
            }
        }
        if (thumb) {
            prim::pushLayerRR(ctx, app.factory(), bub_x + 12, bub_y + 12, 64, 64, 8.0f);
            ctx->DrawBitmap(thumb, D2D1::RectF(bub_x + 12, bub_y + 12, bub_x + 76, bub_y + 76),
                            1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            prim::popLayer(ctx);
        } else {
            prim::fillRR(ctx, bub_x + 12, bub_y + 12, 64, 64, 8.0f,
                         br.solidA(pal.primary, 0.18f));
            auto* ic_fmt = app.texts().format(L"Segoe UI Emoji", ptToDip(20.0f));
            prim::drawText_(ctx, L"🎴", ic_fmt,
                            bub_x + 12, bub_y + 16, 64, 56,
                            br.solid(pal.primary),
                            DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        // 标题
        auto* tt_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(10.5f),
                                          DWRITE_FONT_WEIGHT_BOLD);
        auto* sub_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(8.5f));
        prim::drawText_(ctx, trW("pack.share_card_title"), tt_fmt,
                        bub_x + 88, bub_y + 14, bub_w - 100, 20,
                        br.solid(pal.text));
        std::wstring code_disp = L"launcher://pack/" + short_w;
        if (code_disp.size() > 32) code_disp = code_disp.substr(0, 32) + L"…";
        prim::drawText_(ctx, code_disp, sub_fmt,
                        bub_x + 88, bub_y + 36, bub_w - 100, 18,
                        br.solid(pal.text_muted));
        prim::drawText_(ctx, trW("pack.click_to_view"), sub_fmt,
                        bub_x + 88, bub_y + 58, bub_w - 100, 18,
                        br.solid(pal.primary));

        std::string short_copy = short_a;
        // 第一次见到这个 short → 后台拉 cover 进缓存（不用阻塞 paint）
        static std::unordered_map<std::string, bool> g_pack_thumb_kicked;
        if (!g_pack_thumb_kicked[short_copy]) {
            g_pack_thumb_kicked[short_copy] = true;
            sticker::previewPackByShort(GetActiveWindow(), short_copy);
        }
        hit({ bub_x, bub_y, bub_w, bub_h }, [short_copy](){
            auto* payload = new std::string(short_copy);
            PostMessageW(GetActiveWindow(), WM_APP + 49,
                         (WPARAM)payload, 0);
        }, true);
        // 整行 hit — 让用户右键 row 任何位置（含 avatar / meta header）都弹菜单
        // 不只是 bubble 本体（sticker 100×100 太精确，用户难命中）
        {
            float _row_h = (prev_same_author ? bub_h : bub_h + 22) + reply_h;
            g_msg_hits.push_back({ { x, y, maxw, _row_h }, idx });
        }
        return (prev_same_author ? bub_h : bub_h + 22) + reply_h + 6;
    }

    float bub_max_w = (std::min)(maxw * 0.65f, 480.0f);
    float text_w = bub_max_w - 28.0f;
    WrappedText body_layout = wrapTextForWidth(app, m.body, body_fmt, text_w);
    auto* state_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(7.0f));
    bool show_state = me && m.send_state != MsgSendState::Sent;
    std::wstring state_text;
    if (show_state) {
        state_text = (m.send_state == MsgSendState::Pending)
            ? (m.error_text.empty() ? L"sending..." : m.error_text)
            : (m.error_text.empty() ? L"send failed" : m.error_text);
    }
    // 发送状态走气泡下方 caption（固定 14px 行高），不参与气泡宽高计算，
    // 避免短消息发出瞬间气泡撑大、确认后缩回的跳动。
    float state_h = show_state ? 14.0f : 0.0f;
    float content_w = (std::max)((float)std::ceil(body_layout.max_line_w) + 8.0f, 16.0f);
    float bub_w = (std::max)(44.0f, (std::min)(content_w + 28.0f, bub_max_w));
    if (body_layout.max_line_w >= text_w - 1.0f) bub_w = bub_max_w;
    float bub_h = (std::max)(body_layout.text_h + 18.0f, 30.0f);
    float bub_x = bub_x_for(bub_w);

    uint32_t bub_bg = me
        ? (m.send_state == MsgSendState::Failed ? 0xFFE34B4B : pal.primary)
        : pal.card;
    uint32_t bub_fg = me ? 0xFFFFFFFF : pal.text;
    prim::fillRR(ctx, bub_x, bub_y, bub_w, bub_h, 12.0f,
                 m.send_state == MsgSendState::Pending ? br.solidA(bub_bg, 0.72f) : br.solid(bub_bg));
    prim::drawText_(ctx, body_layout.text, body_fmt,
                    bub_x + 12, bub_y + 8, bub_w - 24, body_layout.text_h + 4.0f,
                    br.solid(bub_fg));
    if (show_state) {
        // caption 在气泡正下方右对齐到气泡右缘，单色弱化文字，不改变气泡尺寸。
        uint32_t cap_color = (m.send_state == MsgSendState::Failed)
            ? 0xFFE34B4B : pal.text_muted;
        float cap_alpha = (m.send_state == MsgSendState::Failed) ? 0.95f : 0.75f;
        prim::drawText_(ctx, state_text, state_fmt,
                        bub_x, bub_y + bub_h, bub_w, state_h,
                        br.solidA(cap_color, cap_alpha),
                        DWRITE_TEXT_ALIGNMENT_TRAILING);
    }

    if (!url.empty()) {
        // 链接气泡下加一个小提示行 + hit 整个气泡 → WebView2 打开
        auto* link_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(7.5f),
                                            DWRITE_FONT_WEIGHT_BOLD);
        prim::drawText_(ctx, trW("chat.click_open"), link_fmt,
                        bub_x + 14, bub_y + bub_h - 14, bub_w - 28, 12,
                        br.solidA(me ? 0xFFFFFF : 0xC96442, 0.7f));
        bub_h += 4;
        std::wstring url_copy = url;
        hit({ bub_x, bub_y, bub_w, bub_h }, [url_copy](){
            auto* payload = new std::wstring(url_copy);
            PostMessageW(GetActiveWindow(), WM_APP + 47,
                         (WPARAM)payload, 0);
        }, true);
    }
    // caption 高度计入行高（与 measureBubbleHeight 的 +14 一致），让下一条消息留出空间。
    bub_h += state_h;
    {
        float _row_h = (prev_same_author ? bub_h : bub_h + 22) + reply_h;
        g_msg_hits.push_back({ { x, y, maxw, _row_h }, idx });
    }

    return (prev_same_author ? bub_h : bub_h + 22) + reply_h + 6;
}

// 反应 chip 行 + "已读" 标记 —— 画在 bubble 行底部（reactionRowHeight 预留的带里）。
// 与 measureBubbleHeight 的 reactionRowHeight()/kReadMarkerH 预留保持镜像。
// row_top/row_h 是该消息整行（含 reply + reaction 带 + 可选 read marker）的矩形。
constexpr float kReadMarkerH = 14.0f;
void paintReactionFooter(D2DApp& app, const Msg& m, float x, float row_top,
                         float row_h, float maxw, bool show_read_marker) {
    const Palette& pal = palette();
    auto* ctx = app.ctx();
    auto& br = app.brushes();
    bool me = isSelfMessage(m);
    constexpr float ar = 14.0f, gap = 10.0f;
    // bubble 内容左右边界（跟 paintBubble 的 bub_x_for 对齐）。
    float left_edge  = x + ar * 2 + gap;
    float right_edge = x + maxw - ar * 2 - gap;

    float read_h = show_read_marker ? kReadMarkerH : 0.0f;
    float chip_h = m.reactions.empty() ? 0.0f : kReactionRowH;
    // 行底部：row_top + row_h - 6(gap)。read marker 占最底，chip 带在其上。
    float footer_bottom = row_top + row_h - 6.0f;
    float read_y = footer_bottom - read_h;
    float chip_top = read_y - chip_h;

    if (!m.reactions.empty()) {
        auto* chip_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(9.0f));
        const float pad = 8.0f, chip_gap = 5.0f, ch_h = 20.0f;
        // 先算每个 chip 宽度。
        std::vector<float> cw(m.reactions.size(), 0);
        float total_w = 0;
        for (size_t i = 0; i < m.reactions.size(); ++i) {
            wchar_t cnt[16]; swprintf_s(cnt, L" %d", m.reactions[i].count);
            std::wstring label = m.reactions[i].emoji + cnt;
            cw[i] = measureW(app, label, chip_fmt) + pad * 2.0f;
            total_w += cw[i] + (i ? chip_gap : 0);
        }
        float cx = me ? (right_edge - total_w) : left_edge;
        float cy = chip_top + (chip_h - ch_h) * 0.5f;
        for (size_t i = 0; i < m.reactions.size(); ++i) {
            const Reaction& r = m.reactions[i];
            uint32_t bg = r.mine ? fadeArgb(pal.primary, 0.18f) : fadeArgb(pal.text, 0.06f);
            prim::fillRR(ctx, cx, cy, cw[i], ch_h, 10.0f, br.solid(bg));
            if (r.mine) {
                prim::strokeRR(ctx, cx, cy, cw[i], ch_h, 10.0f,
                               br.solidA(pal.primary, 0.55f), 1.0f);
            }
            wchar_t cnt[16]; swprintf_s(cnt, L" %d", r.count);
            std::wstring label = r.emoji + cnt;
            prim::drawText_(ctx, label, chip_fmt,
                            cx + pad, cy + 2, cw[i] - pad * 2.0f + 2.0f, 16,
                            br.solid(r.mine ? pal.primary : pal.text));
            // 点击 chip 切换：自己已点 → remove；未点 → add。仅对已落库消息（server_id>0）。
            if (m.server_id > 0) {
                int64_t sid = m.server_id;
                std::wstring emoji = r.emoji;
                bool remove = r.mine;
                std::wstring slug = g_active;
                hit({ cx, cy, cw[i], ch_h }, [slug, sid, emoji, remove]() {
                    reactToMessage(GetActiveWindow(), slug, sid, emoji, remove);
                }, true);
            }
            cx += cw[i] + chip_gap;
        }
    }

    if (show_read_marker) {
        auto* rd_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(7.0f));
        prim::drawText_(ctx, trW("chat.read_receipt"), rd_fmt,
                        x, read_y, maxw, read_h,
                        br.solidA(pal.text_muted, 0.8f),
                        DWRITE_TEXT_ALIGNMENT_TRAILING);
    }
}

// ============== Composer ==============
void paintComposer(D2DApp& app, float ax, float ay, float aw, float ah) {
    const Palette& pal = palette();
    auto* ctx = app.ctx();
    auto& br = app.brushes();
    bool writable = canWriteActiveChannel();
    if (!writable) {
        g_focus_composer = false;
    }
    prim::fillRect(ctx, ax, ay, aw, ah, br.solid(pal.bg));
    prim::drawLine(ctx, ax, ay, ax + aw, ay,
                   br.solid(pal.divider), 1.0f);

    float reply_h = g_pending_reply.active ? 22.0f : 0.0f;
    if (g_pending_reply.active) {
        auto* reply_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(8.0f),
                                             DWRITE_FONT_WEIGHT_BOLD);
        auto* reply_body_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(8.0f));
        float rx = ax + 58.0f;
        float ry = ay + 5.0f;
        float rw = aw - 112.0f;
        prim::fillRR(ctx, rx, ry, rw, 18.0f, 6.0f, br.solidA(pal.primary, 0.10f));
        prim::drawText_(ctx, trW("chat.reply_banner"), reply_fmt,
                        rx + 10, ry + 3, 42, 12, br.solid(pal.primary));
        std::wstring preview = g_pending_reply.author + L": " + g_pending_reply.preview;
        preview = fitTextOneLine(app, preview, reply_body_fmt, rw - 82);
        drawTextOneLine(ctx, preview, reply_body_fmt,
                        rx + 54, ry + 3, rw - 82, 12, br.solid(pal.text_muted));
        LayoutRect cancel{ rx + rw - 22, ry, 18, 18 };
        bool ch = cancel.contains(g_mouse);
        if (ch) prim::fillCircle(ctx, cancel.x + 9, cancel.y + 9, 8, br.solidA(pal.text, 0.12f));
        icons::drawIcon(app, icons::Name::X, cancel.x + 4, cancel.y + 4, 10,
                        ch ? pal.text : pal.text_muted);
        hit(cancel, [](){ g_pending_reply = PendingReply{}; }, true);
    }

    const float ico_sz = 30.0f;
    float ix = ax + 14.0f;
    float iy = ay + reply_h + ((ah - reply_h) - ico_sz) * 0.5f;
    LayoutRect emoji_btn{ ix, iy, ico_sz, ico_sz };
    bool ehov = emoji_btn.contains(g_mouse);
    if (ehov) {
        prim::fillRR(ctx, ix, iy, ico_sz, ico_sz, 8.0f,
                     br.solid(pal.card));
    }
    icons::drawIcon(app, icons::Name::Smile, ix + 6, iy + 6, 18,
                    writable && ehov ? pal.text : pal.text_muted);
    g_emoji_button_rect = emoji_btn;
    hit(emoji_btn, [](){
        if (!requireActiveChannelWrite()) return;
        g_focus_composer = true;
        setPickerOpen(!g_picker_open);
    }, true);

    // textarea
    float fx = ix + ico_sz + 10.0f;
    float send_w = 38.0f;
    float fw = aw - (fx - ax) - 14.0f - send_w - 10.0f;
    float fh = ico_sz;
    float fy = iy;
    g_composer.bounds = { fx, fy, fw, fh };

    prim::fillRR(ctx, fx, fy, fw, fh, fh * 0.5f, br.solid(pal.card));
    auto* border = (g_focus_composer && writable) ? br.solid(pal.primary) : br.solid(pal.divider);
    prim::strokeRR(ctx, fx, fy, fw, fh, fh * 0.5f, border,
                   (g_focus_composer && writable) ? 1.4f : 1.0f);
    if (g_focus_composer && writable) {
        prim::strokeRR(ctx, fx - 2, fy - 2, fw + 4, fh + 4, fh * 0.5f + 2,
                       br.solidA(pal.primary, 0.10f), 3.0f);
    }

    auto* tx_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(9.5f));
    const float pad_l = 16.0f;
    const float text_y = fy + (fh - 14.0f) * 0.5f;
    const float text_w = fw - pad_l * 2;
    float caret_w = g_composer.text.empty()
        ? 0.0f
        : caretMeasureW(app, g_composer.displaySlice(0, g_composer.cursor), tx_fmt);
    float text_scroll_x = (std::max)(0.0f, caret_w - text_w + 6.0f);
    g_composer_caret_xs.clear();
    g_composer_caret_xs.reserve(g_composer.text.size() + 1);
    for (int i = 0; i <= (int)g_composer.text.size(); ++i) {
        g_composer_caret_xs.push_back(
            fx + pad_l + caretMeasureW(app, g_composer.displaySlice(0, i), tx_fmt) - text_scroll_x);
    }

    if (g_composer.text.empty()) {
        prim::drawText_(ctx, writable ? trW("chat.composer_placeholder") : activeWriteBlockedMessage(), tx_fmt,
                        fx + pad_l, text_y, text_w, 18,
                        br.solid(pal.text_muted));
    } else {
        ctx->PushAxisAlignedClip(D2D1::RectF(fx + pad_l, fy + 4,
                                             fx + pad_l + text_w, fy + fh - 4),
                                 D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        // 选区
        if (g_focus_composer && writable && g_composer.hasSelection()) {
            float pre_w = caretMeasureW(app,
                g_composer.displaySlice(0, g_composer.selStart()), tx_fmt);
            float in_w = caretMeasureW(app,
                g_composer.displaySlice(g_composer.selStart(), g_composer.selEnd()), tx_fmt);
            prim::fillRect(ctx,
                           fx + pad_l + pre_w - text_scroll_x, text_y - 1, in_w, 18,
                           br.solidA(pal.primary, 0.38f));
        }
        prim::drawText_(ctx, g_composer.text, tx_fmt,
                        fx + pad_l - text_scroll_x, text_y,
                        (std::max)(text_w, caretMeasureW(app, g_composer.text, tx_fmt) + 4.0f), 18,
                        br.solid(pal.text));
        ctx->PopAxisAlignedClip();
    }

    // caret
    if (g_focus_composer && writable && !g_composer.hasSelection()) {
        float pre_w = caret_w - text_scroll_x;
        int phase = (int)(stages::g_time_in_stage * 1000) % 1000;
        if (phase < 500) {
            prim::drawLine(ctx,
                           fx + pad_l + pre_w, fy + 7,
                           fx + pad_l + pre_w, fy + fh - 7,
                           br.solid(pal.primary), 1.5f);
        }
    }

    hit(g_composer.bounds, [](){
        if (!requireActiveChannelWrite()) return;
        g_focus_composer = true;
    }, true);

    // send btn
    float sx = ax + aw - 14 - send_w;
    float sy = iy + (fh - send_w) * 0.5f;
    bool can_send = writable && !g_composer.text.empty();
    LayoutRect send_btn{ sx, sy, send_w, send_w };
    bool sh_ = send_btn.contains(g_mouse);
    uint32_t sbg = !can_send ? fadeArgb(pal.primary, 0.55f)
                            : (sh_ ? pal.primary_hover : pal.primary);
    prim::fillCircle(ctx, sx + send_w * 0.5f, sy + send_w * 0.5f, send_w * 0.5f,
                     br.solid(sbg));
    icons::drawIcon(app, icons::Name::ArrowUp, sx + 10, sy + 10, 18, 0xFFFFFFFF, 2.1f);
    if (can_send) {
        hit(send_btn, []() {
            if (sendTextMessage(GetActiveWindow(), g_composer.text)) {
                g_composer.text.clear();
                g_composer.cursor = 0;
                g_composer.clearSel();
                g_focus_composer = true;
            }
        }, true);
    } else {
        hit(send_btn, []() {
            if (!canWriteActiveChannel()) toast::show(activeWriteBlockedMessage());
        }, true);
    }
}

// ============== Pane (header + stream + composer) ==============
void paintChatPane(D2DApp& app, float ax, float ay, float aw, float ah) {
    const Palette& pal = palette();
    auto* ctx = app.ctx();
    auto& br = app.brushes();
    prim::fillRect(ctx, ax, ay, aw, ah, br.solid(pal.bg));
    g_avatar_hits.clear();   // 帧首清，paintBubble 会填充
    g_msg_hits.clear();      // 帧首清，paintBubble 注册消息体 hit
    g_msg_row_hits.clear();

    // header
    float hdr_h = 56;
    prim::drawLine(ctx, ax, ay + hdr_h, ax + aw, ay + hdr_h,
                   br.solid(pal.divider), 1.0f);

    auto* ch = activeChannel();
    if (!ch) {
        ch = g_channels.empty() ? nullptr : &g_channels.front();
    }
    auto* hash_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(14.0f),
                                        DWRITE_FONT_WEIGHT_BOLD);
    auto* name_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(11.0f),
                                        DWRITE_FONT_WEIGHT_BOLD);
    auto* sub_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(8.5f));
    prim::drawText_(ctx, L"#", hash_fmt,
                    ax + 18, ay + 16, 16, 22, br.solid(pal.text_muted));
    const wchar_t* channel_name = ch ? ch->name : L"general";
    bool is_market = ch && ch->is_market;
    prim::drawText_(ctx, channel_name, name_fmt,
                    ax + 36, ay + 14, 200, 22, br.solid(pal.text));
    prim::drawTextNoWrap(ctx,
                    is_market ? trW("chat.market_desc") : trW("chat.official"),
                    sub_fmt,
                    ax + 36, ay + 32, (std::max)(300.0f, aw - 72.0f), 16, br.solid(pal.text_muted));

    // 右上 search / more 按钮
    // 有模态打开时不注册这两个 header hit —— 否则点右上角关模态会命中残留的
    // search 按钮 hit,导致搜索反复重开、"取消不掉"(hit 每帧在 view 层注册、
    // 模态后画,点击派发会先撞上 view 的按钮)。
    bool header_btns_active = !modal::anyOpen();
    float btn_x = ax + aw - 14 - 34 * 2 - 4;
    for (int i = 0; i < 2; ++i) {
        LayoutRect ar{ btn_x, ay + 11, 34, 34 };
        bool hov = header_btns_active && ar.contains(g_mouse);
        if (hov) {
            prim::fillRR(ctx, ar.x, ar.y, ar.w, ar.h, 8.0f, br.solid(pal.card));
        }
        icons::Name n = (i == 0) ? icons::Name::Search : icons::Name::More;
        icons::drawIcon(app, n, ar.x + 8, ar.y + 8, 18,
                        hov ? pal.text : pal.text_muted);
        if (header_btns_active) {
            if (i == 0) {
                hit(ar, [](){ modal::openSearch(); }, true);
            } else {
                // 三个点:功能菜单(搜索/成员/静音等,视权限)。
                POINT anchor{ (LONG)ar.x, (LONG)(ar.y + ar.h + 4) };
                hit(ar, [anchor](){ modal::openChatMoreMenu(anchor); }, true);
            }
        }
        btn_x += 38;
    }

    // stream
    float comp_h = 64;
    float stream_y = ay + hdr_h;
    float stream_h = ah - hdr_h - comp_h;
    g_chat_stream_rect = { ax, stream_y, aw, stream_h };

    // 用 PushAxisAlignedClip 保证消息溢出不画到 composer 上
    ctx->PushAxisAlignedClip(D2D1::RectF(ax, stream_y, ax + aw, stream_y + stream_h),
                             D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    auto& msgs = streamFor(g_active);
    if (msgs.empty()) {
        auto* empty_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(10.0f));
        prim::drawText_(ctx,
            is_market ? trW("chat.market_redirect") :
                            trW("chat.empty"),
            empty_fmt,
            ax, stream_y + stream_h * 0.5f - 12, aw, 24,
            br.solid(pal.text_muted),
            DWRITE_TEXT_ALIGNMENT_CENTER);
        ctx->PopAxisAlignedClip();
        // 写一下 scroll 状态避免 wheel 事件来时 g_scroll[g_active] 不存在
        auto& sc = g_scroll[g_active];
        sc.total_height = 0;
        sc.viewport_h = stream_h;
        sc.offset_from_bottom = 0;
        sc.target_offset = 0;
        sc.rendered_count = 0;
        sc.tail_server_id = 0;
        sc.tail_client_msg_id.clear();
        sc.initialized = true;
        // composer
        paintComposer(app, ax, ay + ah - comp_h, aw, comp_h);
        return;
    }
    float maxw = aw - 32;
    // ----- Pass 1：dry-run 测每条 bubble 高度 + 算 total -----
    for (auto& m : msgs) {
        normalizeMsgIdentity(m);
        fillReplySnapshot(m);
        rememberReplySnapshot(g_active, m);
    }
    std::vector<float> heights(msgs.size(), 0);
    float total = kStreamTopPad + kStreamBottomPad;
    // 读回执标记：找"自己最后一条已被 peer 读过的消息"— 只在这一条下画一个 Read 标记。
    int read_marker_idx = -1;
    for (size_t i = msgs.size(); i-- > 0; ) {
        const Msg& m = msgs[i];
        if (isSelfMessage(m) && m.server_id > 0 && messageReadByPeer(g_active, m.server_id)) {
            read_marker_idx = (int)i;
            break;
        }
    }
    for (size_t i = 0; i < msgs.size(); ++i) {
        const Msg& m = msgs[i];
        const Msg* prev = (i > 0) ? &msgs[i - 1] : nullptr;
        bool prev_same = prev && sameGroupedAuthor(*prev, m);
        heights[i] = measureBubbleHeight(app, m, maxw, prev_same);
        if ((int)i == read_marker_idx) heights[i] += kReadMarkerH;   // 预留 Read 标记行
        total += heights[i];
    }
    // ----- 滚动状态 -----
    auto& sc = g_scroll[g_active];
    bool was_at_bottom = (sc.offset_from_bottom < 16.0f && sc.target_offset < 16.0f);
    bool tail_changed = false;
    int64_t tail_server_id = 0;
    std::string tail_client_msg_id;
    if (!msgs.empty()) {
        tail_server_id = msgs.back().server_id;
        tail_client_msg_id = msgs.back().client_msg_id;
    }
    if (sc.initialized) {
        tail_changed = sc.tail_server_id != tail_server_id
            || sc.tail_client_msg_id != tail_client_msg_id;
    }
    sc.total_height = total;
    sc.viewport_h = stream_h;
    if (!sc.initialized) {
        sc.offset_from_bottom = 0;
        sc.target_offset = 0;
        sc.initialized = true;
    } else if (tail_changed && was_at_bottom && !g_scroll_drag.active) {
        sc.offset_from_bottom = 0;
        sc.target_offset = 0;
    }
    sc.rendered_count = msgs.size();
    sc.tail_server_id = tail_server_id;
    sc.tail_client_msg_id = tail_client_msg_id;
    // 已读回执：贴底浏览时把 tail 标记为已读。markRead 内部按 slug 单调去抖，
    // 每帧调用无害（tail 不前进就直接返回）；后端 GREATEST 也容忍重复。
    if (was_at_bottom && !g_scroll_drag.active && tail_server_id > 0) {
        markRead(GetActiveWindow(), g_active, tail_server_id);
    }
    float max_off = (std::max)(0.0f, total - stream_h);
    if (sc.target_offset > max_off) sc.target_offset = max_off;
    if (sc.target_offset < 0) sc.target_offset = 0;
    if (g_focus_target.server_id > 0
        && !g_focus_target.scroll_applied
        && g_focus_target.slug == g_active) {
        float before = 0.0f;
        for (size_t i = 0; i < msgs.size(); ++i) {
            if (msgs[i].server_id == g_focus_target.server_id) {
                float target_center_from_top = before + heights[i] * 0.5f;
                float target = max_off - target_center_from_top + stream_h * 0.5f;
                if (target > max_off) target = max_off;
                if (target < 0) target = 0;
                sc.target_offset = target;
                sc.offset_from_bottom = target;
                g_focus_target.scroll_applied = true;
                break;
            }
            before += heights[i];
        }
        if (!g_focus_target.scroll_applied) {
            auto state = g_history_state[g_active];
            if (state == HistoryLoadState::Loaded || state == HistoryLoadState::Failed) {
                fetchHistory(GetActiveWindow(), g_active);
            } else if (state == HistoryLoadState::Exhausted) {
                g_focus_target.missing_reported = true;
            }
        }
    }
    // 拖动滚动条期间直接同步；否则平滑 lerp 到 target（每帧 18% 趋近 — 连贯但不软）
    if (g_scroll_drag.active && g_mouse_pressed) {
        // 鼠标 y 增加 = 滚动条下移 = offset 减少（更接近底部）
        float dy = (float)g_mouse.y - g_scroll_drag.anchor_mouse_y;
        float track_h = (std::max)(1.0f, g_scroll_drag.bar_track_h);
        float content_per_track = (g_scroll_drag.total_height - g_scroll_drag.viewport_h) / track_h;
        // bar 下移（dy>0）→ offset 减少（向更新消息靠近）
        float new_off = g_scroll_drag.anchor_offset - dy * content_per_track;
        if (new_off > max_off) new_off = max_off;
        if (new_off < 0) new_off = 0;
        sc.target_offset = new_off;
        sc.offset_from_bottom = new_off;
    } else {
        float diff = sc.target_offset - sc.offset_from_bottom;
        if (std::abs(diff) < 0.5f) sc.offset_from_bottom = sc.target_offset;
        else                       sc.offset_from_bottom += diff * 0.22f;
    }
    if (sc.offset_from_bottom > max_off) sc.offset_from_bottom = max_off;
    if (sc.offset_from_bottom < 0) sc.offset_from_bottom = 0;

    float my_top;
    if (total <= stream_h) {
        // 消息没把 viewport 填满 — 顶端开始，不滚
        my_top = stream_y + kStreamTopPad;
    } else {
        // total > viewport：offset_from_bottom 表示从底部往上滚了多少 px
        // offset = 0 → 锁底（最新消息在底部）→ my_top = stream_bottom - total
        // offset = max_off → 顶部（最早消息在顶部）→ my_top = stream_y
        my_top = stream_y + stream_h - total + sc.offset_from_bottom + kStreamTopPad;
    }

    // ----- Pass 2：实际画 + 注册 hit -----
    float my = my_top;
    for (size_t i = 0; i < msgs.size(); ++i) {
        const Msg& m = msgs[i];
        const Msg* prev = (i > 0) ? &msgs[i - 1] : nullptr;
        bool prev_same = prev && sameGroupedAuthor(*prev, m);
        // 跳过完全在 viewport 之外的 bubble — 既省 D2D 也避免 hit 冲突
        if (my + heights[i] < stream_y || my > stream_y + stream_h) {
            my += heights[i];
            continue;
        }
        if (g_focus_target.server_id > 0
            && g_focus_target.slug == g_active
            && msgs[i].server_id == g_focus_target.server_id
            && stages::g_time_in_stage < g_focus_target.highlight_until) {
            float remain = g_focus_target.highlight_until - stages::g_time_in_stage;
            float alpha = (std::min)(0.18f, 0.08f + remain * 0.05f);
            prim::fillRR(ctx, ax + 10, my - 2, aw - 20, heights[i], 8.0f,
                         br.solidA(pal.primary, alpha));
        }
        g_msg_row_hits.push_back({ { ax, my, aw, heights[i] }, (int)i });
        paintBubble(app, m, (int)i, ax + 16, my, maxw, prev_same);
        // 反应 chips + (仅 read_marker_idx 那条) 已读标记，画在该行底部预留带里。
        if (!m.reactions.empty() || (int)i == read_marker_idx) {
            paintReactionFooter(app, m, ax + 16, my, heights[i], maxw,
                                /*show_read_marker=*/(int)i == read_marker_idx);
        }
        my += heights[i];
    }
    ctx->PopAxisAlignedClip();

    // 右侧滚动条 — 可拖动
    if (total > stream_h) {
        float bar_x = ax + aw - 8;
        float bar_w = 6;     // 加宽 4→6 让拖动更好命中
        float bar_track_y = stream_y + 4;
        float bar_track_h = stream_h - 8;
        float bar_h = (stream_h / total) * bar_track_h;
        if (bar_h < 28) bar_h = 28;
        float t_pos = (max_off > 0) ? (sc.offset_from_bottom / max_off) : 0;
        float bar_y = bar_track_y + (bar_track_h - bar_h) * (1.0f - t_pos);
        // track
        prim::fillRR(ctx, bar_x, bar_track_y, bar_w, bar_track_h, 3.0f,
                     br.solidA(pal.text, 0.05f));
        // thumb — hover/拖动时颜色加深
        LayoutRect bar_rect{ bar_x - 2, bar_y, bar_w + 4, bar_h };
        bool bar_hov = bar_rect.contains(g_mouse) || g_scroll_drag.active;
        prim::fillRR(ctx, bar_x, bar_y, bar_w, bar_h, 3.0f,
                     br.solidA(pal.text, bar_hov ? 0.50f : 0.30f));
        // 注册 thumb 拖动 hit
        float anchor_y = (float)g_mouse.y;
        float anchor_off = sc.offset_from_bottom;
        float track_h_capt = bar_track_h - bar_h;
        float total_capt = total;
        float vp_capt = stream_h;
        hit(bar_rect, [anchor_y, anchor_off, track_h_capt, total_capt, vp_capt](){
            g_scroll_drag.active = true;
            g_scroll_drag.anchor_mouse_y = anchor_y;
            g_scroll_drag.anchor_offset = anchor_off;
            g_scroll_drag.bar_track_h = track_h_capt;
            g_scroll_drag.total_height = total_capt;
            g_scroll_drag.viewport_h = vp_capt;
        }, true);
    }

    // composer
    paintComposer(app, ax, ay + ah - comp_h, aw, comp_h);
}

// ============== Picker ==============
// 200+ 常用 emoji — 不分类，按 group 排（Segoe UI Emoji 都能渲染）。picker 区域加滚动。
const wchar_t* kEmoji[] = {
    // 笑脸
    L"😀",L"😃",L"😄",L"😁",L"😆",L"😅",L"🤣",L"😂",L"🙂",L"🙃",
    L"😉",L"😊",L"😇",L"🥰",L"😍",L"🤩",L"😘",L"😗",L"😚",L"😙",
    L"😋",L"😛",L"😜",L"🤪",L"😝",L"🤑",L"🤗",L"🤭",L"🤫",L"🤔",
    L"🤐",L"🤨",L"😐",L"😑",L"😶",L"😏",L"😒",L"🙄",L"😬",L"🤥",
    L"😌",L"😔",L"😪",L"🤤",L"😴",L"😷",L"🤒",L"🤕",L"🤢",L"🤮",
    // 情绪
    L"🥳",L"😎",L"🤓",L"🧐",L"😕",L"😟",L"🙁",L"😮",L"😯",L"😲",
    L"😳",L"🥺",L"😦",L"😧",L"😨",L"😰",L"😥",L"😢",L"😭",L"😱",
    L"😖",L"😣",L"😞",L"😓",L"😩",L"😫",L"🥱",L"😤",L"😡",L"😠",
    L"🤬",L"😈",L"👿",L"💀",L"💩",L"🤡",L"👹",L"👺",L"👻",L"👽",
    L"👾",L"🤖",
    // 手势
    L"👍",L"👎",L"👊",L"✊",L"🤛",L"🤜",L"👏",L"🙌",L"👐",L"🤲",
    L"🤝",L"🙏",L"✌",L"🤞",L"🤟",L"🤘",L"🤙",L"👌",L"👈",L"👉",
    L"👆",L"👇",L"☝",L"✋",L"🤚",L"🖐",L"🖖",L"👋",L"💪",L"🦾",
    // 心
    L"❤",L"🧡",L"💛",L"💚",L"💙",L"💜",L"🖤",L"🤍",L"🤎",L"💔",
    L"❣",L"💕",L"💞",L"💓",L"💗",L"💖",L"💘",L"💝",L"💟",
    // 动作 / 标记
    L"💯",L"💢",L"💥",L"💫",L"💦",L"💨",L"💣",L"💬",L"💭",L"💤",
    L"🔥",L"🌟",L"⭐",L"✨",L"⚡",L"🌈",L"☀",L"🌙",L"☁",L"❄",
    // 物品 / 食物
    L"🎉",L"🎊",L"🎁",L"🎂",L"🍰",L"🍕",L"🍔",L"🍟",L"🌭",L"🍿",
    L"🍣",L"🍱",L"🍜",L"🍙",L"🍩",L"🍪",L"🍫",L"🍬",L"🍭",L"🍮",
    L"🥤",L"🍻",L"🍺",L"🍷",L"🍸",L"☕",L"🍵",L"🥛",
    // 动物
    L"🐶",L"🐱",L"🐭",L"🐹",L"🐰",L"🦊",L"🐻",L"🐼",L"🐨",L"🐯",
    L"🦁",L"🐮",L"🐷",L"🐸",L"🐵",L"🙈",L"🙉",L"🙊",L"🐒",L"🐔",
    L"🐧",L"🐤",L"🦆",L"🦅",L"🦉",L"🐺",L"🐗",
    // 游戏 / 运动
    L"🎮",L"🕹",L"🎯",L"🎲",L"🎴",L"♟",L"🎳",L"🎱",L"⚽",L"🏀",
    L"🏈",L"⚾",L"🎾",L"🏐",L"🏉",L"🚀",L"💎",L"🎵",L"🎶",L"🌸",
    L"🌹",L"🌺",L"🌻",L"🌷",L"🌴",L"🍀",
};

void paintPicker(D2DApp& app, float anchor_x, float anchor_y) {
    if (!g_picker_open && g_picker_t.value() < 0.001f) return;
    float t = g_picker_t.value();
    if (t < 0.001f) return;

    const Palette& pal = palette();
    auto* ctx = app.ctx();
    auto& br = app.brushes();

    // 480(原 330):左侧 表情/表情包 标签(右缘 ~px+160)与右侧 4 个按钮
    //(导入文件/导入/导出/新建,约 247 宽)在窄面板下重叠;480 宽下按钮组左缘
    // 约 px+219,离标签 px+160 有 ~59px 余量。
    float pw = 480, ph = 340;
    float px = anchor_x;
    float py = anchor_y - ph - 8 + (1.0f - t) * 14.0f;
    g_picker_origin_x = px; g_picker_origin_y = py;
    g_picker_rect = { px, py, pw, ph };
    g_picker_content_rect = { px + 14, py + 50, pw - 28, ph - 62 };
    hit(g_picker_rect, [](){}, false);

    prim::drawShadow(ctx, br, px, py, pw, ph, 12.0f,
                     pal.shadow_card_hover, 0.28f * t, 2.0f, 2);
    prim::fillRR(ctx, px, py, pw, ph, 12.0f, br.solidA(pal.card, t));
    prim::strokeRR(ctx, px, py, pw, ph, 12.0f, br.solidA(pal.divider, t));

    auto* tab_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(9.5f),
                                       DWRITE_FONT_WEIGHT_BOLD);
    auto* hint_fmt = app.texts().format(L"Microsoft YaHei UI", ptToDip(8.0f));

    // ===== 顶部 seg：[表情] [表情包]，右边 [↥导入] [⇣导出] [+新建] =====
    float seg_y = py + 12;
    LayoutRect tab_em{ px + 14, seg_y, 60, 26 };
    LayoutRect tab_pk{ px + 14 + 60 + 6, seg_y, 80, 26 };
    bool em_act = (g_picker_tab == 0);
    // 滑块 — 跟随 active 动画
    float pill_x = px + 14 + g_top_seg_x.value();
    float pill_w = g_top_seg_w.started ? g_top_seg_w.value() : (em_act ? 60.0f : 80.0f);
    if (pill_w > 0)
        prim::fillRR(ctx, pill_x, seg_y, pill_w, 26, 6.0f,
                     br.solidA(pal.primary, 0.18f * t));
    prim::drawText_(ctx, trW("picker.emoji"), tab_fmt,
                    tab_em.x, tab_em.y + 5, tab_em.w, 18,
                    br.solidA(em_act ? pal.primary : pal.text_muted, t),
                    DWRITE_TEXT_ALIGNMENT_CENTER);
    hit(tab_em, [](){
        setPickerTabSmooth(0);
    }, true);
    prim::drawText_(ctx, trW("picker.packs"), tab_fmt,
                    tab_pk.x, tab_pk.y + 5, tab_pk.w, 18,
                    br.solidA(g_picker_tab > 0 ? pal.primary : pal.text_muted, t),
                    DWRITE_TEXT_ALIGNMENT_CENTER);
    hit(tab_pk, [](){
        setPickerTabSmooth(g_picker_tab == 0 ? 1 : g_picker_tab);
    }, true);

    // 决定当前 pack 状态（用于按钮 enable / 操作目标）
    auto& packs = sticker::g_packs;
    int active_pack = -1;     // -1 = 在 emoji tab 或没 pack
    if (g_picker_tab > 0) {
        active_pack = g_picker_tab - 1;
        if (active_pack >= (int)packs.size()) active_pack = 0;
    }
    std::string cur_pid;
    if (active_pack >= 0 && active_pack < (int)packs.size()) {
        cur_pid = packs[active_pack].id;
    }

    // ===== 右上 3 按钮 =====
    auto draw_btn = [&](float bx, float by, float bw, float bh,
                        std::wstring_view label, uint32_t color, bool primary,
                        std::function<void()> on_click) {
        LayoutRect r{ bx, by, bw, bh };
        bool hov = r.contains(g_mouse);
        if (primary) {
            prim::fillRR(ctx, bx, by, bw, bh, 6.0f,
                         br.solidA(color, t * (hov ? 1.0f : 0.85f)));
        } else {
            prim::fillRR(ctx, bx, by, bw, bh, 6.0f,
                         br.solidA(color, t * (hov ? 0.18f : 0.08f)));
        }
        prim::drawTextNoWrap(ctx, label, hint_fmt,
                        bx, by + 5, bw, 16,
                        br.solidA(primary ? 0xFFFFFF : color, t),
                        DWRITE_TEXT_ALIGNMENT_CENTER);
        hit(r, std::move(on_click), true);
    };
    // 按钮宽度按文案实测（日文「エクスポート」比中「导出」宽得多），最小 48。
    std::wstring imp_lbl = trW("picker.import_short");
    std::wstring exp_lbl = trW("picker.export_short");
    std::wstring new_lbl = trW("picker.new_short");
    std::wstring files_lbl = trW("sticker.import_files_short");
    auto pbtn_w = [&](const std::wstring& lbl) {
        return (std::max)(48.0f, std::ceil(measureW(app, lbl, hint_fmt)) + 18.0f);
    };
    float bw_new = pbtn_w(new_lbl), bw_imp = pbtn_w(imp_lbl), bw_exp = pbtn_w(exp_lbl);
    float bw_files = pbtn_w(files_lbl);
    float bgap = 5;
    float right_btn_y = seg_y;
    float bx_new = px + pw - 14 - bw_new;
    float bx_exp = bx_new - bgap - bw_exp;
    float bx_imp = bx_exp - bgap - bw_imp;
    float bx_files = bx_imp - bgap - bw_files;
    // [⁝⁝ 导入文件]（多选文件，紧邻文件夹导入左侧）
    draw_btn(bx_files, right_btn_y, bw_files, 26, files_lbl, pal.text, false, [cur_pid](){
        if (cur_pid.empty()) {
            PostMessageW(GetActiveWindow(), WM_APP + 41, 0, 0);
        } else {
            auto* payload = new std::string(cur_pid);
            PostMessageW(GetActiveWindow(), WM_APP + 67,
                         (WPARAM)payload, 0);
        }
    });
    // [↥ 导入]
    draw_btn(bx_imp, right_btn_y, bw_imp, 26, imp_lbl, pal.text, false, [cur_pid](){
        if (cur_pid.empty()) {
            PostMessageW(GetActiveWindow(), WM_APP + 41, 0, 0);
        } else {
            auto* payload = new std::string(cur_pid);
            PostMessageW(GetActiveWindow(), WM_APP + 34,
                         (WPARAM)payload, 0);
        }
    });
    // [⇣ 导出]
    draw_btn(bx_exp, right_btn_y, bw_exp, 26, exp_lbl, pal.text, false, [cur_pid](){
        if (cur_pid.empty()) {
            PostMessageW(GetActiveWindow(), WM_APP + 41, 0, 0);
        } else {
            sticker::exportPackToFolder(GetActiveWindow(), cur_pid);
        }
    });
    // [+ 新建]
    draw_btn(bx_new, right_btn_y, bw_new, 26, new_lbl, pal.primary, true, [](){
        setPickerOpen(false);
        PostMessageW(GetActiveWindow(), WM_APP + 21, 0, 0);
    });

    float content_t = g_picker_content_t.started ? g_picker_content_t.value() : 1.0f;
    float open_content_t = clampf((t - 0.22f) / 0.78f, 0.0f, 1.0f);
    float ct = t * content_t * open_content_t;
    float content_y = (1.0f - content_t) * 8.0f;

    if (g_picker_tab == 0) {
        // ===== 8 列 emoji grid + 垂直滚动 =====
        int cols = 8;
        int total_n = (int)(sizeof(kEmoji) / sizeof(kEmoji[0]));
        if ((int)g_emoji_hover_t.size() != total_n) {
            g_emoji_hover_t.assign(total_n, 0.0f);
        }
        float cell = 37.0f;
        float grid_x = px + 14, grid_y = py + 50 + content_y;
        // viewport：emoji 区高度 = picker 底部 - grid_y - 12 边距
        float view_h = (py + ph - 12) - grid_y;
        int rows_total = (total_n + cols - 1) / cols;
        float total_h = rows_total * cell;
        g_emoji_grid_h_last = view_h;
        g_emoji_total_h_last = total_h;
        // 钳 scroll
        float max_scroll = (std::max)(0.0f, total_h - view_h);
        if (g_picker_scroll_drag.active && g_picker_scroll_drag.mode == 0 && g_mouse_pressed) {
            float track_move = (std::max)(1.0f, g_picker_scroll_drag.track_h - g_picker_scroll_drag.thumb_h);
            float dy = (float)g_mouse.y - g_picker_scroll_drag.anchor_mouse_y;
            g_emoji_scroll_y = g_picker_scroll_drag.anchor_scroll_y
                + (dy / track_move) * g_picker_scroll_drag.max_scroll;
        } else if (!g_mouse_pressed) {
            g_picker_scroll_drag.active = false;
        }
        g_emoji_scroll_y = clampf(g_emoji_scroll_y, 0.0f, max_scroll);

        // clip 到 emoji 区
        ctx->PushAxisAlignedClip(D2D1::RectF(grid_x, grid_y, grid_x + cols * cell, grid_y + view_h),
                                 D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        auto* em_fmt = app.texts().format(L"Segoe UI Emoji", ptToDip(16.0f));
        int first_row = (std::max)(0, (int)(g_emoji_scroll_y / cell) - 1);
        int last_row = (std::min)(rows_total - 1, (int)((g_emoji_scroll_y + view_h) / cell) + 1);
        for (int i = first_row * cols; i < total_n && i < (last_row + 1) * cols; ++i) {
            int row = i / cols, col = i % cols;
            float ex = grid_x + col * cell;
            float ey = grid_y + row * cell - g_emoji_scroll_y;
            // 完全不可见的跳过
            if (ey + cell < grid_y) continue;
            if (ey > grid_y + view_h) break;
            LayoutRect cell_r{ ex, ey, cell, cell };
            bool hov = cell_r.contains(g_mouse);
            float& ht = g_emoji_hover_t[i];
            ht += ((hov ? 1.0f : 0.0f) - ht) * 0.24f;
            if (ht > 0.01f) {
                float inset = 3.0f - ht;
                prim::fillRR(ctx, ex + inset, ey + inset,
                             cell - inset * 2.0f, cell - inset * 2.0f,
                             7.0f, br.solidA(pal.primary, ct * (0.06f + 0.10f * ht)));
                prim::strokeRR(ctx, ex + inset, ey + inset,
                               cell - inset * 2.0f, cell - inset * 2.0f,
                               7.0f, br.solidA(pal.primary, ct * 0.20f * ht), 1.0f);
            }
            float lift = ht * 2.0f;
            float grow = ht * 2.0f;
            // emoji 字形栅格化进缓存位图后 DrawBitmap 复用（彩色字形每帧重栅格化是卡顿根因）。
            // 缓存位图是 cell×cell、字形居中；hover 时目标矩形对称放大 + 上移，无需重栅格化。
            const wchar_t* e = kEmoji[i];
            float gx = ex - grow * 0.5f;
            float gy = ey - grow * 0.5f - lift;
            float gw = cell + grow;
            float gh = cell + grow;
            ID2D1Bitmap* ebmp = app.emojis().get(e, cell, ptToDip(16.0f));
            if (ebmp) {
                ctx->DrawBitmap(ebmp, D2D1::RectF(gx, gy, gx + gw, gy + gh),
                                ct, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            } else {
                prim::drawText_(ctx, e, em_fmt,
                                ex - grow * 0.5f, ey + 4 - lift, cell + grow, cell - 4 + grow,
                                br.solidA(pal.text, ct),
                                DWRITE_TEXT_ALIGNMENT_CENTER);
            }
            hit(cell_r, [e](){
                std::wstring s = e;
                if (g_react_target.active) {
                    // React 模式：把选中的 emoji 发到目标消息（remove=false 表示加反应）。
                    reactToMessage(GetActiveWindow(), g_react_target.slug,
                                   g_react_target.server_id, s, /*remove=*/false);
                    g_react_target.active = false;
                    setPickerOpen(false);
                } else {
                    g_composer.replaceSelection(s);
                    g_focus_composer = true;
                    // 不自动关 picker — 用户可能要连续选
                }
            }, true);
        }
        ctx->PopAxisAlignedClip();
        // 滚动条
        if (max_scroll > 0) {
            float bar_x = grid_x + cols * cell + 2;
            float bar_w = 6;
            float bar_h_p = (std::max)(28.0f, (view_h / total_h) * view_h);
            float bar_top = grid_y + (view_h - bar_h_p) * (g_emoji_scroll_y / max_scroll);
            LayoutRect thumb{ bar_x - 4, bar_top, bar_w + 8, bar_h_p };
            bool bar_hov = thumb.contains(g_mouse) || g_picker_scroll_drag.active;
            prim::fillRR(ctx, bar_x, grid_y, bar_w, view_h, 2.0f,
                         br.solidA(pal.text, ct * 0.05f));
            prim::fillRR(ctx, bar_x, bar_top, bar_w, bar_h_p, 2.0f,
                         br.solidA(pal.text, ct * (bar_hov ? 0.50f : 0.30f)));
            float anchor_y = (float)g_mouse.y;
            float anchor_scroll = g_emoji_scroll_y;
            hit(thumb, [anchor_y, anchor_scroll, view_h, bar_h_p, max_scroll](){
                g_picker_scroll_drag.active = true;
                g_picker_scroll_drag.mode = 0;
                g_picker_scroll_drag.anchor_mouse_y = anchor_y;
                g_picker_scroll_drag.anchor_scroll_y = anchor_scroll;
                g_picker_scroll_drag.track_h = view_h;
                g_picker_scroll_drag.thumb_h = bar_h_p;
                g_picker_scroll_drag.max_scroll = max_scroll;
            }, true);
        }
    } else {
        // ===== 表情包面板 =====
        // pack 顶部 tab 行 — 横向滑动 + 拖拽排序
        float tab_y = py + 50 + content_y;
        g_pack_tab_rects.clear();
        // pre-layout: 算每个 tab 的宽度
        std::vector<float> ws(packs.size(), 0);
        for (size_t i = 0; i < packs.size(); ++i) {
            const auto& p = packs[i];
            wchar_t buf[40]; swprintf_s(buf, L"%.10ls", sticker::packDisplayName(p).c_str());
            float bw = measureW(app, buf, hint_fmt) + 16.0f;
            if (bw > 100.0f) bw = 100.0f;
            if (bw < 40.0f) bw = 40.0f;
            ws[i] = bw;
        }
        float gap_tab = 4.0f;
        std::vector<float> xs(packs.size(), 0);
        float bx = px + 14;
        for (size_t i = 0; i < packs.size(); ++i) {
            xs[i] = bx;
            bx += ws[i] + gap_tab;
        }
        // 在 g_pack_tab_rects 缓存所有 tab 位置（拖拽 / 命中）
        for (size_t i = 0; i < packs.size(); ++i) {
            g_pack_tab_rects.push_back({ {xs[i], tab_y, ws[i], 24}, (int)i });
        }
        retargetPackTab();
        // ---- 拖拽检测 + 实时重排 ----
        if (g_pack_drag.from >= 0 && g_mouse_pressed) {
            float dx = (float)g_mouse.x - g_pack_drag.start_x;
            if (!g_pack_drag.moved && std::abs(dx) > 6.0f) g_pack_drag.moved = true;
            if (g_pack_drag.moved) {
                // 找到鼠标所处的目标位置
                int target = g_pack_drag.from;
                for (size_t i = 0; i < packs.size(); ++i) {
                    if (g_mouse.x >= xs[i] && g_mouse.x <= xs[i] + ws[i]) {
                        target = (int)i;
                        break;
                    }
                }
                // 如果目标变了 → 立即在 g_packs 里 swap (本地立即响应；松手后云端保存)
                if (target != g_pack_drag.from
                    && target >= 0 && target < (int)packs.size()) {
                    std::lock_guard<std::mutex> lk(sticker::g_packs_mtx);
                    auto& v = sticker::g_packs;
                    if (g_pack_drag.from < (int)v.size() && target < (int)v.size()) {
                        sticker::Pack moving = std::move(v[g_pack_drag.from]);
                        v.erase(v.begin() + g_pack_drag.from);
                        v.insert(v.begin() + target, std::move(moving));
                        g_pack_drag.from = target;
                        g_picker_tab = 1 + target;
                        retargetPackTab();
                    }
                }
            }
        }
        // 滑块（active pill）— 用 tween 平滑
        if (!g_pack_tab_x.started && active_pack >= 0 && active_pack < (int)packs.size()) {
            g_pack_tab_x.start(xs[active_pack], xs[active_pack], 0.001f, 0, curve::easeOutQuint);
            g_pack_tab_w.start(ws[active_pack], ws[active_pack], 0.001f, 0, curve::easeOutQuint);
        }
        if (g_pack_tab_w.value() > 0.5f) {
            prim::fillRR(ctx, g_pack_tab_x.value(), tab_y,
                         g_pack_tab_w.value(), 24, 4.0f,
                         br.solidA(pal.primary, ct * 0.18f));
        }
        // 画每个 tab
        for (size_t i = 0; i < packs.size(); ++i) {
            // 拖拽中：源 tab 跟随鼠标
            float draw_x = xs[i];
            if (g_pack_drag.from == (int)i && g_pack_drag.moved) {
                draw_x = g_mouse.x - g_pack_drag.anchor_dx;
                // 不画背景 — 用纯文字 + 半透明高亮
                prim::fillRR(ctx, draw_x, tab_y, ws[i], 24, 4.0f,
                             br.solidA(pal.primary, ct * 0.30f));
            }
            wchar_t buf[40]; swprintf_s(buf, L"%.10ls", sticker::packDisplayName(packs[i]).c_str());
            bool pa = ((int)i == active_pack);
            uint32_t tcol = pa ? pal.primary : pal.text_muted;
            prim::drawText_(ctx, buf, hint_fmt,
                            draw_x + 4, tab_y + 5, ws[i] - 8, 16,
                            br.solidA(tcol, ct),
                            DWRITE_TEXT_ALIGNMENT_CENTER);
            int idx = (int)i;
            // 注意 hit 的是 tab 实际位置（拖动时这个 hit 跟着移）— 让点击到拖到位置上
            LayoutRect r{ draw_x, tab_y, ws[i], 24 };
            float anchor_dx = g_mouse.x - xs[i];
            hit(r, [idx, anchor_dx](){
                // 如果不是拖动结束的 click（左键单击）就切 active
                if (g_pack_drag.from == idx && g_pack_drag.moved) return;
                setPickerTabSmooth(1 + idx);
            }, true);
        }

        // ===== 当前 pack 内容 =====
        bool has_active_pack = active_pack >= 0 && active_pack < (int)packs.size();
        if (!has_active_pack) {
            g_pack_grid_h_last = 1.0f;
            g_pack_total_h_last = 0.0f;
            prim::drawText_(ctx, trW("picker.no_packs"),
                            hint_fmt, px + 14, py + 130, pw - 28, 18,
                            br.solidA(pal.text_muted, ct),
                            DWRITE_TEXT_ALIGNMENT_CENTER);
        } else {
            const auto& cur_pack = packs[active_pack];
            // 创建人小标
            if (!cur_pack.creator_name.empty() || !cur_pack.is_owner) {
                std::wstring tip;
                if (cur_pack.is_owner) tip = trW("pack.created_by_me");
                else if (!cur_pack.creator_name.empty()) tip = trW("pack.by_prefix") + cur_pack.creator_name;
                else tip = trW("pack.installed");
                prim::drawText_(ctx, tip, hint_fmt,
                                px + 14, py + 78, pw - 28, 14,
                                br.solidA(pal.text_faint, ct));
            }
            if (cur_pack.stickers.empty()) {
                g_pack_grid_h_last = 1.0f;
                g_pack_total_h_last = 0.0f;
                prim::drawText_(ctx,
                    cur_pack.is_system
                        ? trW("picker.switch_to_emoji") : trW("picker.empty"),
                    hint_fmt,
                    px + 14, py + 130, pw - 28, 18,
                    br.solidA(pal.text_muted, ct),
                    DWRITE_TEXT_ALIGNMENT_CENTER);
            } else {
                int cols = 5;
                float cell = 56.0f;
                float gap = 5.0f;
                float gx = px + 14, gy = py + 96 + content_y;
                float view_h = (py + ph - 44.0f) - gy;
                int rows_total = ((int)cur_pack.stickers.size() + cols - 1) / cols;
                float row_h = cell + gap;
                float total_h = rows_total * row_h;
                g_pack_grid_h_last = view_h;
                g_pack_total_h_last = total_h;
                float max_scroll = (std::max)(0.0f, total_h - view_h);
                if (g_picker_scroll_drag.active && g_picker_scroll_drag.mode == 1 && g_mouse_pressed) {
                    float track_move = (std::max)(1.0f, g_picker_scroll_drag.track_h - g_picker_scroll_drag.thumb_h);
                    float dy = (float)g_mouse.y - g_picker_scroll_drag.anchor_mouse_y;
                    g_pack_scroll_y = g_picker_scroll_drag.anchor_scroll_y
                        + (dy / track_move) * g_picker_scroll_drag.max_scroll;
                } else if (!g_mouse_pressed) {
                    g_picker_scroll_drag.active = false;
                }
                g_pack_scroll_y = clampf(g_pack_scroll_y, 0.0f, max_scroll);

                ctx->PushAxisAlignedClip(D2D1::RectF(gx, gy, gx + cols * (cell + gap), gy + view_h),
                                         D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
                int first_row = (std::max)(0, (int)(g_pack_scroll_y / row_h) - 1);
                int last_row = (std::min)(rows_total - 1, (int)((g_pack_scroll_y + view_h) / row_h) + 1);
                bool decode_bitmaps = ct > 0.45f;
                for (size_t i = (size_t)(first_row * cols);
                     i < cur_pack.stickers.size() && i < (size_t)((last_row + 1) * cols); ++i) {
                    int row = (int)(i / cols), col = (int)(i % cols);
                    float ex = gx + col * (cell + gap), ey = gy + row * row_h - g_pack_scroll_y;
                    if (ey + cell < gy) continue;
                    if (ey > gy + view_h) break;
                    LayoutRect sr{ ex, ey, cell, cell };
                    bool sh_ = sr.contains(g_mouse);
                    prim::fillRR(ctx, ex, ey, cell, cell, 7.0f,
                                 br.solidA(pal.primary, ct * (sh_ ? 0.12f : 0.035f)));
                    ID2D1Bitmap* sticker_bmp = nullptr;
                    std::wstring sp = cur_pack.stickers[i];
                    auto sd = sp.find_last_of(L'.');
                    bool is_gif = (sd != std::wstring::npos
                                   && (sp.substr(sd) == L".gif"
                                       || sp.substr(sd) == L".GIF"));
                    if (decode_bitmaps) {
                        if (is_gif) {
                            auto* sa = app.gifs().fromFile(sp, 100);
                            if (sa) sticker_bmp = app.gifs().frameAt(sa, stages::g_time_in_stage);
                        }
                        if (!sticker_bmp) sticker_bmp = app.images().fromFile(sp, 100);
                    }
                    if (sticker_bmp) {
                        prim::pushLayerRR(ctx, app.factory(),
                                          ex + 4, ey + 4, cell - 8, cell - 8, 8.0f);
                        ctx->DrawBitmap(sticker_bmp,
                            D2D1::RectF(ex + 4, ey + 4, ex + cell - 4, ey + cell - 4),
                            ct, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                        prim::popLayer(ctx);
                    }
                    std::wstring path = cur_pack.stickers[i];
                    bool can_delete = cur_pack.is_owner;
                    auto send_sticker = [path]() {
                        // React 模式:后端 reactions 仅存 <=16 字符的 emoji 文本
                        // (chat.rs react),贴纸/GIF 是文件引用无法作为反应存储/广播。
                        // 不再误把贴纸当成一条频道消息发出——阻止并提示改用 Emoji 标签。
                        if (g_react_target.active) {
                            toast::show(trW("toast.react_emoji_only"));
                            return;
                        }
                        if (!requireActiveChannelWrite()) return;
                        Msg m;
                        auto sd2 = path.find_last_of(L'.');
                        bool is_g = (sd2 != std::wstring::npos
                                     && (path.substr(sd2) == L".gif"
                                         || path.substr(sd2) == L".GIF"));
                        m.kind = is_g ? MsgKind::Gif : MsgKind::Sticker;
                        m.from = L"me";
                        m.author = g_user.nickname;
                        m.author_key = selfAuthorKey();
                        m.status = L"online";
                        m.body = path;
                        m.time = localTimeText();
                        m.client_msg_id = makeClientMsgId();
                        m.send_state = MsgSendState::Pending;
                        std::string client_msg_id = m.client_msg_id;
                        appendLocalMessage(std::move(m));
                        sendChatMessage(GetActiveWindow(), path,
                                        is_g ? "gif" : "sticker", client_msg_id);
                        setPickerOpen(false);
                    };
                    hit(sr, send_sticker, true);
                    if (sh_ && can_delete) {
                        LayoutRect xb{ ex + cell - 18, ey + 2, 16, 16 };
                        bool xh = xb.contains(g_mouse);
                        prim::fillCircle(ctx, xb.x + 8, xb.y + 8, 8,
                                         br.solidA(0x000000, ct * (xh ? 0.85f : 0.65f)));
                        icons::drawIcon(app, icons::Name::X, xb.x + 2, xb.y + 2, 12,
                                        fadeArgb(0xFFFFFFFF, ct));
                        hit(xb, [path](){
                            auto* payload = new std::wstring(path);
                            PostMessageW(GetActiveWindow(), WM_APP + 40,
                                         (WPARAM)payload, 0);
                        }, true);
                    }
                }
                ctx->PopAxisAlignedClip();

                if (max_scroll > 0) {
                    float bar_x = px + pw - 18.0f;
                    float bar_w = 6.0f;
                    float bar_h_p = (std::max)(28.0f, (view_h / total_h) * view_h);
                    float bar_top = gy + (view_h - bar_h_p) * (g_pack_scroll_y / max_scroll);
                    LayoutRect thumb{ bar_x - 4, bar_top, bar_w + 8, bar_h_p };
                    bool bar_hov = thumb.contains(g_mouse) || (g_picker_scroll_drag.active && g_picker_scroll_drag.mode == 1);
                    prim::fillRR(ctx, bar_x, gy, bar_w, view_h, 2.0f,
                                 br.solidA(pal.text, ct * 0.05f));
                    prim::fillRR(ctx, bar_x, bar_top, bar_w, bar_h_p, 2.0f,
                                 br.solidA(pal.text, ct * (bar_hov ? 0.50f : 0.30f)));
                    float anchor_y = (float)g_mouse.y;
                    float anchor_scroll = g_pack_scroll_y;
                    hit(thumb, [anchor_y, anchor_scroll, view_h, bar_h_p, max_scroll](){
                        g_picker_scroll_drag.active = true;
                        g_picker_scroll_drag.mode = 1;
                        g_picker_scroll_drag.anchor_mouse_y = anchor_y;
                        g_picker_scroll_drag.anchor_scroll_y = anchor_scroll;
                        g_picker_scroll_drag.track_h = view_h;
                        g_picker_scroll_drag.thumb_h = bar_h_p;
                        g_picker_scroll_drag.max_scroll = max_scroll;
                    }, true);
                }
            }

        // ===== 操作行：[复制分享链接] [重命名(仅 owner)] [删除(仅 owner)] =====
        // 「我的表情」是每个用户的默认分组，不允许重命名/删除（只能改其中的 sticker）。
        if (!cur_pack.is_system && !cur_pack.id.empty()) {
            bool is_my_stickers = sticker::isMyStickersPack(cur_pack);
            float oy = py + ph - 36;
            std::string pid = cur_pack.id;
            std::wstring pname = cur_pack.name;
            bool is_owner = cur_pack.is_owner;

            // 分享：永远是「复制分享链接」按钮，点击 → sharePack(pid, true) → 自动复制
            // 按钮宽度按文案实测（英/日文比中文长），并 NO_WRAP 防折行裁切。
            auto btn_w = [&](const std::wstring& lbl, float minw) {
                return (std::max)(minw, std::ceil(measureW(app, lbl, hint_fmt)) + 20.0f);
            };
            std::wstring share_lbl = trW("picker.copy_share_link");
            float share_w = btn_w(share_lbl, 110.0f);
            LayoutRect sb{ px + 14, oy, share_w, 24 };
            bool s_h = sb.contains(g_mouse);
            prim::fillRR(ctx, sb.x, sb.y, sb.w, sb.h, 4,
                         br.solidA(pal.primary, ct * (s_h ? 0.30f : 0.15f)));
            prim::drawTextNoWrap(ctx, share_lbl, hint_fmt,
                            sb.x, sb.y + 5, sb.w, 16,
                            br.solidA(pal.primary, ct),
                            DWRITE_TEXT_ALIGNMENT_CENTER);
            hit(sb, [pid](){
                sticker::sharePack(GetActiveWindow(), pid, true);
            }, true);

            float bx2 = sb.x + share_w + 6;
            if (is_owner && !is_my_stickers) {
                std::wstring rn_lbl = trW("picker.rename");
                float rn_w = btn_w(rn_lbl, 64.0f);
                LayoutRect rb{ bx2, oy, rn_w, 24 };
                bool rh = rb.contains(g_mouse);
                prim::fillRR(ctx, rb.x, rb.y, rb.w, rb.h, 4,
                             br.solidA(pal.text, ct * (rh ? 0.10f : 0.05f)));
                prim::drawTextNoWrap(ctx, rn_lbl, hint_fmt,
                                rb.x, rb.y + 5, rb.w, 16,
                                br.solidA(pal.text, ct),
                                DWRITE_TEXT_ALIGNMENT_CENTER);
                hit(rb, [pid, pname](){
                    setPickerOpen(false);
                    auto* payload = new PackActionPayload{ pid, pname };
                    PostMessageW(GetActiveWindow(), WM_APP + 31,
                                 (WPARAM)payload, 0);
                }, true);
                bx2 += rn_w + 6;
            }
            // 删除：owner = 删自己创建的；非 owner = 卸载（uninstall）
            // 「我的表情」默认分组不可删除，跳过此按钮。
            if (!is_my_stickers) {
            std::wstring del_lbl = is_owner ? trW("picker.delete") : trW("picker.uninstall");
            float del_w = btn_w(del_lbl, 64.0f);
            LayoutRect db{ bx2, oy, del_w, 24 };
            bool dh = db.contains(g_mouse);
            prim::fillRR(ctx, db.x, db.y, db.w, db.h, 4,
                         br.solidA(0xE34B4B, ct * (dh ? 0.18f : 0.08f)));
            prim::drawTextNoWrap(ctx, del_lbl, hint_fmt,
                            db.x, db.y + 5, db.w, 16,
                            br.solidA(0xE34B4B, ct),
                            DWRITE_TEXT_ALIGNMENT_CENTER);
            hit(db, [pid, pname, is_owner](){
                auto* payload = new PackActionPayload{ pid, pname };
                if (is_owner) {
                    PostMessageW(GetActiveWindow(), WM_APP + 32,
                                 (WPARAM)payload, 0);
                } else {
                    PostMessageW(GetActiveWindow(), WM_APP + 51,
                                 (WPARAM)payload, 0);
                }
            }, true);
            }
        }
        }
    }

    // 顶部固定命中层最后注册，避免滚动内容或贴纸格子吞掉 tab/按钮点击。
    hit(tab_em, [](){
        setPickerTabSmooth(0);
    }, true);
    hit(tab_pk, [](){
        setPickerTabSmooth(g_picker_tab == 0 ? 1 : g_picker_tab);
    }, true);
    hit({ bx_files, right_btn_y, bw_files, 26 }, [cur_pid](){
        if (cur_pid.empty()) {
            PostMessageW(GetActiveWindow(), WM_APP + 41, 0, 0);
        } else {
            auto* payload = new std::string(cur_pid);
            PostMessageW(GetActiveWindow(), WM_APP + 67,
                         (WPARAM)payload, 0);
        }
    }, true);
    hit({ bx_imp, right_btn_y, bw_imp, 26 }, [cur_pid](){
        if (cur_pid.empty()) {
            PostMessageW(GetActiveWindow(), WM_APP + 41, 0, 0);
        } else {
            auto* payload = new std::string(cur_pid);
            PostMessageW(GetActiveWindow(), WM_APP + 34,
                         (WPARAM)payload, 0);
        }
    }, true);
    hit({ bx_exp, right_btn_y, bw_exp, 26 }, [cur_pid](){
        if (cur_pid.empty()) {
            PostMessageW(GetActiveWindow(), WM_APP + 41, 0, 0);
        } else {
            sticker::exportPackToFolder(GetActiveWindow(), cur_pid);
        }
    }, true);
    hit({ bx_new, right_btn_y, bw_new, 26 }, [](){
        setPickerOpen(false);
        PostMessageW(GetActiveWindow(), WM_APP + 21, 0, 0);
    }, true);
}

void paintChatView(D2DApp& app, float ax, float ay, float aw, float ah) {
    float lw = 240.0f;
    paintChatList(app, ax, ay, lw, ah);
    paintChatPane(app, ax + lw + 1, ay, aw - lw - 1, ah);

    // picker 在 composer 上面浮起
    float comp_h = 64;
    paintPicker(app, ax + lw + 1 + 14, ay + ah - comp_h);
}


} // namespace launcher::d2d::chat
