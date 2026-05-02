// admin SSR helper：handler render askama Template 返回 axum Html 响应。
//
// askama_axum 0.4 没把 askama::Template 自动 impl axum::response::IntoResponse，
// 所以走 helper 显式 render -> Html。

pub const ROUTE_DASHBOARD: &str = "dashboard";
pub const ROUTE_USERS:     &str = "users";
pub const ROUTE_INVITES:   &str = "invites";
pub const ROUTE_REBIND:    &str = "rebind";
pub const ROUTE_CHANNELS:  &str = "channels";

pub fn host() -> &'static str { "154.40.36.22:1337" }

pub fn render<T: askama::Template>(t: &T) -> axum::response::Html<String> {
    axum::response::Html(
        t.render().unwrap_or_else(|e| format!("<pre>template error: {e}</pre>"))
    )
}
