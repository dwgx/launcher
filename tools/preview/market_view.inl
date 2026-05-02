// Market view — 出售 CS2 .cfg 参数 / 灵敏度 / autoexec / crosshair。
// design: market-grid 单列 + 卡片 hover translateY(-1) + border primary.4
//         + market-search 38 高 + market-sort 排序下拉 + 分页 5/page。
#pragma once

namespace marketv {

struct Listing {
    const wchar_t* title;
    const wchar_t* author;
    const wchar_t* price;
    const wchar_t* tag;
    const wchar_t* body;
    int sold;
};

const Listing kListings[] = {
    { L"s1mple — 360° 急停 cfg + crosshair_v2", L"sakuya", L"¥38",   L"热卖",
      L"sens 3.09 dpi 400 zoom 1 · 含 m_yaw 微调 · 实测 2k+ 局", 312 },
    { L"自用 CS2 灵敏度 cfg (FPS 240)",         L"yuki",   L"¥18",   L"虚拟",
      L"raw input + accel 0 · jumpthrow 绑 mwheelup · viewmodel 优化", 220 },
    { L"NiKo crosshair v2025 (绿色)",            L"reimu",  L"¥10",   L"全新",
      L"导入 share code 即可 · 4 套切换 · 含 awp 准心", 180 },
    { L"awp 跳狙 cfg + 网页教程",                 L"yuki",   L"¥28",   L"服务",
      L"按住右键飞跳 · faceit 通过反作弊 · 老 1.6 风格", 90 },
    { L"完整 autoexec 模板 — 240Hz 优化版",       L"flandre",L"¥15",   L"虚拟",
      L"r_drawtracers 0 · cl_interp 0 · cmdrate 128 · 含 mat_queue", 168 },
    { L"代练 Premier 上 18000 (5 局)",           L"yuki",   L"¥260",  L"服务",
      L"3000+ ELO · 不上号 · 失败包退 · 全程录屏", 64  },
    { L"FPS 优化 cfg 包 (低端机专用)",            L"marisa", L"¥12",   L"求购",
      L"i3+1050 起步可用 · 关 shadow 关 ssao · video.txt 完整", 220 },
    { L"求收 Logitech Pro X Superlight 白",      L"flandre",L"求购",   L"求购",
      L"八成新以上 · 长期收 · 价好", 0 },
    { L"viewmodel 全集 (FaZe / NaVi / Liquid)",  L"sakuya", L"¥20",   L"虚拟",
      L"30+ 职业选手视角配置 · 一键切换 · 含 demo 录屏", 145 },
    { L"周末 5v5 内战代组队 (faceit 5k+)",         L"reimu",  L"¥80/局", L"服务",
      L"招满 5 人即开 · 队长经验 3 年 · 不喷队友", 30 },
};

std::wstring g_query;
int g_sort = 0;          // 0=hot 1=new 2=price
int g_page = 0;
const int kPerPage = 5;

bool matches(const Listing& l) {
    if (g_query.empty()) return true;
    std::wstring q = g_query;
    for (auto& c : q) c = (wchar_t)towlower(c);
    auto contains = [&](const wchar_t* hay) {
        std::wstring h = hay;
        for (auto& c : h) c = (wchar_t)towlower(c);
        return h.find(q) != std::wstring::npos;
    };
    return contains(l.title) || contains(l.author);
}

std::vector<const Listing*> filtered() {
    std::vector<const Listing*> r;
    for (auto& l : kListings) if (matches(l)) r.push_back(&l);
    if (g_sort == 1) {
        std::reverse(r.begin(), r.end());
    } else if (g_sort == 2) {
        std::sort(r.begin(), r.end(), [](const Listing* a, const Listing* b) {
            auto parse = [](const wchar_t* p) {
                int v = 0; for (; *p; ++p) if (iswdigit(*p)) v = v * 10 + (*p - L'0');
                return v;
            };
            return parse(a->price) < parse(b->price);
        });
    } else {
        std::sort(r.begin(), r.end(), [](const Listing* a, const Listing* b) {
            return a->sold > b->sold;
        });
    }
    return r;
}

}  // namespace marketv

