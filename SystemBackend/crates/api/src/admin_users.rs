// 管理员用户管控（SSR + JSON API）。
// 改 UID / Username / Nickname / Tier；重置密码。

use crate::state::AppState;
use crate::ui;
use axum::{
    extract::{State, Query, Path, Json, Form},
    http::StatusCode,
    response::{IntoResponse, Redirect, Html},
    Router,
    routing::{get, post},
};
use serde::{Deserialize, Serialize};
use std::sync::Arc;
use uuid::Uuid;
use launcher_shared::{hashing, uid as shared_uid};
use askama::Template;

// ---------------- gate ----------------
#[derive(Deserialize)]
pub struct AdminAuth { pub key: Option<String> }

fn check(state: &AppState, auth: &AdminAuth) -> Result<(), (StatusCode, String)> {
    match &auth.key {
        Some(k) if k == &state.cfg.admin_password => Ok(()),
        _ => Err((StatusCode::UNAUTHORIZED, "admin key invalid".into())),
    }
}

fn internal<E: std::fmt::Display>(e: E) -> (StatusCode, String) {
    (StatusCode::INTERNAL_SERVER_ERROR, e.to_string())
}

// ---------------- JSON API ----------------
#[derive(Serialize)]
pub struct UserSummary {
    pub id:        String,
    pub uid:       Option<String>,
    pub username:  Option<String>,
    pub nickname:  Option<String>,
    pub tier:      Option<String>,
    pub created_at: i64,
    pub last_login_at: Option<i64>,
}

