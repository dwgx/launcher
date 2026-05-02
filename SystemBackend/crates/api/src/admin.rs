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
        .with_state(state)
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
