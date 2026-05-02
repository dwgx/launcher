#pragma once

// Emoji / Sticker / GIF picker, 320x360, design tokens 1:1.
// 三 tab：Emoji 8 列 grid / Sticker 8 列 / GIF 2 列 4:3 占位.
// 出现位置：composer 上方 64 偏移. 关闭：点 picker 外或 Esc.

#include "app/common.h"
#include "ui/anim/animated_property.h"

#include <functional>
#include <string>
#include <vector>

namespace launcher::ui::components {

struct PickedItem {
    enum class Kind { Emoji, Sticker, Gif } kind{Kind::Emoji};
    std::string value;       // emoji 字符 / sticker key / gif title
};

class Picker {
public:
    void open(PickedItem::Kind initial_tab);
    void close();
    bool is_open() const;

    std::function<void(const PickedItem&)> on_pick;

private:
    bool m_open{false};
    PickedItem::Kind m_tab{PickedItem::Kind::Emoji};
    std::string m_query;
    anim::AnimatedProperty<f32> m_t{0.0f};
};

}  // namespace launcher::ui::components