pub async fn list_users(
    State(s): State<Arc<AppState>>,
    Query(auth): Query<AdminAuth>,
) -> Result<Json<Vec<UserSummary>>, (StatusCode, String)> {
    check(&s, &auth)?;
    let rows = sqlx::query!(
        r#"SELECT id, uid, username, nickname, subscription_tier,
                  created_at, last_login_at
           FROM users ORDER BY created_at DESC LIMIT 200"#)
        .fetch_all(&s.db).await.map_err(internal)?;
    Ok(Json(rows.into_iter().map(|r| UserSummary {
        id: r.id.to_string(),
        uid: r.uid,
        username: r.username,
        nickname: r.nickname,
        tier: r.subscription_tier,
        created_at: r.created_at.timestamp(),
        last_login_at: r.last_login_at.map(|t| t.timestamp()),
    }).collect()))
}

#[derive(Deserialize)]
pub struct PatchUser {
    pub key:       String,
    pub uid:       Option<String>,
    pub username:  Option<String>,
    pub nickname:  Option<String>,
    pub tier:      Option<String>,
    pub tier_expires_at: Option<i64>,
}

pub async fn patch_user(
    State(s): State<Arc<AppState>>,
    Path(id): Path<Uuid>,
    Json(req): Json<PatchUser>,
) -> Result<StatusCode, (StatusCode, String)> {
    check(&s, &AdminAuth { key: Some(req.key) })?;
    if let Some(ref u) = req.uid {
        if !shared_uid::is_valid(u) {
            return Err((StatusCode::BAD_REQUEST, "UID 必须是 7 位数字（不前导 0）".into()));
        }
    }
    sqlx::query!(
        r#"UPDATE users SET
            uid = COALESCE($2, uid),
            username = COALESCE($3, username),
            nickname = COALESCE($4, nickname),
            subscription_tier = COALESCE($5, subscription_tier),
            subscription_expires_at = COALESCE(
                CASE WHEN $6::BIGINT IS NULL THEN NULL ELSE to_timestamp($6) END,
                subscription_expires_at)
           WHERE id=$1"#,
        id, req.uid, req.username, req.nickname, req.tier, req.tier_expires_at)
        .execute(&s.db).await.map_err(internal)?;
    sqlx::query!(
        "INSERT INTO audit_log (actor, action, target, metadata) VALUES ($1, 'admin.patch_user', $2, NULL)",
        "admin", id.to_string())
        .execute(&s.db).await.ok();
    Ok(StatusCode::NO_CONTENT)
}

#[derive(Deserialize)]
pub struct ResetPwReq { pub key: String, pub new_password: String }

pub async fn admin_reset_password(
    State(s): State<Arc<AppState>>,
    Path(id): Path<Uuid>,
    Json(req): Json<ResetPwReq>,
) -> Result<StatusCode, (StatusCode, String)> {
    check(&s, &AdminAuth { key: Some(req.key) })?;
    if req.new_password.len() < 8 {
        return Err((StatusCode::BAD_REQUEST, "password >= 8".into()));
    }
    let h = hashing::hash_password(&req.new_password,
        s.cfg.argon_memory_kib, s.cfg.argon_iterations).map_err(internal)?;
    sqlx::query!(
        "UPDATE users SET password_hash=$1, password_changed_at=now() WHERE id=$2",
        h, id).execute(&s.db).await.map_err(internal)?;
    sqlx::query!("DELETE FROM sessions WHERE user_id=$1", id)
        .execute(&s.db).await.ok();
    sqlx::query!(
        "INSERT INTO audit_log (actor, action, target, metadata) VALUES ('admin', 'admin.reset_password', $1, NULL)",
        id.to_string()).execute(&s.db).await.ok();
    Ok(StatusCode::NO_CONTENT)
}

// ---------------- SSR ----------------
pub struct UserVm {
    pub id: String, pub uid: String, pub username: String, pub nickname: String,
    pub tier: String, pub created: String, pub invite_code: String,
}

#[derive(Template)]
#[template(path = "users_content.html")]
pub struct UsersPage {
    pub title:    String,
    pub subtitle: Option<String>,
    pub host:     &'static str,
    pub route:    &'static str,
    pub users:    Vec<UserVm>,
}

async fn users_page(State(s): State<Arc<AppState>>) -> Html<String> {
    let rows = sqlx::query!(
        r#"SELECT id, uid, username, nickname, subscription_tier, created_at, invite_code_used
           FROM users ORDER BY created_at DESC LIMIT 200"#)
        .fetch_all(&s.db).await.unwrap_or_default();
    let users = rows.into_iter().map(|r| UserVm {
        id: r.id.to_string(),
        uid: r.uid.unwrap_or("—".into()),
        username: r.username.unwrap_or("—".into()),
        nickname: r.nickname.unwrap_or("—".into()),
        tier: r.subscription_tier.unwrap_or("—".into()),
        created: r.created_at.format("%Y-%m-%d %H:%M").to_string(),
        invite_code: r.invite_code_used.unwrap_or("—".into()),
    }).collect();
    ui::render(&UsersPage {
        title: "用户".into(),
        subtitle: None,
        host: ui::host(),
        route: ui::ROUTE_USERS,
        users,
    })
}

#[derive(Deserialize)]
pub struct EditForm {
    pub uid: Option<String>,
    pub username: Option<String>,
    pub nickname: Option<String>,
    pub tier: Option<String>,
}

async fn user_edit_submit(
    State(s): State<Arc<AppState>>,
    Path(id): Path<Uuid>,
    Form(form): Form<EditForm>,
) -> impl IntoResponse {
    let _ = sqlx::query!(
        r#"UPDATE users SET
            uid = COALESCE(NULLIF($2,''), uid),
            username = COALESCE(NULLIF($3,''), username),
            nickname = COALESCE(NULLIF($4,''), nickname),
            subscription_tier = COALESCE(NULLIF($5,''), subscription_tier)
           WHERE id=$1"#,
        id, form.uid, form.username, form.nickname, form.tier)
        .execute(&s.db).await;
    Redirect::to("/admin/users")
}

// SSR 重置密码：表单 POST，强制断开该用户所有 session
#[derive(Deserialize)]
pub struct ResetPwForm { pub new_password: String }

async fn user_reset_pw_form(
    State(s): State<Arc<AppState>>,
    Path(id): Path<Uuid>,
    Form(form): Form<ResetPwForm>,
) -> impl IntoResponse {
    if form.new_password.len() < 8 {
        return Redirect::to("/admin/users?err=pw_too_short");
    }
    let h = match hashing::hash_password(&form.new_password,
        s.cfg.argon_memory_kib, s.cfg.argon_iterations) {
        Ok(v) => v,
        Err(_) => return Redirect::to("/admin/users?err=hash_failed"),
    };
    let _ = sqlx::query!(
        "UPDATE users SET password_hash=$1, password_changed_at=now() WHERE id=$2",
        h, id).execute(&s.db).await;
    let _ = sqlx::query!("DELETE FROM sessions WHERE user_id=$1", id)
        .execute(&s.db).await;
    let _ = sqlx::query!(
        "INSERT INTO audit_log (actor, action, target, metadata) VALUES ('admin','admin.reset_password_form',$1,NULL)",
        id.to_string()).execute(&s.db).await;
    Redirect::to("/admin/users?reset=ok")
}

pub fn routes() -> Router<Arc<AppState>> {
    Router::new()
        .route("/admin/users",                  get(users_page))
        .route("/admin/users/:id/edit",         post(user_edit_submit))
        .route("/admin/users/:id/reset-pw",     post(user_reset_pw_form))
        .route("/api/admin/users",              get(list_users))
        .route("/api/admin/users/:id",          post(patch_user))
        .route("/api/admin/users/:id/reset-pw", post(admin_reset_password))
}
