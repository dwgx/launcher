// Admin SSR：登录、dashboard。
// users / invites / rebind 各管自己的 page struct（在对应文件）。
// 全部继承 templates/base.html 的 layout（Tailwind + DaisyUI luxury 主题）。

use crate::state::AppState;
use crate::ui;
use axum::{
    routing::get,
    extract::{State, Form},
    response::{IntoResponse, Redirect, Html},
    Router,
};
use serde::Deserialize;
use std::sync::Arc;
use askama::Template;

// ---------------- login ----------------
#[derive(Template, Default)]
#[template(path = "login.html")]
pub struct LoginPage {
    pub error:     bool,
    pub error_msg: String,
}

#[derive(Deserialize)]
pub struct AdminLogin { pub password: String }

async fn login_page() -> Html<String> {
    ui::render(&LoginPage::default())
}

async fn login_submit(
    State(s): State<Arc<AppState>>,
    Form(form): Form<AdminLogin>,
) -> axum::response::Response {
    if form.password == s.cfg.admin_password {
        Redirect::to("/admin").into_response()
    } else {
        ui::render(&LoginPage { error: true, error_msg: "密码错误".into() }).into_response()
    }
}

async fn logout() -> impl IntoResponse {
    // 当前简化：还没做 admin session，直接回 login 页
    Redirect::to("/admin/login")
}

// ---------------- dashboard ----------------
pub struct AuditRow {
    pub time: String,
    pub action: String,
    pub actor: String,
    pub target: String,
}

#[derive(Template)]
#[template(path = "dashboard_content.html")]
pub struct DashboardPage {
    pub title:           String,
    pub subtitle:        Option<String>,
    pub host:            &'static str,
    pub route:           &'static str,

    pub user_count:      i64,
    pub user_today:      i64,
    pub active_sessions: i64,
    pub pending_rebinds: i64,
    pub active_invites:  i64,

    pub bind_addr:       String,
    pub tls_on:          bool,
    pub require_invite:  bool,
    pub cdn_base:        String,
    pub sub_count:       i64,

    pub recent_audit:    Vec<AuditRow>,
}

async fn dashboard(State(s): State<Arc<AppState>>) -> Html<String> {
    let user_count   = sqlx::query_scalar!("SELECT COUNT(*) FROM users")
        .fetch_one(&s.db).await.unwrap_or(Some(0)).unwrap_or(0);
    let user_today   = sqlx::query_scalar!(
        "SELECT COUNT(*) FROM users WHERE created_at > now() - interval '1 day'")
        .fetch_one(&s.db).await.unwrap_or(Some(0)).unwrap_or(0);
    let active_sessions = sqlx::query_scalar!(
        "SELECT COUNT(*) FROM sessions WHERE expires_at > now()")
        .fetch_one(&s.db).await.unwrap_or(Some(0)).unwrap_or(0);
    let pending_rebinds = sqlx::query_scalar!(
        "SELECT COUNT(*) FROM hwid_rebind_requests WHERE status='pending'")
        .fetch_one(&s.db).await.unwrap_or(Some(0)).unwrap_or(0);
    let active_invites = sqlx::query_scalar!(
        "SELECT COUNT(*) FROM invite_codes \
         WHERE revoked_at IS NULL \
           AND (expires_at IS NULL OR expires_at > now()) \
           AND use_count < max_uses")
        .fetch_one(&s.db).await.unwrap_or(Some(0)).unwrap_or(0);
    let sub_count = sqlx::query_scalar!("SELECT COUNT(*) FROM subscriptions")
        .fetch_one(&s.db).await.unwrap_or(Some(0)).unwrap_or(0);

    let audit = sqlx::query!(
        r#"SELECT occurred_at, action, actor, target
           FROM audit_log ORDER BY occurred_at DESC LIMIT 12"#)
        .fetch_all(&s.db).await.unwrap_or_default();
    let recent_audit = audit.into_iter().map(|r| AuditRow {
        time:   r.occurred_at.format("%m-%d %H:%M:%S").to_string(),
        action: r.action,
        actor:  r.actor.unwrap_or_else(|| "—".into()),
        target: r.target.unwrap_or_else(|| "—".into()),
    }).collect();

    ui::render(&DashboardPage {
        title: "仪表盘".into(),
        subtitle: Some(format!("{} · 当前 {} 个活跃 session", ui::host(), active_sessions)),
        host: ui::host(),
        route: ui::ROUTE_DASHBOARD,
        user_count, user_today, active_sessions, pending_rebinds, active_invites,
        bind_addr: s.cfg.bind_addr.clone(),
        tls_on: s.cfg.tls_cert_path.is_some(),
        require_invite: s.cfg.require_invite_code,
        cdn_base: s.cfg.cdn_base.clone(),
        sub_count,
        recent_audit,
    })
}

pub fn routes(state: Arc<AppState>) -> Router<Arc<AppState>> {
    Router::new()
        .route("/",       get(dashboard))
        .route("/login",  get(login_page).post(login_submit))
        .route("/logout", get(logout))
        .with_state(state)
}
