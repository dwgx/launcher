#pragma once

// 聊天 view：Discord 风固定左栏 (240px) + 频道流。
// 频道列表官方写死，admin 可在后端 /admin/channels 重命名/清理。
// 入场动画：bubble cubic-bezier(0.16,1,0.3,1) 280ms translateY(6) scale(.97).

#include "app/common.h"
#include "ui/anim/animated_property.h"
#include "ui/components/avatar.h"
#include "ui/views/view.h"

#include <string>
#include <vector>
#include <unordered_map>

namespace launcher::ui::views {

struct ChatChannel {
    std::string id;
    std::string name;
    std::string group;       // 例：IMPORTANT / GENERAL / GAMES / SHOP
    u32  unread{0};
    bool is_market{false};   // SHOP/market 走 MarketView 渲染
};

struct ChatMessageView {
    enum class Kind { Text, Sticker, Gif, Image, Video, LinkCard, System, DayDivider, Typing };
    Kind        kind{Kind::Text};
    std::string from_uid;        // "me" 表示自己
    std::string author_name;
    std::string body;            // 文字 / sticker / gif key / link href / day text
    std::string time_hhmm;
    std::string reply_to_uid;    // 引用消息（QQ/TG/Discord 风）
    std::string reply_to_excerpt;
    bool        read{false};
    u32         video_seconds{0};
};

class ChatView : public View {
public:
    void setMyName(std::string name);
    void setChannels(std::vector<ChatChannel> channels);
    void appendMessage(const std::string& channel_id, ChatMessageView msg);

    void onEnter() override;
    void tick(f32 dt) override;
    void draw(render::SkiaRenderer& r, render::FontManager& f, Rect area) override;
    void onMouseMove(f32 x, f32 y, Rect area) override;
    bool onClick(f32 x, f32 y, Rect area) override;

private:
    std::string m_my_name;
    std::vector<ChatChannel> m_channels;
    std::unordered_map<std::string, std::vector<ChatMessageView>> m_streams;
    std::string m_active_channel{"general"};
    std::unordered_map<std::string, std::string> m_drafts;
    std::unordered_map<std::string, bool> m_group_collapsed;
    enum class Picker { None, Emoji, Sticker, Gif } m_picker{Picker::None};
    anim::AnimatedProperty<f32> m_picker_open{0.0f};
};

}  // namespace launcher::ui::views
