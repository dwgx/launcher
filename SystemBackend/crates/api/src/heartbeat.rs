use crate::state::AppState;
use axum::{extract::State, Json, http::StatusCode};
use serde::{Deserialize, Serialize};
use std::sync::Arc;
use chrono::Utc;

#[derive(Deserialize)]
pub struct HeartbeatReq {
    pub session_token: String,
    pub hwid_hex: String,
}

#[derive(Serialize)]
pub struct HeartbeatResp {
    pub server_time: i64,
    pub session_valid_until: i64,
    pub subscription_valid_until: Option<i64>,
}

pub async fn heartbeat(
    State(s): State<Arc<AppState>>,
    Json(req): Json<HeartbeatReq>,
) -> Result<Json<HeartbeatResp>, (StatusCode, String)> {
    let row = sqlx::query!(
        r#"SELECT s.user_id, s.expires_at,
                  u.subscription_expires_at, u.hwid_bound
           FROM sessions s JOIN users u ON u.id = s.user_id
           WHERE s.token = $1"#,
        req.session_token)
        .fetch_optional(&s.db).await
        .map_err(|e| (StatusCode::INTERNAL_SERVER_ERROR, e.to_string()))?
        .ok_or((StatusCode::UNAUTHORIZED, "no session".into()))?;

    if row.expires_at < Utc::now() {
        return Err((StatusCode::UNAUTHORIZED, "session expired".into()));
    }

    let salted = launcher_shared::hashing::salt_hwid(&req.hwid_hex, b"launcher.hwid.salt.v1");
    if let Some(bound) = &row.hwid_bound {
        if bound != &salted {
            return Err((StatusCode::FORBIDDEN, "hwid mismatch".into()));
        }
    }

    // 心跳即"在线"信号 — 刷 last_seen，离线判定靠 (now() - last_seen)
    sqlx::query!("UPDATE users SET last_seen = now() WHERE id = $1", row.user_id)
        .execute(&s.db).await.ok();

    Ok(Json(HeartbeatResp {
        server_time: Utc::now().timestamp(),
        session_valid_until: row.expires_at.timestamp(),
        subscription_valid_until: row.subscription_expires_at.map(|t| t.timestamp()),
    }))
}
