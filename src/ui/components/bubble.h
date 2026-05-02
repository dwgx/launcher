#pragma once

// 聊天气泡 — 综合 QQ / Telegram / Discord 三家最优写法：
//   * 同一作者连续消息：共用 avatar + tail-stack (尾部圆角拉直)
//   * 我自己的气泡：右对齐 + primary 填色 + 双勾已读 (Telegram 风)
//   * 引用 (reply): 上方 2px 主色竖条 + 缩略原文 (Discord 风)
//   * 长按/右键消息：弹出操作 popover (回复/复制/反应/删除) (QQ 风)
//   * 入场：280ms easeOutQuint translateY(6) scale(.97) (design styles.css bubble-in)

#include "app/common.h"

#include <string>

namespace launcher::ui::components {

enum class BubbleKind {
    Text, Sticker, Gif, Image, Video, LinkCard, System, DayDivider, Typing,
};

struct BubbleProps {
    BubbleKind  kind{BubbleKind::Text};
    bool        is_me{false};
    bool        prev_same_author{false};   // tail-stack 触发
    bool        in_group_chat{true};       // 是否需要画作者名
    std::string author;
    std::string body;
    std::string time_hhmm;
    bool        read{false};
    // 引用上下文（QQ/Telegram/Discord 通用）
    std::string reply_author;
    std::string reply_excerpt;
    // 链接卡（自动检测 URL）
    std::string link_url;
    std::string link_title;
    std::string link_host;
    // 视频
    u32         video_seconds{0};
};

}  // namespace launcher::ui::components
