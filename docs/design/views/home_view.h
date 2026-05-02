#pragma once

// 主页：用户问候 + 头像大卡片 + 订阅/到期/设备 ID/上次登录详情卡。
// 入场动画：内容 stagger 浮入 (delay 0.2 + 200ms 淡入 + 12px 上移)。

#include "app/common.h"
#include "ui/anim/animated_property.h"
#include "ui/components/avatar.h"
#include "ui/views/view.h"

namespace launcher::ui::views {

struct UserSummary {
    std::string name;
    std::string email;
    std::string tier_key;          // i18n key: "tier.1week" 等
    std::string device_id_short;   // 显示截断后的 hex
    std::string subscription_expires_human;
    std::string last_login_human;
    bool        online{true};
};

class HomeView : public View {
public:
    void setUser(UserSummary u);

    void onEnter() override;
    void tick(f32 dt) override;
    void draw(render::SkiaRenderer& r, render::FontManager& f, Rect area) override;

private:
    UserSummary m_user;
    components::Avatar m_avatar;
    anim::AnimatedProperty<f32> m_opacity{0.0f};
    anim::AnimatedProperty<f32> m_translate_y{12.0f};
};

}  // namespace launcher::ui::views
