// admin SSR helper：handler render askama Template 返回 axum Html 响应。
//
// askama_axum 0.4 没把 askama::Template 自动 impl axum::response::IntoResponse，
// 所以走 helper 显式 render -> Html。

pub const ROUTE_DASHBOARD: &str = "dashboard";
pub const ROUTE_USERS: &str = "users";
pub const ROUTE_INVITES: &str = "invites";
pub const ROUTE_REBIND: &str = "rebind";
pub const ROUTE_CHANNELS: &str = "channels";
pub const ROUTE_CHAT: &str = "chat";
pub const ROUTE_TICKETS: &str = "tickets";
pub const ROUTE_ANNOUNCEMENTS: &str = "announcements";
pub const ROUTE_CONFIG: &str = "config";
pub const ROUTE_OPERATORS: &str = "operators";
pub const ROUTE_AUDIT: &str = "audit";

// 管理面板页脚展示用的 host 标签。默认占位，可用 LAUNCHER_PUBLIC_HOST 环境变量
// 覆盖为真实公网地址；避免把生产地址写死进源码。
pub fn host() -> &'static str {
    use std::sync::OnceLock;
    static HOST: OnceLock<String> = OnceLock::new();
    HOST.get_or_init(|| {
        std::env::var("LAUNCHER_PUBLIC_HOST").unwrap_or_else(|_| "localhost:1337".into())
    })
    .as_str()
}

#[derive(Clone)]
pub struct AdminNotice {
    pub kind: &'static str,
    pub text: &'static str,
}

impl AdminNotice {
    pub fn success(text: &'static str) -> Self {
        Self {
            kind: "success",
            text,
        }
    }
    pub fn error(text: &'static str) -> Self {
        Self {
            kind: "error",
            text,
        }
    }
    pub fn warning(text: &'static str) -> Self {
        Self {
            kind: "warning",
            text,
        }
    }
}

pub fn render<T: askama::Template>(t: &T) -> axum::response::Html<String> {
    axum::response::Html(
        t.render()
            .unwrap_or_else(|e| format!("<pre>template error: {e}</pre>")),
    )
}
