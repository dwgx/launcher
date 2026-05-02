// HWID 重绑定：用户换硬件后走这里，管理员审批后才能登录。
//
// POST /api/hwid/rebind/request - 客户端发起，需 session token
// GET  /api/hwid/rebind/list    - 用户查自己历史
// 管理面板：
//   GET  /admin/rebind         - pending 列表
//   POST /admin/rebind/{id}/approve
//   POST /admin/rebind/{id}/deny

use crate::state::AppState;
use axum::{extract::{State, Path, Json, Query}, http::StatusCode};
use serde::{Deserialize, Serialize};
use serde_json::Value as JsonValue;
use std::sync::Arc;
use chrono::Utc;
use uuid::Uuid;

#[derive(Deserialize)]
pub struct RebindReq {
    pub session_token:    String,
    pub old_fingerprint:  Option<String>,
    pub new_fingerprint:  String,
    pub parts_full:       JsonValue,    // {part_name: value}
    pub parts_diff:       JsonValue,    // {part_name: {old, new}}
    pub user_reason:      Option<String>,
}

#[derive(Serialize)]
pub struct RebindResp {
    pub request_id: String,
    pub status:     String,
}

pub async fn submit(
    State(s): State<Arc<AppState>>,
    Json(req): Json<RebindReq>,
) -> Result<Json<RebindResp>, (StatusCode, String)> {
    let user_id: Uuid = sqlx::query_scalar!(
        "SELECT user_id FROM sessions WHERE token = $1",
        req.session_token)
        .fetch_optional(&s.db).await
        .map_err(internal)?
        .ok_or((StatusCode::UNAUTHORIZED, "no session".into()))?;

    let id: Uuid = sqlx::query_scalar!(
        r#"INSERT INTO hwid_rebind_requests
              (user_id, old_fingerprint, new_fingerprint,
               parts_diff, parts_full, user_reason)
           VALUES ($1, $2, $3, $4, $5, $6)
           RETURNING id"#,
        user_id, req.old_fingerprint, req.new_fingerprint,
        req.parts_diff, req.parts_full, req.user_reason)
        .fetch_one(&s.db).await
        .map_err(internal)?;

    sqlx::query!(
        r#"INSERT INTO audit_log (actor, action, target, metadata)
           VALUES ($1, 'hwid_rebind.submit', $2, $3)"#,
        user_id.to_string(),
        id.to_string(),
        serde_json::json!({"new_fingerprint": req.new_fingerprint}))
        .execute(&s.db).await.ok();

    Ok(Json(RebindResp {
        request_id: id.to_string(),
        status: "pending".into(),
    }))
}

#[derive(Deserialize)]
pub struct ListQuery { pub session_token: String }

#[derive(Serialize)]
pub struct RebindRow {
    pub id:              String,
    pub status:          String,
    pub submitted_at:    i64,
    pub reviewed_at:     Option<i64>,
    pub user_reason:     Option<String>,
    pub review_note:     Option<String>,
}

pub async fn list_for_user(
    State(s): State<Arc<AppState>>,
    Query(q): Query<ListQuery>,
) -> Result<Json<Vec<RebindRow>>, (StatusCode, String)> {
    let user_id: Uuid = sqlx::query_scalar!(
        "SELECT user_id FROM sessions WHERE token = $1 AND expires_at > now()",
        q.session_token)
        .fetch_optional(&s.db).await
        .map_err(internal)?
        .ok_or((StatusCode::UNAUTHORIZED, "no session".into()))?;

    let rows = sqlx::query!(
        r#"SELECT id, status, submitted_at, reviewed_at, user_reason, review_note
           FROM hwid_rebind_requests
           WHERE user_id = $1
           ORDER BY submitted_at DESC LIMIT 50"#,
        user_id)
        .fetch_all(&s.db).await
        .map_err(internal)?;

    Ok(Json(rows.into_iter().map(|r| RebindRow {
        id: r.id.to_string(),
        status: r.status,
        submitted_at: r.submitted_at.timestamp(),
        reviewed_at: r.reviewed_at.map(|t| t.timestamp()),
        user_reason: r.user_reason,
        review_note: r.review_note,
    }).collect()))
}

#[derive(Deserialize)]
pub struct ReviewBody { pub review_note: Option<String> }

pub async fn admin_approve(
    State(s): State<Arc<AppState>>,
    Path(id): Path<Uuid>,
    Json(body): Json<ReviewBody>,
) -> Result<Json<RebindResp>, (StatusCode, String)> {
    let row = sqlx::query!(
        r#"UPDATE hwid_rebind_requests
           SET status='approved', reviewed_at=now(), reviewer='admin', review_note=$2
           WHERE id=$1 AND status='pending'
           RETURNING user_id, new_fingerprint"#,
        id, body.review_note)
        .fetch_optional(&s.db).await
        .map_err(internal)?
        .ok_or((StatusCode::NOT_FOUND, "no pending request".into()))?;

    // 同时把用户的 hwid_bound 改成新 fingerprint，下次登录通过
    sqlx::query!(
        "UPDATE users SET hwid_bound = $1, hwid_last_changed_at = $2 WHERE id = $3",
        row.new_fingerprint, Utc::now(), row.user_id)
        .execute(&s.db).await.map_err(internal)?;

    sqlx::query!(
        "INSERT INTO audit_log (actor, action, target, metadata) VALUES ($1, 'hwid_rebind.approve', $2, $3)",
        "admin", id.to_string(),
        serde_json::json!({"user_id": row.user_id.to_string()}))
        .execute(&s.db).await.ok();

    Ok(Json(RebindResp { request_id: id.to_string(), status: "approved".into() }))
}

pub async fn admin_deny(
    State(s): State<Arc<AppState>>,
    Path(id): Path<Uuid>,
    Json(body): Json<ReviewBody>,
) -> Result<Json<RebindResp>, (StatusCode, String)> {
    let updated = sqlx::query!(
        r#"UPDATE hwid_rebind_requests
           SET status='denied', reviewed_at=now(), reviewer='admin', review_note=$2
           WHERE id=$1 AND status='pending'"#,
        id, body.review_note)
        .execute(&s.db).await
        .map_err(internal)?
        .rows_affected();
    if updated == 0 { return Err((StatusCode::NOT_FOUND, "no pending request".into())); }

    sqlx::query!(
        "INSERT INTO audit_log (actor, action, target, metadata) VALUES ($1, 'hwid_rebind.deny', $2, NULL)",
        "admin", id.to_string())
        .execute(&s.db).await.ok();

    Ok(Json(RebindResp { request_id: id.to_string(), status: "denied".into() }))
}

fn internal<E: std::fmt::Display>(e: E) -> (StatusCode, String) {
    (StatusCode::INTERNAL_SERVER_ERROR, e.to_string())
}
