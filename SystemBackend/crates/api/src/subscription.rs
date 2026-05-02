use crate::state::AppState;
use axum::{extract::{State, Query}, Json, http::StatusCode};
use serde::{Deserialize, Serialize};
use std::sync::Arc;

#[derive(Deserialize)]
pub struct ListQuery { pub session_token: String }

#[derive(Serialize)]
pub struct SubscriptionMeta {
    pub id: String,
    pub name: String,
    pub helix_url: String,        // 客户端从这里拉签名后的 .helix
    pub updated_at: i64,
}

pub async fn list_for_user(
    State(s): State<Arc<AppState>>,
    Query(q): Query<ListQuery>,
) -> Result<Json<Vec<SubscriptionMeta>>, (StatusCode, String)> {
    let user_id = sqlx::query_scalar!(
        "SELECT user_id FROM sessions WHERE token = $1 AND expires_at > now()",
        q.session_token)
        .fetch_optional(&s.db).await
        .map_err(|e| (StatusCode::INTERNAL_SERVER_ERROR, e.to_string()))?
        .ok_or((StatusCode::UNAUTHORIZED, "no session".into()))?;

    let rows = sqlx::query!(
        r#"SELECT id, name, updated_at FROM subscriptions
           WHERE user_id = $1 ORDER BY updated_at DESC"#,
        user_id)
        .fetch_all(&s.db).await
        .map_err(|e| (StatusCode::INTERNAL_SERVER_ERROR, e.to_string()))?;

    let cdn = &s.cfg.cdn_base;
    let v = rows.into_iter().map(|r| SubscriptionMeta {
        id: r.id.clone(),
        name: r.name,
        helix_url: format!("{}/sub/{}.helix", cdn.trim_end_matches('/'), r.id),
        updated_at: r.updated_at.timestamp(),
    }).collect();
    Ok(Json(v))
}
