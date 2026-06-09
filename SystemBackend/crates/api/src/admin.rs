// Admin SSR：登录、dashboard。
// users / invites / rebind 各管自己的 page struct（在对应文件）。
// 全部继承 templates/base.html 的 layout（Tailwind + DaisyUI luxury 主题）。

use crate::state::AppState;
use crate::ui;
use axum::{
    routing::get,
    extract::{State, Form},
    http::{header, HeaderMap, HeaderValue},
    response::{IntoResponse, Redirect, Html, Response},
    Router,
};
use chrono::Utc;
use serde::Deserialize;
use std::sync::Arc;
use askama::Template;
use sha2::{Digest, Sha256};

const ADMIN_COOKIE: &str = "launcher_admin";
const ADMIN_COOKIE_MAX_AGE: i64 = 60 * 60 * 12;

fn admin_cookie_value(state: &AppState, issued_at: i64) -> String {
    let mut hasher = Sha256::new();
    hasher.update(b"launcher.admin.session.v1");
    hasher.update(state.cfg.admin_password.as_bytes());
    hasher.update(issued_at.to_string().as_bytes());
    format!("{}:{}", issued_at, hex::encode(hasher.finalize()))
}

fn validate_admin_cookie_value(state: &AppState, value: &str) -> bool {
    let Some((issued_at_raw, _sig)) = value.split_once(':') else {
        return false;
    };
    let Ok(issued_at) = issued_at_raw.parse::<i64>() else {
        return false;
    };
    let now = Utc::now().timestamp();
    if issued_at > now || now.saturating_sub(issued_at) > ADMIN_COOKIE_MAX_AGE {
        return false;
    }
    value == admin_cookie_value(state, issued_at)
}

pub(crate) fn has_admin_session(headers: &HeaderMap, state: &AppState) -> bool {
    let Some(raw) = headers.get(header::COOKIE).and_then(|v| v.to_str().ok()) else {
        return false;
    };
    raw.split(';').any(|part| {
        let part = part.trim();
        let Some((name, value)) = part.split_once('=') else {
            return false;
        };
        name == ADMIN_COOKIE && validate_admin_cookie_value(state, value)
    })
}

pub(crate) fn admin_login_redirect() -> Response {
    Redirect::to("/admin/login").into_response()
}

fn set_admin_cookie(resp: &mut Response, state: &AppState) {
    let secure = if state.cfg.tls_cert_path.is_some() { "; Secure" } else { "" };
    let cookie = format!(
        "{}={}; Path=/admin; Max-Age={}; HttpOnly; SameSite=Strict{}",
        ADMIN_COOKIE, admin_cookie_value(state, Utc::now().timestamp()), ADMIN_COOKIE_MAX_AGE, secure
    );
    if let Ok(value) = HeaderValue::from_str(&cookie) {
        resp.headers_mut().insert(header::SET_COOKIE, value);
    }
}

fn clear_admin_cookie(resp: &mut Response, state: &AppState) {
    let secure = if state.cfg.tls_cert_path.is_some() { "; Secure" } else { "" };
    let cookie = format!(
        "{}=; Path=/admin; Max-Age=0; HttpOnly; SameSite=Strict{}",
        ADMIN_COOKIE, secure
    );
    if let Ok(value) = HeaderValue::from_str(&cookie) {
        resp.headers_mut().insert(header::SET_COOKIE, value);
    }
}

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
        let mut resp = Redirect::to("/admin").into_response();
        set_admin_cookie(&mut resp, &s);
        resp
    } else {
        ui::render(&LoginPage { error: true, error_msg: "密码错误".into() }).into_response()
    }
}

async fn logout(State(s): State<Arc<AppState>>) -> Response {
    let mut resp = Redirect::to("/admin/login").into_response();
    clear_admin_cookie(&mut resp, &s);
    resp
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
    pub notice:          Option<ui::AdminNotice>,
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

async fn dashboard(State(s): State<Arc<AppState>>, headers: HeaderMap) -> Response {
    if !has_admin_session(&headers, &s) {
        return admin_login_redirect();
    }

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
        notice: None,
        host: ui::host(),
        route: ui::ROUTE_DASHBOARD,
        user_count, user_today, active_sessions, pending_rebinds, active_invites,
        bind_addr: s.cfg.bind_addr.clone(),
        tls_on: s.cfg.tls_cert_path.is_some(),
        require_invite: s.cfg.require_invite_code,
        cdn_base: s.cfg.cdn_base.clone(),
        sub_count,
        recent_audit,
    }).into_response()
}

pub fn routes(state: Arc<AppState>) -> Router<Arc<AppState>> {
    Router::new()
        .route("/",       get(dashboard))
        .route("/login",  get(login_page).post(login_submit))
        .route("/logout", get(logout))
        .with_state(state)
}
