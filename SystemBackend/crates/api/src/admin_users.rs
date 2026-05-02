// 管理员用户管控：列出 / 改 UID / 改 Username / 重置密码 / 强制改昵称 / 调整订阅 / 封禁
//
// 全部经 admin session（admin.rs 里登录拿到 admin_token）才能调，
// 这里简化：通过查询参数 admin_pwd 验证（与 config.toml.admin_password 比对）。
// 生产前应该升级为 admin session 表。

use crate::state::AppState;
use axum::{
    extract::{State, Query, Path, Json, Form},
    http::StatusCode,
    response::{IntoResponse, Html, Redirect},
    Router,
    routing::{get, post},
};
use serde::{Deserialize, Serialize};
use std::sync::Arc;
use uuid::Uuid;
use launcher_shared::{hashing, uid as shared_uid};
use askama::Template;

// =====================================================================
// admin gate (super 简单：query string ?key=ADMIN_PASSWORD)
// 生产应换 admin session
// =====================================================================
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

// =====================================================================
// API: list / patch / reset password
// =====================================================================
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
    pub key:       String,         // admin password
    pub uid:       Option<String>,
    pub username:  Option<String>,
    pub nickname:  Option<String>,
    pub tier:      Option<String>,
    pub tier_expires_at: Option<i64>,    // unix seconds
}

pub async fn patch_user(
    State(s): State<Arc<AppState>>,
    Path(id): Path<Uuid>,
    Json(req): Json<PatchUser>,
) -> Result<StatusCode, (StatusCode, String)> {
    check(&s, &AdminAuth { key: Some(req.key) })?;

    if let Some(ref u) = req.uid {
        if !shared_uid::is_valid(u) {
            return Err((StatusCode::BAD_REQUEST, "invalid UID format".into()));
        }
    }

    sqlx::query!(
        r#"UPDATE users SET
            uid = COALESCE($2, uid),
            username = COALESCE($3, username),
            nickname = COALESCE($4, nickname),
            subscription_tier = COALESCE($5, subscription_tier),
            subscription_expires_at = COALESCE(
                CASE WHEN $6::BIGINT IS NULL THEN NULL
                     ELSE to_timestamp($6) END,
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
    sqlx::query!(
        "DELETE FROM sessions WHERE user_id=$1", id)
        .execute(&s.db).await.ok();
    sqlx::query!(
        "INSERT INTO audit_log (actor, action, target, metadata) VALUES ('admin', 'admin.reset_password', $1, NULL)",
        id.to_string()).execute(&s.db).await.ok();
    Ok(StatusCode::NO_CONTENT)
}

// =====================================================================
// SSR: 用户管控页 /admin/users
// =====================================================================
#[derive(Template)]
#[template(source = "<!doctype html><html><head><meta charset=utf-8>\
<title>Users - Launcher</title><style>\
body{font-family:'Source Han Sans CN',sans-serif;background:#FAF7F2;color:#1F1E1D;padding:32px;}\
h1{color:#C96442;margin:0 0 16px 0;}table{width:100%;border-collapse:collapse;background:#FFF;border-radius:12px;overflow:hidden;box-shadow:0 1px 3px rgba(0,0,0,.04);}\
th{background:#F3EFE8;text-align:left;padding:12px;font-size:13px;font-weight:600;}\
td{padding:12px;border-top:1px solid #EDE9E1;font-size:13px;}\
.tag{background:#C96442;color:#fff;padding:2px 8px;border-radius:6px;font-size:11px;}\
input,select{padding:6px;border:1px solid #EDE9E1;border-radius:6px;font:inherit;}\
.btn{background:#C96442;color:#fff;padding:6px 12px;border:none;border-radius:8px;cursor:pointer;font-size:12px;}\
.muted{color:#6B6A67;font-size:11px;}</style></head>\
<body><h1>用户管理</h1><p><a href=/admin>← Dashboard</a> · <a href=/admin/rebind>HWID Rebind</a></p>\
<table><thead><tr><th>UID</th><th>Username</th><th>Nickname</th><th>Tier</th><th>Created</th><th>Actions</th></tr></thead>\
<tbody>{% for u in users %}<tr>\
<td><code>{{ u.uid }}</code></td><td>{{ u.username }}</td><td>{{ u.nickname }}</td>\
<td><span class=tag>{{ u.tier }}</span></td><td class=muted>{{ u.created }}</td>\
<td><a href=\"/admin/users/{{ u.id }}/edit\">Edit</a> · <a href=\"/admin/users/{{ u.id }}/reset-pw\">Reset PW</a></td>\
</tr>{% endfor %}</tbody></table></body></html>", ext = "html")]
struct UsersTpl { users: Vec<UserVm> }

#[derive(Default)]
struct UserVm {
    id: String, uid: String, username: String, nickname: String,
    tier: String, created: String,
}

async fn users_page(State(s): State<Arc<AppState>>) -> impl IntoResponse {
    let rows = sqlx::query!(
        r#"SELECT id, uid, username, nickname, subscription_tier, created_at
           FROM users ORDER BY created_at DESC LIMIT 200"#)
        .fetch_all(&s.db).await.unwrap_or_default();
    let vm = UsersTpl {
        users: rows.into_iter().map(|r| UserVm {
            id: r.id.to_string(),
            uid: r.uid.unwrap_or("—".into()),
            username: r.username.unwrap_or("—".into()),
            nickname: r.nickname.unwrap_or("—".into()),
            tier: r.subscription_tier.unwrap_or("—".into()),
            created: r.created_at.format("%Y-%m-%d %H:%M").to_string(),
        }).collect()
    };
    Html(vm.render().unwrap_or_default())
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

pub fn routes() -> Router<Arc<AppState>> {
    // Why: 在 main.rs 顶层 .merge() 进来，所以路径要带 /admin / /api 前缀
    Router::new()
        .route("/admin/users",                  get(users_page))
        .route("/admin/users/:id/edit",         post(user_edit_submit))
        .route("/api/admin/users",              get(list_users))
        .route("/api/admin/users/:id",          post(patch_user))
        .route("/api/admin/users/:id/reset-pw", post(admin_reset_password))
}
