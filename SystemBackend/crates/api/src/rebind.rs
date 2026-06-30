// HWID 重绑定：用户换硬件后走这里，管理员审批后才能登录。
//
// POST /api/hwid/rebind/request - 客户端发起，需 session token
// GET  /api/hwid/rebind/list    - 用户查自己历史
// 管理面板：
//   GET  /admin/rebind         - pending 列表
//   POST /admin/rebind/{id}/approve
//   POST /admin/rebind/{id}/deny

use crate::state::AppState;
use crate::ui;
use askama::Template;
use axum::{
    extract::{Json, Path, Query, State},
    http::{HeaderMap, StatusCode},
    response::{IntoResponse, Response},
    routing::{get, post},
    Router,
};
use serde::{Deserialize, Serialize};
use serde_json::Value as JsonValue;
use std::sync::Arc;
use uuid::Uuid;

#[derive(Deserialize)]
pub struct RebindReq {
    pub session_token: String,
    pub old_fingerprint: Option<String>,
    pub new_fingerprint: String,
    pub parts_full: JsonValue, // {part_name: value}
    pub parts_diff: JsonValue, // {part_name: {old, new}}
    pub user_reason: Option<String>,
}

#[derive(Serialize)]
pub struct RebindResp {
    pub request_id: String,
    pub status: String,
}

pub async fn submit(
    State(s): State<Arc<AppState>>,
    Json(req): Json<RebindReq>,
) -> Result<Json<RebindResp>, (StatusCode, String)> {
    let user_id: Uuid = sqlx::query_scalar!(
        "SELECT user_id FROM sessions WHERE token = $1",
        req.session_token
    )
    .fetch_optional(&s.db)
    .await
    .map_err(internal)?
    .ok_or((StatusCode::UNAUTHORIZED, "no session".into()))?;

    let id: Uuid = sqlx::query_scalar!(
        r#"INSERT INTO hwid_rebind_requests
              (user_id, old_fingerprint, new_fingerprint,
               parts_diff, parts_full, user_reason)
           VALUES ($1, $2, $3, $4, $5, $6)
           RETURNING id"#,
        user_id,
        req.old_fingerprint,
        req.new_fingerprint,
        req.parts_diff,
        req.parts_full,
        req.user_reason
    )
    .fetch_one(&s.db)
    .await
    .map_err(internal)?;

    sqlx::query!(
        r#"INSERT INTO audit_log (actor, action, target, metadata)
           VALUES ($1, 'hwid_rebind.submit', $2, $3)"#,
        user_id.to_string(),
        id.to_string(),
        serde_json::json!({"new_fingerprint": req.new_fingerprint})
    )
    .execute(&s.db)
    .await
    .ok();

    Ok(Json(RebindResp {
        request_id: id.to_string(),
        status: "pending".into(),
    }))
}

#[derive(Deserialize)]
pub struct ListQuery {
    pub session_token: String,
}

#[derive(Serialize)]
pub struct RebindRow {
    pub id: String,
    pub status: String,
    pub submitted_at: i64,
    pub reviewed_at: Option<i64>,
    pub user_reason: Option<String>,
    pub review_note: Option<String>,
}

pub async fn list_for_user(
    State(s): State<Arc<AppState>>,
    Query(q): Query<ListQuery>,
) -> Result<Json<Vec<RebindRow>>, (StatusCode, String)> {
    let user_id: Uuid = sqlx::query_scalar!(
        "SELECT user_id FROM sessions WHERE token = $1 AND expires_at > now()",
        q.session_token
    )
    .fetch_optional(&s.db)
    .await
    .map_err(internal)?
    .ok_or((StatusCode::UNAUTHORIZED, "no session".into()))?;

    let rows = sqlx::query!(
        r#"SELECT id, status, submitted_at, reviewed_at, user_reason, review_note
           FROM hwid_rebind_requests
           WHERE user_id = $1
           ORDER BY submitted_at DESC LIMIT 50"#,
        user_id
    )
    .fetch_all(&s.db)
    .await
    .map_err(internal)?;

    Ok(Json(
        rows.into_iter()
            .map(|r| RebindRow {
                id: r.id.to_string(),
                status: r.status,
                submitted_at: r.submitted_at.timestamp(),
                reviewed_at: r.reviewed_at.map(|t| t.timestamp()),
                user_reason: r.user_reason,
                review_note: r.review_note,
            })
            .collect(),
    ))
}

