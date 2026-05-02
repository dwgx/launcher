#pragma once

// 登录页：用户名 + 密码 + 记住我 + 登录按钮。
// 在加载完成后由 EventLoop 创建并切入。

#include "app/common.h"
#include "ui/anim/animated_property.h"
#include "ui/views/view.h"
#include <functional>

namespace launcher::ui::views {

struct LoginCredentials {
    std::string username;
    std::string password;
    bool remember{true};
};

class LoginView : public View {
public:
    using OnSubmitFn = std::function<void(const LoginCredentials&)>;

    void setOnSubmit(OnSubmitFn fn) { m_on_submit = std::move(fn); }
    void setError(std::string msg) { m_error = std::move(msg); }

    void onEnter() override;
    void tick(f32 dt) override;
    void draw(render::SkiaRenderer& r, render::FontManager& f, Rect area) override;

    void onMouseMove(f32 x, f32 y, Rect area) override;
    bool onClick(f32 x, f32 y, Rect area) override;
    void onChar(u32 codepoint);
    void onKeyDown(int vk);

private:
    LoginCredentials m_creds;
    std::string      m_error;
    int              m_focus{-1};   // 0=username 1=password
    OnSubmitFn       m_on_submit;
    anim::AnimatedProperty<f32> m_opacity{0.0f};
    anim::AnimatedProperty<f32> m_card_y{12.0f};
};

}  // namespace launcher::ui::views
