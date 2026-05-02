// 管理后台：SSR 渲染（askama），不要 SPA。
// 路由：/admin/login → /admin/dashboard → /admin/users → /admin/subscriptions

use crate::state::AppState;
use axum::{
    routing::{get, post},
    extract::{State, Form},
    response::{IntoResponse, Redirect, Html},
    http::StatusCode,
    Router,
};
use serde::Deserialize;
use std::sync::Arc;
use askama::Template;

#[derive(Template)]
#[template(source = "<!doctype html><html><head><meta charset=utf-8><title>Launcher Admin</title>\
<style>body{font-family:'Source Han Sans CN',sans-serif;background:#FAF7F2;color:#1F1E1D;padding:32px;}\
h1{color:#C96442;}table{border-collapse:collapse;width:100%}td,th{border:1px solid #EDE9E1;padding:8px;text-align:left;}\
.btn{background:#C96442;color:#fff;padding:8px 16px;border:none;border-radius:8px;cursor:pointer;}</style></head>\
<body><h1>Launcher Admin</h1><p>Users: {{ user_count }}, Subscriptions: {{ sub_count }}</p>\
<p><a href=/admin/users>Users</a> · <a href=/admin/subscriptions>Subscriptions</a></p></body></html>", ext = "html")]
struct DashTpl { user_count: i64, sub_count: i64 }

#[derive(Template)]
#[template(source = "<!doctype html><html><body><form method=post action=/admin/login>\
<input name=password type=password placeholder=password><button>Sign in</button></form></body></html>", ext = "html")]
struct LoginTpl;

#[derive(Deserialize)]
pub struct AdminLogin { pub password: String }

pub fn routes(state: Arc<AppState>) -> Router<Arc<AppState>> {
    Router::new()
        .route("/", get(dashboard))
        .route("/login", get(login_page).post(login_submit))
        .route("/rebind", get(rebind_pending_page))
        .route("/rebind/:id/approve", post(crate::rebind::admin_approve))
        .route("/rebind/:id/deny",    post(crate::rebind::admin_deny))
        .with_state(state)
}

#[derive(Template)]
#[template(source = "<!doctype html><html><head><meta charset=utf-8><title>Rebind - Launcher</title>\
<style>body{font-family:'Source Han Sans CN',sans-serif;background:#FAF7F2;color:#1F1E1D;padding:32px;}\
h1{color:#C96442;}.row{background:#FFF;padding:16px;border-radius:12px;margin:8px 0;box-shadow:0 1px 3px rgba(0,0,0,.04);}\
.btn{background:#C96442;color:#fff;padding:6px 14px;border:none;border-radius:8px;cursor:pointer;}\
.deny{background:#E34B4B;}.muted{color:#6B6A67;font-size:13px;}</style></head>\
<body><h1>HWID Rebind Requests</h1><p><a href=/admin>← Back</a></p>\
{% for r in rows %}<div class=row>\
<div><b>{{ r.user_id }}</b> · {{ r.submitted_at }}</div>\
<div class=muted>old: {{ r.old }}<br>new: {{ r.new }}<br>reason: {{ r.reason }}</div>\
<div class=muted>{{ r.diff }}</div>\
<form method=post action=/admin/rebind/{{ r.id }}/approve style='display:inline'><button class=btn>Approve</button></form>\
<form method=post action=/admin/rebind/{{ r.id }}/deny style='display:inline'><button class='btn deny'>Deny</button></form>\
</div>{% endfor %}</body></html>", ext = "html")]
struct RebindTpl { rows: Vec<RebindRowVm> }

#[derive(Default)]
struct RebindRowVm {
    id: String, user_id: String, submitted_at: String,
    old: String, new: String, reason: String, diff: String,
}

async fn rebind_pending_page(State(s): State<Arc<AppState>>) -> impl IntoResponse {
    let rows = sqlx::query!(
        r#"SELECT id, user_id, submitted_at, old_fingerprint, new_fingerprint,
                  user_reason, parts_diff
           FROM hwid_rebind_requests WHERE status='pending'
           ORDER BY submitted_at DESC LIMIT 100"#)
        .fetch_all(&s.db).await.unwrap_or_default();

    let vm = RebindTpl {
        rows: rows.into_iter().map(|r| RebindRowVm {
            id: r.id.to_string(),
            user_id: r.user_id.to_string(),
            submitted_at: r.submitted_at.format("%Y-%m-%d %H:%M").to_string(),
            old: r.old_fingerprint.unwrap_or_default(),
            new: r.new_fingerprint,
            reason: r.user_reason.unwrap_or_default(),
            diff: r.parts_diff.to_string(),
        }).collect()
    };
    Html(vm.render().unwrap_or_default())
}

async fn login_page() -> impl IntoResponse {
    Html(LoginTpl.render().unwrap())
}

async fn login_submit(
    State(s): State<Arc<AppState>>,
    Form(form): Form<AdminLogin>,
) -> impl IntoResponse {
    if form.password == s.cfg.admin_password {
        Redirect::to("/admin").into_response()
    } else {
        (StatusCode::UNAUTHORIZED, "wrong password").into_response()
    }
}

async fn dashboard(State(s): State<Arc<AppState>>) -> impl IntoResponse {
    let user_count = sqlx::query_scalar!("SELECT COUNT(*) FROM users")
        .fetch_one(&s.db).await.unwrap_or(Some(0)).unwrap_or(0);
    let sub_count  = sqlx::query_scalar!("SELECT COUNT(*) FROM subscriptions")
        .fetch_one(&s.db).await.unwrap_or(Some(0)).unwrap_or(0);
    Html(DashTpl { user_count, sub_count }.render().unwrap())
}
