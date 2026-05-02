use crate::state::AppState;
use axum::{extract::State, Json, http::StatusCode};
use launcher_shared::{error::AppError, hashing, tier::Tier, uid as shared_uid};
use serde::{Deserialize, Serialize};
use std::sync::Arc;
use chrono::{Utc, Duration};
use uuid::Uuid;

// =====================================================================
// POST /api/auth/register
// =====================================================================
#[derive(Deserialize)]
pub struct RegisterReq {
    pub username:  String,         // 注册时定，永远不可改
    pub password:  String,         // ≥8 chars
    pub email:     Option<String>, // 可选，未来用作密保
    pub hwid_hex:  String,         // HWID 注册即绑
    pub client_ver: Option<String>,
    pub invite_code: Option<String>, // 邀请制时校验，目前不强制
}

#[derive(Serialize)]
pub struct RegisterResp {
    pub user_id:       String,
    pub uid:           String,     // 公开 8 位短码 K8RX2QZP
    pub username:      String,
    pub nickname:      String,
    pub session_token: String,
    pub expires_at:    i64,
}

fn validate_username(u: &str) -> Result<(), &'static str> {
    if u.len() < 3 || u.len() > 32 {
        return Err("username 3-32 chars");
    }
    if !u.chars().all(|c| c.is_ascii_alphanumeric() || c == '_' || c == '.' || c == '-') {
        return Err("username allow [a-zA-Z0-9_.-] only");
    }
    if u.starts_with('.') || u.starts_with('-') {
        return Err("username cannot start with . or -");
    }
    Ok(())
}

pub async fn register(
    State(s): State<Arc<AppState>>,
    Json(req): Json<RegisterReq>,
) -> Result<Json<RegisterResp>, (StatusCode, String)> {
    // 1. 校验输入
    validate_username(&req.username).map_err(|e| (StatusCode::BAD_REQUEST, e.into()))?;
    if req.password.len() < 8 {
        return Err((StatusCode::BAD_REQUEST, "password ≥ 8 chars".into()));
    }
    if req.hwid_hex.len() != 64 {
        return Err((StatusCode::BAD_REQUEST, "hwid_hex must be 64 hex chars".into()));
    }

    // 2. username 是否已占用？username_hash 用于 login lookup
    let uname_hash = hashing::salt_hwid(&req.username, b"launcher.user.salt.v1");
    let exists = sqlx::query_scalar!(
        "SELECT 1 as exists FROM users WHERE username_hash = $1 OR username = $2",
        uname_hash, req.username)
        .fetch_optional(&s.db).await.map_err(internal)?;
    if exists.is_some() {
        return Err((StatusCode::CONFLICT, "username taken".into()));
    }

    // 3. 生成唯一 UID（最多重试 5 次）
    let uid = {
        let mut tries = 0;
        loop {
            let candidate = shared_uid::generate();
            let dup = sqlx::query_scalar!(
                "SELECT 1 as exists FROM users WHERE uid = $1", candidate)
                .fetch_optional(&s.db).await.map_err(internal)?;
            if dup.is_none() { break candidate; }
            tries += 1;
            if tries >= 5 {
                return Err((StatusCode::INTERNAL_SERVER_ERROR, "UID gen failed".into()));
            }
        }
    };

    // 4. argon2id hash 密码
    let pw_hash = hashing::hash_password(&req.password,
        s.cfg.argon_memory_kib, s.cfg.argon_iterations).map_err(internal)?;
    let hwid_salted = hashing::salt_hwid(&req.hwid_hex, b"launcher.hwid.salt.v1");
    let nickname    = req.username.clone();   // 默认 nickname = username

    // 5. INSERT
    let row = sqlx::query!(
        r#"INSERT INTO users
            (username_hash, password_hash, hwid_bound, uid, username, nickname,
             password_changed_at, created_at)
           VALUES ($1, $2, $3, $4, $5, $6, now(), now())
           RETURNING id"#,
        uname_hash, pw_hash, hwid_salted,
        uid, req.username, nickname)
        .fetch_one(&s.db).await.map_err(internal)?;

    // 6. 自动登录（发 session token）
    let now = Utc::now();
    let token = Uuid::new_v4().to_string();
    let expires = now + Duration::seconds(s.cfg.session_ttl_seconds);
    sqlx::query!(
        "INSERT INTO sessions (token, user_id, expires_at, created_at) VALUES ($1, $2, $3, $4)",
        token, row.id, expires, now)
        .execute(&s.db).await.map_err(internal)?;

    // 7. 记 audit + login_history
    sqlx::query!(
        "INSERT INTO audit_log (actor, action, target, metadata) VALUES ($1, 'auth.register', $2, $3)",
        row.id.to_string(), row.id.to_string(),
        serde_json::json!({"uid": uid, "username": req.username}))
        .execute(&s.db).await.ok();
    sqlx::query!(
        r#"INSERT INTO login_history (user_id, success, hwid_short, client_ver, failure_reason)
           VALUES ($1, true, $2, $3, NULL)"#,
        row.id, &req.hwid_hex[..16.min(req.hwid_hex.len())], req.client_ver)
        .execute(&s.db).await.ok();

    Ok(Json(RegisterResp {
        user_id: row.id.to_string(),
        uid,
        nickname: nickname.clone(),
        username: req.username,
        session_token: token,
        expires_at: expires.timestamp(),
    }))
}

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
