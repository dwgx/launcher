// 管理员用户管控（SSR + JSON API）。
// 改 UID / Username / Nickname / Tier；重置密码。

use crate::state::AppState;
use crate::ui;
use askama::Template;
use axum::{
    extract::{Form, Json, Path, Query, State},
    http::{HeaderMap, StatusCode},
    response::{IntoResponse, Redirect, Response},
    routing::{get, post},
    Router,
};
use launcher_shared::{hashing, uid as shared_uid};
use serde::{Deserialize, Serialize};
use std::sync::Arc;
use uuid::Uuid;

// ---------------- gate ----------------
#[derive(Deserialize)]
pub struct AdminAuth {
    pub key: Option<String>,
}

use crate::error::internal;

// ---------------- JSON API ----------------
#[derive(Serialize)]
pub struct UserSummary {
    pub id: String,
    pub uid: Option<String>,
    pub username: Option<String>,
    pub nickname: Option<String>,
    pub tier: Option<String>,
    pub role: String,
    pub role_label: Option<String>,
    pub is_admin: bool,
    pub created_at: i64,
    pub last_login_at: Option<i64>,
}

pub async fn list_users(
    State(s): State<Arc<AppState>>,
    headers: HeaderMap,
    Query(auth): Query<AdminAuth>,
) -> Result<Json<Vec<UserSummary>>, (StatusCode, String)> {
    let _actor = crate::admin_customization::require_actor_or_admin_key(
        &headers,
        &s,
        auth.key.as_deref(),
        "admin.users.read",
    )
    .await?;
    let rows = sqlx::query!(
        r#"SELECT id, uid, username, nickname, subscription_tier, role, role_label, is_admin,
                  created_at, last_login_at
           FROM users ORDER BY created_at DESC LIMIT 200"#
    )
    .fetch_all(&s.db)
    .await
    .map_err(internal)?;
    Ok(Json(
        rows.into_iter()
            .map(|r| UserSummary {
                id: r.id.to_string(),
                uid: r.uid,
                username: r.username,
                nickname: r.nickname,
                tier: r.subscription_tier,
                role: r.role,
                role_label: r.role_label,
                is_admin: r.is_admin,
                created_at: r.created_at.timestamp(),
                last_login_at: r.last_login_at.map(|t| t.timestamp()),
            })
            .collect(),
    ))
}

#[derive(Deserialize)]
pub struct PatchUser {
    pub key: Option<String>,
    pub uid: Option<String>,
    pub username: Option<String>,
    pub nickname: Option<String>,
    pub tier: Option<String>,
    pub tier_expires_at: Option<i64>,
    pub role: Option<String>,
    pub role_label: Option<String>,
    pub is_admin: Option<bool>,
}

pub async fn patch_user(
    State(s): State<Arc<AppState>>,
    headers: HeaderMap,
    Path(id): Path<Uuid>,
    Json(req): Json<PatchUser>,
) -> Result<StatusCode, (StatusCode, String)> {
    let actor = crate::admin_customization::require_actor_or_admin_key(
        &headers,
        &s,
        req.key.as_deref(),
        "admin.users.manage",
    )
    .await?;
    if let Some(ref u) = req.uid {
        if !shared_uid::is_valid(u) {
            return Err((
                StatusCode::BAD_REQUEST,
                "UID 必须是 7 位数字（不前导 0）".into(),
            ));
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
                subscription_expires_at),
            role = COALESCE($7, role),
            role_label = COALESCE($8, role_label),
            is_admin = COALESCE($9, CASE WHEN $7 IN ('admin','owner','super_admin') THEN TRUE ELSE is_admin END)
           WHERE id=$1"#,
        id, req.uid, req.username, req.nickname, req.tier, req.tier_expires_at,
        req.role, req.role_label, req.is_admin)
        .execute(&s.db).await.map_err(internal)?;
    sqlx::query!(
        "INSERT INTO audit_log (actor, action, target, metadata) VALUES ($1, 'admin.patch_user', $2, NULL)",
        &actor.name, id.to_string())
        .execute(&s.db).await.ok();
    crate::audit::event(&actor.name, "admin.patch_user", &id.to_string());
    Ok(StatusCode::NO_CONTENT)
}

#[derive(Deserialize)]
pub struct ResetPwReq {
    pub key: Option<String>,
    pub new_password: String,
}

