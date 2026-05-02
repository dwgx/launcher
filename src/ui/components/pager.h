#pragma once

// 分页控件 (用于 History modal / Market view).
// design: 28x28 6px radius border divider; on=primary; disabled=opacity .4.

#include "app/common.h"

#include <functional>

namespace launcher::ui::components {

struct PagerProps {
    u32 page{0};
    u32 total_pages{1};
    std::function<void(u32)> on_change;
};

}  // namespace launcher::ui::components
