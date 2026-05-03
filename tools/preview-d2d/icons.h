// Claude 风 SVG icons — 1:1 复刻 tools/preview/icons.inl 30+ 个图标。
// 用 ID2D1PathGeometry + DrawLine + DrawEllipse 替代 GDI+ GraphicsPath / DrawPath。
#pragma once

#include "d2d_app.h"

namespace launcher::d2d::icons {

enum class Name {
    Home, Library, Cloud, Chat, Settings, Logout, Logo,
    Eye, EyeOff, X, History, Search, Send, Phone, Video, More,
    Smile, Paperclip, Check, Check2, User, Moon, Shield, Bell,
    Reply, At, Link, Play, Trash, Edit, Plus, Hash,
    Cart,
};

// 在 [x, y, x+size, y+size] 区域画 24×24 viewBox 路径。
// stroke = ARGB32 颜色；stroke_w = 描边宽度（按 size/24 缩放）。
void drawIcon(D2DApp& app, Name n, float x, float y, float size,
              uint32_t stroke_argb, float stroke_w = 1.7f);

}  // namespace launcher::d2d::icons