pub async fn admin_reset_password(
    State(s): State<Arc<AppState>>,
    headers: HeaderMap,
    Path(id): Path<Uuid>,
    Json(req): Json<ResetPwReq>,
) -> Result<StatusCode, (StatusCode, String)> {
    let actor = crate::admin_customization::require_actor_or_admin_key(
        &headers,
        &s,
        req.key.as_deref(),
        "admin.users.manage",
    )
    .await?;
    if req.new_password.len() < 8 {
        return Err((StatusCode::BAD_REQUEST, "password >= 8".into()));
    }
    let h = hashing::hash_password(
        &req.new_password,
        s.cfg.argon_memory_kib,
        s.cfg.argon_iterations,
    )
    .map_err(internal)?;
    sqlx::query!(
        "UPDATE users SET password_hash=$1, password_changed_at=now() WHERE id=$2",
        h,
        id
    )
    .execute(&s.db)
    .await
    .map_err(internal)?;
    sqlx::query!("DELETE FROM sessions WHERE user_id=$1", id)
        .execute(&s.db)
        .await
        .ok();
    sqlx::query!(
        "INSERT INTO audit_log (actor, action, target, metadata) VALUES ($1, 'admin.reset_password', $2, NULL)",
        &actor.name, id.to_string()).execute(&s.db).await.ok();
    crate::audit::event(&actor.name, "admin.reset_password", &id.to_string());
    Ok(StatusCode::NO_CONTENT)
}

// ---------------- SSR ----------------
pub struct UserVm {
    pub id: String,
    pub uid: String,
    pub username: String,
    pub nickname: String,
    pub tier: String,
    pub created: String,
    pub invite_code: String,
    pub role: String,
    pub role_label: String, // 0009 加的字段
    pub hwid_bound: bool,    // 是否已绑定 HWID
    pub hwid_fp: String,     // 最近一次登录的 HWID 指纹（hex 前 16 位），未知显 —
    pub hwid_changed: String, // hwid_last_changed_at，未变显 —
}
pub struct RoleVm {
    pub role: String,
    pub label_zh: String,
}

#[derive(Template)]
#[template(path = "users_content.html")]
pub struct UsersPage {
    pub title: String,
    pub subtitle: Option<String>,
    pub notice: Option<ui::AdminNotice>,
    pub host: &'static str,
    pub route: &'static str,
    pub users: Vec<UserVm>,
    pub roles: Vec<RoleVm>,
    pub search: String,    // 回显搜索框，无则空串
    pub page: i64,         // 当前页（从 1 开始）
    pub total_pages: i64,  // 总页数（至少 1）
    pub total: i64,        // 匹配的用户总数
}

#[derive(Deserialize, Default)]
pub struct UsersNoticeQuery {
    pub reset: Option<String>,
    pub delete: Option<String>,
    pub hwid: Option<String>,
    pub err: Option<String>,
    pub q: Option<String>,     // 搜索词（UID / 用户名 / 昵称）
    pub page: Option<i64>,     // 页码，从 1 开始
}

fn users_notice(q: &UsersNoticeQuery) -> Option<ui::AdminNotice> {
    match (q.reset.as_deref(), q.delete.as_deref(), q.err.as_deref()) {
        (Some("ok"), _, _) => Some(ui::AdminNotice::success(
            "密码已重置，用户所有 session 已失效",
        )),
        (_, Some("ok"), _) => Some(ui::AdminNotice::success("用户已删除")),
        (_, _, Some("pw_too_short")) => Some(ui::AdminNotice::error("新密码至少需要 8 个字符")),
        (_, _, Some("hash_failed")) => Some(ui::AdminNotice::error("密码哈希失败，请重试")),
        (_, _, Some("user_not_found")) => Some(ui::AdminNotice::error("用户不存在或已被删除")),
        (_, _, Some("delete_confirm_mismatch")) => {
            Some(ui::AdminNotice::warning("删除确认用户名不一致，操作已取消"))
        }
        (_, _, Some("delete_failed")) => Some(ui::AdminNotice::error("删除失败，请查看服务日志")),
        _ if q.hwid.as_deref() == Some("ok") => {
            Some(ui::AdminNotice::success("HWID 已重置，该用户下次登录将自动绑定新机器"))
        }
        _ => None,
    }
}

