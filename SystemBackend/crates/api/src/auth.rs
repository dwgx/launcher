use crate::state::AppState;
use axum::{extract::State, Json, http::StatusCode};
use launcher_shared::{error::AppError, hashing, tier::Tier};
use serde::{Deserialize, Serialize};
use std::sync::Arc;
use chrono::{Utc, Duration};
use uuid::Uuid;

#[derive(Deserialize)]
pub struct LoginReq {
    pub username:  String,
    pub password:  String,
    pub hwid_hex:  String,        // 客户端拼接 + sha256 后的 hex
    pub client_ver: String,
}

#[derive(Serialize)]
pub struct LoginResp {
    pub session_token:        String,
    pub expires_at:           i64,
    pub subscription_tier:    Option<String>,
    pub subscription_expires: Option<i64>,
    pub user_id:              String,
}

pub async fn login(
    State(s): State<Arc<AppState>>,
    Json(req): Json<LoginReq>,
) -> Result<Json<LoginResp>, (StatusCode, String)> {
    let row = sqlx::query!(
        r#"SELECT id, password_hash, hwid_bound, subscription_tier, subscription_expires_at
           FROM users WHERE username_hash = $1"#,
        hashing::salt_hwid(&req.username, b"launcher.user.salt.v1"),
    )
    .fetch_optional(&s.db).await
    .map_err(internal)?
    .ok_or((StatusCode::UNAUTHORIZED, "invalid credentials".into()))?;

    if !hashing::verify_password(&req.password, &row.password_hash).map_err(internal)? {
        return Err((StatusCode::UNAUTHORIZED, "invalid credentials".into()));
    }

    let hwid_salted = hashing::salt_hwid(&req.hwid_hex, b"launcher.hwid.salt.v1");
    match &row.hwid_bound {
        Some(bound) if bound != &hwid_salted => {
            return Err((StatusCode::FORBIDDEN, "hwid mismatch".into()));
        }
        None => {
            sqlx::query!("UPDATE users SET hwid_bound = $1 WHERE id = $2",
                hwid_salted, row.id)
                .execute(&s.db).await.map_err(internal)?;
        }
        _ => {}
    }

    let now = Utc::now();
    let token = Uuid::new_v4().to_string();
    let expires = now + Duration::seconds(s.cfg.session_ttl_seconds);

    sqlx::query!(
        r#"INSERT INTO sessions (token, user_id, expires_at, created_at)
           VALUES ($1, $2, $3, $4)"#,
        token, row.id, expires, now)
        .execute(&s.db).await.map_err(internal)?;

    let tier_str = row.subscription_tier.clone();
    let exp_unix = row.subscription_expires_at.map(|t| t.timestamp());

    Ok(Json(LoginResp {
        session_token: token,
        expires_at: expires.timestamp(),
        subscription_tier: tier_str,
        subscription_expires: exp_unix,
        user_id: row.id.to_string(),
    }))
}

#[derive(Deserialize)]
pub struct LogoutReq { pub session_token: String }

pub async fn logout(
    State(s): State<Arc<AppState>>,
    Json(req): Json<LogoutReq>,
) -> Result<StatusCode, (StatusCode, String)> {
    sqlx::query!("DELETE FROM sessions WHERE token = $1", req.session_token)
        .execute(&s.db).await.map_err(internal)?;
    Ok(StatusCode::NO_CONTENT)
}

fn internal<E: std::fmt::Display>(e: E) -> (StatusCode, String) {
    (StatusCode::INTERNAL_SERVER_ERROR, e.to_string())
}

#[allow(dead_code)]
fn _unused(_e: AppError, _t: Tier) {}