void paintMarketView(Graphics& g, RectF area) {
    const Palette& pal = palette();
    fillRR(g, area.X, area.Y, area.Width, area.Height, 0, pal.bg);

    float pad = 18;
    float vx = area.X + pad, vy = area.Y + 14;

    // search + sort head
    float sh = 36;
    float sort_w = 100;
    float search_w = area.Width - pad * 2 - sort_w - 10;
    fillRR(g, vx, vy, search_w, sh, 10, pal.card);
    strokeRR(g, vx, vy, search_w, sh, 10, pal.divider);
    icons::drawSvg(g, icons::Name::Search, vx + 12, vy + 9, 16, pal.text_muted);
    if (marketv::g_query.empty()) {
        drawText_(g, L"搜索 CS2 配置 / 卖家", vx + 36, vy + 11, search_w - 50,
                  9.0f, pal.text_muted);
    } else {
        drawText_(g, marketv::g_query.c_str(), vx + 36, vy + 11, search_w - 50,
                  9.0f, pal.text);
    }
    fillRR(g, vx + search_w + 10, vy, sort_w, sh, 10, pal.card);
    strokeRR(g, vx + search_w + 10, vy, sort_w, sh, 10, pal.divider);
    const wchar_t* sort_labels[] = { L"热度", L"最新", L"价格" };
    drawText_(g, sort_labels[marketv::g_sort], vx + search_w + 22, vy + 11, sort_w - 30,
              9.0f, pal.text);
    drawText_(g, L"▾", vx + search_w + sort_w - 6, vy + 11, 14, 8.0f, pal.text_muted);
    int cur_sort = marketv::g_sort;
    hit(RectF(vx + search_w + 10, vy, sort_w, sh), [cur_sort](){
        marketv::g_sort = (cur_sort + 1) % 3;
        marketv::g_page = 0;
    }, true);

    // grid
    float ly = vy + sh + 12;
    auto items = marketv::filtered();
    int total = (int)items.size();
    int pages = std::max(1, (total + marketv::kPerPage - 1) / marketv::kPerPage);
    if (marketv::g_page >= pages) marketv::g_page = pages - 1;

    int start = marketv::g_page * marketv::kPerPage;
    int end = std::min(start + marketv::kPerPage, total);

    for (int i = start; i < end; ++i) {
        const auto* l = items[i];
        float ch = 84;
        bool hov = inRect(g_mouse, RectF(vx, ly, area.Width - pad * 2, ch));
        if (hov) {
            drawShadow(g, vx, ly - 1, area.Width - pad * 2, ch, 12,
                       Color(36, 0, 0, 0), 4, 3);
        }
        fillRR(g, vx, ly - (hov ? 1 : 0), area.Width - pad * 2, ch, 12, pal.card);
        Color borderC = hov ? Color(110, pal.primary.GetR(), pal.primary.GetG(), pal.primary.GetB())
                            : pal.divider;
        strokeRR(g, vx, ly - (hov ? 1 : 0), area.Width - pad * 2, ch, 12, borderC);

        // title + tag pill
        drawText_(g, l->title, vx + 18, ly + 10, area.Width - pad * 2 - 100, 11.0f,
                  pal.text, StringAlignmentNear, FontStyleBold);
        // tag pill 在右上
        float tagw = measureText(g, l->tag, 7.5f, FontStyleBold).Width + 18;
        Color pillBg(40, pal.primary.GetR(), pal.primary.GetG(), pal.primary.GetB());
        fillRR(g, vx + area.Width - pad * 2 - tagw - 14, ly + 10, tagw, 18, 9, pillBg);
        drawText_(g, l->tag, vx + area.Width - pad * 2 - tagw - 14, ly + 13, tagw, 7.5f,
                  pal.primary, StringAlignmentCenter, FontStyleBold);

        // body
        drawText_(g, l->body, vx + 18, ly + 32, area.Width - pad * 2 - 36,
                  8.5f, pal.text_muted);

        // foot: @author · price · sold
        drawText_(g, (std::wstring(L"@") + l->author).c_str(),
                  vx + 18, ly + ch - 22, 120, 8.0f, pal.primary,
                  StringAlignmentNear, FontStyleBold);
        drawText_(g, L"·", vx + 18 + 80, ly + ch - 22, 6, 8.0f, pal.text_muted);
        drawText_(g, l->price, vx + 18 + 92, ly + ch - 22, 80, 8.5f,
                  pal.text, StringAlignmentNear, FontStyleBold);
        if (l->sold > 0) {
            wchar_t sold[32]; swprintf_s(sold, 32, L"已售 %d", l->sold);
            drawText_(g, sold, vx + area.Width - pad * 2 - 80, ly + ch - 22, 60, 7.5f,
                      pal.text_muted, StringAlignmentFar);
        }

        ly += ch + 10;
    }
    if (total == 0) {
        drawText_(g, L"没有匹配的商品", vx, ly + 30, area.Width - pad * 2,
                  10.0f, pal.text_muted, StringAlignmentCenter);
    }

    // pager
    if (pages > 1) {
        float pgy = area.Y + area.Height - 36;
        float btnw = 28, gap = 6;
        float total_w = btnw * (pages + 2) + gap * (pages + 1);
        float pgx = area.X + (area.Width - total_w) / 2;
        // ‹
        bool can_prev = marketv::g_page > 0;
        fillRR(g, pgx, pgy, btnw, btnw, 6, pal.card);
        strokeRR(g, pgx, pgy, btnw, btnw, 6, pal.divider);
        Color prevC = can_prev ? pal.text : Color(80, pal.text.GetR(), pal.text.GetG(), pal.text.GetB());
        drawText_(g, L"‹", pgx, pgy + 6, btnw, 10.0f, prevC, StringAlignmentCenter, FontStyleBold);
        if (can_prev) hit(RectF(pgx, pgy, btnw, btnw), [](){ if (marketv::g_page > 0) marketv::g_page--; }, true);
        pgx += btnw + gap;
        // pages
        for (int p = 0; p < pages; ++p) {
            bool on = (p == marketv::g_page);
            Color bg = on ? pal.primary : pal.card;
            Color fg = on ? Color(255, 255, 255, 255) : pal.text;
            Color bd = on ? pal.primary : pal.divider;
            fillRR(g, pgx, pgy, btnw, btnw, 6, bg);
            strokeRR(g, pgx, pgy, btnw, btnw, 6, bd);
            wchar_t num[8]; swprintf_s(num, 8, L"%d", p + 1);
            drawText_(g, num, pgx, pgy + 6, btnw, 9.5f, fg, StringAlignmentCenter, FontStyleBold);
            int target = p;
            hit(RectF(pgx, pgy, btnw, btnw), [target](){ marketv::g_page = target; }, true);
            pgx += btnw + gap;
        }
        // ›
        bool can_next = marketv::g_page < pages - 1;
        fillRR(g, pgx, pgy, btnw, btnw, 6, pal.card);
        strokeRR(g, pgx, pgy, btnw, btnw, 6, pal.divider);
        Color nextC = can_next ? pal.text : Color(80, pal.text.GetR(), pal.text.GetG(), pal.text.GetB());
        drawText_(g, L"›", pgx, pgy + 6, btnw, 10.0f, nextC, StringAlignmentCenter, FontStyleBold);
        if (can_next) hit(RectF(pgx, pgy, btnw, btnw), [pages](){ if (marketv::g_page < pages - 1) marketv::g_page++; }, true);
    }
}