async fn users_page(
    State(s): State<Arc<AppState>>,
    headers: HeaderMap,
    Query(q): Query<UsersNoticeQuery>,
) -> Response {
    if let Err(resp) =
        crate::admin_customization::require_actor(&headers, &s, "admin.users.read").await
    {
        return resp;
    }

    // 搜索词：trim 后为空当 None，避免无意义过滤
    let search: Option<String> = q
        .q
        .as_deref()
        .map(str::trim)
        .filter(|s| !s.is_empty())
        .map(|s| s.to_string());

    const PER_PAGE: i64 = 50;
    // 总数（同样的 WHERE 条件），用于算总页数
    let total: i64 = sqlx::query!(
        r#"SELECT COUNT(*) AS "count!" FROM users u
           WHERE ($1::text IS NULL
                  OR u.username ILIKE '%'||$1||'%'
                  OR u.nickname ILIKE '%'||$1||'%'
                  OR u.uid ILIKE '%'||$1||'%')"#,
        search.as_deref())
        .fetch_one(&s.db)
        .await
        .map(|r| r.count)
        .unwrap_or(0);

    let total_pages = if total <= 0 { 1 } else { (total + PER_PAGE - 1) / PER_PAGE };
    // page 边界保护：>=1 且 <= total_pages
    let page = q.page.unwrap_or(1).max(1).min(total_pages);
    let offset = (page - 1) * PER_PAGE;

    let rows = sqlx::query!(
        r#"SELECT u.id, u.uid, u.username, u.nickname, u.subscription_tier, u.created_at,
                  u.invite_code_used, u.role, u.role_label,
                  u.hwid_bound, u.hwid_last_changed_at,
                  (SELECT lh.hwid_short FROM login_history lh
                     WHERE lh.user_id = u.id AND lh.success = true
                     ORDER BY lh.occurred_at DESC LIMIT 1) AS hwid_fp
           FROM users u
           WHERE ($1::text IS NULL
                  OR u.username ILIKE '%'||$1||'%'
                  OR u.nickname ILIKE '%'||$1||'%'
                  OR u.uid ILIKE '%'||$1||'%')
           ORDER BY u.created_at DESC LIMIT $2 OFFSET $3"#,
        search.as_deref(), PER_PAGE, offset)
        .fetch_all(&s.db).await.unwrap_or_default();
    let users = rows
        .into_iter()
        .map(|r| UserVm {
            id: r.id.to_string(),
            uid: r.uid.unwrap_or("—".into()),
            username: r.username.unwrap_or("—".into()),
            nickname: r.nickname.unwrap_or("—".into()),
            tier: r.subscription_tier.unwrap_or("—".into()),
            created: r.created_at.format("%Y-%m-%d %H:%M").to_string(),
            invite_code: r.invite_code_used.unwrap_or("—".into()),
            role: r.role,
            role_label: r.role_label.unwrap_or("—".into()),
            hwid_bound: r.hwid_bound.is_some(),
            hwid_fp: r.hwid_fp.unwrap_or("—".into()),
            hwid_changed: r
                .hwid_last_changed_at
                .map(|t| t.format("%Y-%m-%d %H:%M").to_string())
                .unwrap_or("—".into()),
        })
        .collect();
    let role_rows =
        sqlx::query!(r#"SELECT role, label_zh FROM user_roles_catalog ORDER BY sort_order"#)
            .fetch_all(&s.db)
            .await
            .unwrap_or_default();
    let roles = role_rows
        .into_iter()
        .map(|r| RoleVm {
            role: r.role,
            label_zh: r.label_zh,
        })
        .collect();
    ui::render(&UsersPage {
        title: "用户".into(),
        subtitle: None,
        notice: users_notice(&q),
        host: ui::host(),
        route: ui::ROUTE_USERS,
        users,
        roles,
        search: search.unwrap_or_default(),
        page,
        total_pages,
        total,
    })
    .into_response()
}

#[derive(Deserialize)]
pub struct EditForm {
    pub uid: Option<String>,
    pub username: Option<String>,
    pub nickname: Option<String>,
    pub tier: Option<String>,
    pub role: Option<String>,
    pub role_label: Option<String>,
}

async fn user_edit_submit(
    State(s): State<Arc<AppState>>,
    headers: HeaderMap,
    Path(id): Path<Uuid>,
    Form(form): Form<EditForm>,
) -> Response {
    let actor =
        match crate::admin_customization::require_actor(&headers, &s, "admin.users.manage").await {
            Ok(v) => v,
            Err(resp) => return resp,
        };

    let _ = sqlx::query!(
        r#"UPDATE users SET
            uid = COALESCE(NULLIF($2,''), uid),
            username = COALESCE(NULLIF($3,''), username),
            nickname = COALESCE(NULLIF($4,''), nickname),
            subscription_tier = COALESCE(NULLIF($5,''), subscription_tier),
            role = COALESCE(NULLIF($6,''), role),
            role_label = COALESCE(NULLIF($7,''), role_label),
            is_admin = CASE WHEN $6 IN ('admin','owner','super_admin') THEN TRUE
                            WHEN $6 IS NOT NULL AND $6 != '' THEN FALSE
                            ELSE is_admin END
           WHERE id=$1"#,
        id,
        form.uid,
        form.username,
        form.nickname,
        form.tier,
        form.role,
        form.role_label
    )
    .execute(&s.db)
    .await;
    let _ = sqlx::query!(
        "INSERT INTO audit_log (actor, action, target, metadata) VALUES ($1,'admin.edit_user',$2,NULL)",
        &actor.name, id.to_string()).execute(&s.db).await;
    crate::audit::event(&actor.name, "admin.edit_user", &id.to_string());
    Redirect::to("/admin/users").into_response()
}

// SSR 重置密码：表单 POST，强制断开该用户所有 session
#[derive(Deserialize)]
pub struct ResetPwForm {
    pub new_password: String,
}

async fn user_reset_pw_form(
    State(s): State<Arc<AppState>>,
    headers: HeaderMap,
    Path(id): Path<Uuid>,
    Form(form): Form<ResetPwForm>,
) -> Response {
    let actor =
        match crate::admin_customization::require_actor(&headers, &s, "admin.users.manage").await {
            Ok(v) => v,
            Err(resp) => return resp,
        };

    if form.new_password.len() < 8 {
        return Redirect::to("/admin/users?err=pw_too_short").into_response();
    }
    let h = match hashing::hash_password(
        &form.new_password,
        s.cfg.argon_memory_kib,
        s.cfg.argon_iterations,
    ) {
        Ok(v) => v,
        Err(_) => return Redirect::to("/admin/users?err=hash_failed").into_response(),
    };
    let _ = sqlx::query!(
        "UPDATE users SET password_hash=$1, password_changed_at=now() WHERE id=$2",
        h,
        id
    )
    .execute(&s.db)
    .await;
    let _ = sqlx::query!("DELETE FROM sessions WHERE user_id=$1", id)
        .execute(&s.db)
        .await;
    let _ = sqlx::query!(
        "INSERT INTO audit_log (actor, action, target, metadata) VALUES ($1,'admin.reset_password_form',$2,NULL)",
        &actor.name, id.to_string()).execute(&s.db).await;
    crate::audit::event(&actor.name, "admin.reset_password_form", &id.to_string());
    Redirect::to("/admin/users?reset=ok").into_response()
}

// SSR 重置 HWID：把 hwid_bound 置空，下次任意机器登录自动重绑。
// 用于用户换硬件/重装后管理员手动放行（不走 rebind 审批流时的快捷操作）。
async fn user_reset_hwid_form(
    State(s): State<Arc<AppState>>,
    headers: HeaderMap,
    Path(id): Path<Uuid>,
) -> Response {
    let actor =
        match crate::admin_customization::require_actor(&headers, &s, "admin.users.manage").await {
            Ok(v) => v,
            Err(resp) => return resp,
        };

    let _ = sqlx::query!(
        "UPDATE users SET hwid_bound = NULL, hwid_last_changed_at = now() WHERE id = $1",
        id
    )
    .execute(&s.db)
    .await;
    let _ = sqlx::query!(
        "INSERT INTO audit_log (actor, action, target, metadata) VALUES ($1,'admin.reset_hwid',$2,NULL)",
        &actor.name, id.to_string()).execute(&s.db).await;
    crate::audit::event(&actor.name, "admin.reset_hwid", &id.to_string());
    Redirect::to("/admin/users?hwid=ok").into_response()
}

// =====================================================================
// 删除用户 — admin 双重确认
// 安全设计：
//   1. SSR dialog "type-to-confirm"：必须输入用户名一致 + JS confirm()
//   2. 后端再校验 confirm_username 字段必须等于 DB 里的 username（不一致 → reject）
//   3. 解除外键引用：messages.sender_id / stickers.creator_id / sticker_packs.creator_id
//      / media_files.uploader_id 一律 SET NULL（保留内容）
//   4. 级联删 = sessions / login_history / hwid_rebind_requests / heartbeats
//      / user_avatar_meta / chat_members / message_reactions / user_sticker_packs
//      / user_tags / market_listings / market_orders / market_reviews / credit_ledger
//      / invite_code_uses / subscriptions（FK 多数已 ON DELETE CASCADE，但显式 DELETE 保险）
//   5. 最后 DELETE FROM users
// =====================================================================
#[derive(Deserialize)]
pub struct DeleteForm {
    pub confirm_username: String,
}

async fn user_delete_submit(
    State(s): State<Arc<AppState>>,
    headers: HeaderMap,
    Path(id): Path<Uuid>,
    Form(form): Form<DeleteForm>,
) -> Response {
    let actor =
        match crate::admin_customization::require_actor(&headers, &s, "admin.users.manage").await {
            Ok(v) => v,
            Err(resp) => return resp,
        };

    // 第一道：拿真 username 跟客户端输入比对（防误删）
    let actual = sqlx::query_scalar!("SELECT username FROM users WHERE id=$1", id)
        .fetch_optional(&s.db)
        .await
        .ok()
        .flatten()
        .flatten();
    let actual = match actual {
        Some(u) => u,
        None => return Redirect::to("/admin/users?err=user_not_found").into_response(),
    };
    if actual != form.confirm_username.trim() {
        return Redirect::to("/admin/users?err=delete_confirm_mismatch").into_response();
    }

    let res = async {
        let mut tx = s.db.begin().await?;

        // 解除引用 (SET NULL) — 保留内容但去除作者
        sqlx::query!("UPDATE messages SET sender_id=NULL WHERE sender_id=$1", id)
            .execute(&mut *tx).await?;
        sqlx::query!("UPDATE stickers SET creator_id=NULL WHERE creator_id=$1", id)
            .execute(&mut *tx).await?;
        sqlx::query!("UPDATE sticker_packs SET creator_id=NULL WHERE creator_id=$1", id)
            .execute(&mut *tx).await?;
        sqlx::query!("UPDATE media_files SET uploader_id=NULL WHERE uploader_id=$1", id)
            .execute(&mut *tx).await?;

        // 删除引用了该用户的强关联表（FK 多数 CASCADE，仍显式 DELETE 防 schema 漂移）
        sqlx::query!("DELETE FROM sessions WHERE user_id=$1", id).execute(&mut *tx).await?;
        sqlx::query!("DELETE FROM user_tags WHERE user_id=$1", id).execute(&mut *tx).await?;
        sqlx::query!("DELETE FROM user_sticker_packs WHERE user_id=$1", id).execute(&mut *tx).await?;

        // 写审计日志（用户已经要被删，这里 actor=admin / target=被删 user 的 id+username）
        sqlx::query!(
            "INSERT INTO audit_log (actor, action, target, metadata) VALUES ($1,'admin.delete_user',$2,$3)",
            &actor.name, id.to_string(), serde_json::json!({"username": &actual}))
            .execute(&mut *tx).await?;

        // 真删
        sqlx::query!("DELETE FROM users WHERE id=$1", id).execute(&mut *tx).await?;
        tx.commit().await
    }.await;
    if let Err(e) = res {
        tracing::error!("delete user failed: {}", e);
        return Redirect::to("/admin/users?err=delete_failed").into_response();
    }
    crate::audit::event(&actor.name, "admin.delete_user", &id.to_string());
    Redirect::to("/admin/users?delete=ok").into_response()
}

pub fn routes() -> Router<Arc<AppState>> {
    Router::new()
        .route("/admin/users", get(users_page))
        .route("/admin/users/:id/edit", post(user_edit_submit))
        .route("/admin/users/:id/reset-pw", post(user_reset_pw_form))
        .route("/admin/users/:id/reset-hwid", post(user_reset_hwid_form))
        .route("/admin/users/:id/delete", post(user_delete_submit))
        .route("/api/admin/users", get(list_users))
        .route("/api/admin/users/:id", post(patch_user))
        .route("/api/admin/users/:id/reset-pw", post(admin_reset_password))
}
