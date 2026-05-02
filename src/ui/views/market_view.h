#pragma once

// 市场 view：用户出售 .cfg 参数 (CS2 灵敏度 / crosshair / autoexec) 等虚拟商品。
// design: market-grid 单列 + market-card hover translateY(-1) + border primary.4
// + market-search 38 高 input + market-sort 排序下拉 + 分页 5/page.

#include "app/common.h"
#include "ui/anim/animated_property.h"
#include "ui/views/view.h"

#include <string>
#include <vector>

namespace launcher::ui::views {

struct MarketListing {
    u64         id{0};
    std::string title;
    std::string author;          // 卖家昵称
    std::string body;            // 详情多行
    std::string price_label;     // "¥420" / "求购" / "¥50/局"
    std::string tag;             // "热卖" / "全新" / "求购" / "服务" / "虚拟"
    std::string category;        // cs2-cfg / autoexec / sensitivity / etc.
    u64         sold_count{0};
};

enum class MarketSort { Hot, Newest, PriceAsc };

class MarketView : public View {
public:
    void setListings(std::vector<MarketListing> items);

    void onEnter() override;
    void tick(f32 dt) override;
    void draw(render::SkiaRenderer& r, render::FontManager& f, Rect area) override;
    void onMouseMove(f32 x, f32 y, Rect area) override;
    bool onClick(f32 x, f32 y, Rect area) override;

private:
    std::vector<MarketListing> m_items;
    std::string m_query;
    MarketSort m_sort{MarketSort::Hot};
    u32 m_page{0};
    static constexpr u32 kPerPage = 5;
};

}  // namespace launcher::ui::views