#[derive(Deserialize)]
pub struct ReviewBody {
    pub key: Option<String>,
    pub review_note: Option<String>,
}

pub async fn admin_approve(
    State(s): State<Arc<AppState>>,
    headers: HeaderMap,
    Path(id): Path<Uuid>,
    Json(body): Json<ReviewBody>,
) -> Result<Json<RebindResp>, (StatusCode, String)> {
    let actor = crate::admin_customization::require_actor_or_admin_key(
        &headers,
        &s,
        body.key.as_deref(),
        "admin.rebind.manage",
    )
    .await?;
    let row = sqlx::query!(
        r#"UPDATE hwid_rebind_requests
           SET status='approved', reviewed_at=now(), reviewer=$2, review_note=$3
           WHERE id=$1 AND status='pending'
           RETURNING user_id, new_fingerprint"#,
        id,
        &actor.name,
        body.review_note
    )
    .fetch_optional(&s.db)
    .await
    .map_err(internal)?
    .ok_or((StatusCode::NOT_FOUND, "no pending request".into()))?;

    // 同时把用户的 hwid_bound 改成新 fingerprint，下次登录通过
    sqlx::query!(
        "UPDATE users SET hwid_bound = $1, hwid_last_changed_at = $2 WHERE id = $3",
        row.new_fingerprint,
        chrono::Utc::now(),
        row.user_id
    )
    .execute(&s.db)
    .await
    .map_err(internal)?;

    sqlx::query!(
        "INSERT INTO audit_log (actor, action, target, metadata) VALUES ($1, 'hwid_rebind.approve', $2, $3)",
        &actor.name, id.to_string(),
        serde_json::json!({"user_id": row.user_id.to_string()}))
        .execute(&s.db).await.ok();
    crate::audit::event(&actor.name, "hwid_rebind.approve", &id.to_string());

    Ok(Json(RebindResp {
        request_id: id.to_string(),
        status: "approved".into(),
    }))
}

pub async fn admin_deny(
    State(s): State<Arc<AppState>>,
    headers: HeaderMap,
    Path(id): Path<Uuid>,
    Json(body): Json<ReviewBody>,
) -> Result<Json<RebindResp>, (StatusCode, String)> {
    let actor = crate::admin_customization::require_actor_or_admin_key(
        &headers,
        &s,
        body.key.as_deref(),
        "admin.rebind.manage",
    )
    .await?;
    let updated = sqlx::query!(
        r#"UPDATE hwid_rebind_requests
           SET status='denied', reviewed_at=now(), reviewer=$2, review_note=$3
           WHERE id=$1 AND status='pending'"#,
        id,
        &actor.name,
        body.review_note
    )
    .execute(&s.db)
    .await
    .map_err(internal)?
    .rows_affected();
    if updated == 0 {
        return Err((StatusCode::NOT_FOUND, "no pending request".into()));
    }

    sqlx::query!(
        "INSERT INTO audit_log (actor, action, target, metadata) VALUES ($1, 'hwid_rebind.deny', $2, NULL)",
        &actor.name, id.to_string())
        .execute(&s.db).await.ok();
    crate::audit::event(&actor.name, "hwid_rebind.deny", &id.to_string());

    Ok(Json(RebindResp {
        request_id: id.to_string(),
        status: "denied".into(),
    }))
}

use crate::error::internal;

// ---------------- admin SSR ----------------
pub struct RebindRowVm {
    pub id: String,
    pub user_id: String,
    pub submitted_at: String,
    pub old: String,
    pub new: String,
    pub reason: String,
    pub diff: String,
}

#[derive(Template)]
#[template(path = "rebind_content.html")]
pub struct RebindPage {
    pub title: String,
    pub subtitle: Option<String>,
    pub notice: Option<ui::AdminNotice>,
    pub host: &'static str,
    pub route: &'static str,
    pub rows: Vec<RebindRowVm>,
}

#[derive(Deserialize, Default)]
pub struct RebindNoticeQuery {
    pub ok: Option<String>,
    pub err: Option<String>,
}

fn rebind_notice(q: &RebindNoticeQuery) -> Option<ui::AdminNotice> {
    match (q.ok.as_deref(), q.err.as_deref()) {
        (Some("approve"), _) => Some(ui::AdminNotice::success("HWID 重绑已通过")),
        (Some("deny"), _) => Some(ui::AdminNotice::success("HWID 重绑已拒绝")),
        (_, Some("not_found")) => Some(ui::AdminNotice::warning("请求不存在或已处理")),
        _ => None,
    }
}

pub async fn admin_pending_page(
    State(s): State<Arc<AppState>>,
    headers: HeaderMap,
    Query(q): Query<RebindNoticeQuery>,
) -> Response {
    if let Err(resp) =
        crate::admin_customization::require_actor(&headers, &s, "admin.rebind.read").await
    {
        return resp;
    }

    let rows = sqlx::query!(
        r#"SELECT id, user_id, submitted_at, old_fingerprint, new_fingerprint,
                  user_reason, parts_diff
           FROM hwid_rebind_requests WHERE status='pending'
           ORDER BY submitted_at DESC LIMIT 100"#
    )
    .fetch_all(&s.db)
    .await
    .unwrap_or_default();
    let rows = rows
        .into_iter()
        .map(|r| RebindRowVm {
            id: r.id.to_string(),
            user_id: r.user_id.to_string(),
            submitted_at: r.submitted_at.format("%Y-%m-%d %H:%M:%S").to_string(),
            old: r.old_fingerprint.unwrap_or_default(),
            new: r.new_fingerprint,
            reason: r.user_reason.unwrap_or_default(),
            diff: serde_json::to_string_pretty(&r.parts_diff).unwrap_or_default(),
        })
        .collect();
    ui::render(&RebindPage {
        title: "HWID 重绑定".into(),
        subtitle: Some("用户换硬件后等你审批".into()),
        notice: rebind_notice(&q),
        host: ui::host(),
        route: ui::ROUTE_REBIND,
        rows,
    })
    .into_response()
}

pub fn admin_routes() -> Router<Arc<AppState>> {
    Router::new()
        .route("/admin/rebind", get(admin_pending_page))
        .route("/admin/rebind/:id/approve", post(form_approve))
        .route("/admin/rebind/:id/deny", post(form_deny))
        .route("/api/admin/rebind/:id/approve", post(admin_approve))
        .route("/api/admin/rebind/:id/deny", post(admin_deny))
}

// SSR 的 form approve/deny（无 body）走简化 handler，复用 admin_approve 的 SQL
async fn form_approve(
    State(s): State<Arc<AppState>>,
    headers: HeaderMap,
    Path(id): Path<Uuid>,
) -> Response {
    let actor = match crate::admin_customization::require_actor(&headers, &s, "admin.rebind.manage")
        .await
    {
        Ok(v) => v,
        Err(resp) => return resp,
    };

    if let Ok(Some(row)) = sqlx::query!(
        r#"UPDATE hwid_rebind_requests SET status='approved', reviewed_at=now(), reviewer=$2
           WHERE id=$1 AND status='pending' RETURNING user_id, new_fingerprint"#,
        id,
        &actor.name
    )
    .fetch_optional(&s.db)
    .await
    {
        let _ = sqlx::query!(
            "UPDATE users SET hwid_bound=$1, hwid_last_changed_at=now() WHERE id=$2",
            row.new_fingerprint,
            row.user_id
        )
        .execute(&s.db)
        .await;
        let _ = sqlx::query!(
            "INSERT INTO audit_log (actor, action, target, metadata) VALUES ($1,'hwid_rebind.approve',$2,NULL)",
            &actor.name, id.to_string()).execute(&s.db).await;
        crate::audit::event(&actor.name, "hwid_rebind.approve", &id.to_string());
    }
    axum::response::Redirect::to("/admin/rebind?ok=approve").into_response()
}

async fn form_deny(
    State(s): State<Arc<AppState>>,
    headers: HeaderMap,
    Path(id): Path<Uuid>,
) -> Response {
    let actor = match crate::admin_customization::require_actor(&headers, &s, "admin.rebind.manage")
        .await
    {
        Ok(v) => v,
        Err(resp) => return resp,
    };

    let _ = sqlx::query!(
        "UPDATE hwid_rebind_requests SET status='denied', reviewed_at=now(), reviewer=$2 WHERE id=$1 AND status='pending'",
        id, &actor.name).execute(&s.db).await;
    let _ = sqlx::query!(
        "INSERT INTO audit_log (actor, action, target, metadata) VALUES ($1,'hwid_rebind.deny',$2,NULL)",
        &actor.name, id.to_string()).execute(&s.db).await;
    crate::audit::event(&actor.name, "hwid_rebind.deny", &id.to_string());
    axum::response::Redirect::to("/admin/rebind?ok=deny").into_response()
}
