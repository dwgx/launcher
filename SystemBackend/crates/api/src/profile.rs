// 用户自服务：改昵称（限频）/ 改密码 / 上传头像 / 看登录历史。
// UID 和 Username 在这里**永远不能改**；admin 走 admin.rs 才能改。

use crate::media_policy;
use crate::state::AppState;
use axum::{
    extract::{Json, Multipart, Query, State},
    http::StatusCode,
    response::IntoResponse,
};
use chrono::Utc;
use launcher_shared::hashing;
use serde::{Deserialize, Serialize};
use std::sync::Arc;
use uuid::Uuid;

// =====================================================================
// 共用：从 session token 解出 user_id
// =====================================================================
async fn auth_user(state: &AppState, token: &str) -> Result<Uuid, (StatusCode, String)> {
    sqlx::query_scalar!(
        "SELECT user_id FROM sessions WHERE token = $1 AND expires_at > now()",
        token
    )
    .fetch_optional(&state.db)
    .await
    .map_err(internal)?
    .ok_or((StatusCode::UNAUTHORIZED, "no session".into()))
}

fn internal<E: std::fmt::Display>(e: E) -> (StatusCode, String) {
    (StatusCode::INTERNAL_SERVER_ERROR, e.to_string())
}

async fn rate_limit_seconds(state: &AppState, key: &str) -> i32 {
    sqlx::query_scalar!("SELECT seconds FROM rate_limits WHERE key=$1", key)
        .fetch_optional(&state.db)
        .await
        .ok()
        .flatten()
        .unwrap_or(0)
}

// =====================================================================
// GET /api/profile
// =====================================================================
#[derive(Deserialize)]
pub struct ProfileQuery {
    pub session_token: String,
}

#[derive(Serialize)]
pub struct ProfileResp {
    pub user_id: String,
    pub uid: String,
    pub username: String,
    pub nickname: Option<String>,
    pub avatar_url: Option<String>,
    pub tier: Option<String>,
    pub tier_expires_at: Option<i64>,
    pub nickname_changed_at: Option<i64>,
    pub password_changed_at: Option<i64>,
    pub nickname_change_cooldown_seconds: i32,
    // 用户可在客户端编辑的内容 — 客户端登录后拉一次填回 g_user / g_status
    pub status: String,
    pub status_text: String,
    pub bio: String,
    pub role: String,
    pub role_label: Option<String>,
    pub is_admin: bool,
}

pub async fn get_profile(
    State(s): State<Arc<AppState>>,
    Query(q): Query<ProfileQuery>,
) -> Result<Json<ProfileResp>, (StatusCode, String)> {
    let uid = auth_user(&s, &q.session_token).await?;
    let row = sqlx::query!(
        r#"SELECT uid, username, nickname, avatar_path,
                  subscription_tier, subscription_expires_at,
                  nickname_changed_at, password_changed_at,
                  status,
                  COALESCE(status_text, '') as "status_text!",
                  COALESCE(bio, '') as "bio!",
                  role, role_label, is_admin
           FROM users WHERE id = $1"#,
        uid
    )
    .fetch_one(&s.db)
    .await
    .map_err(internal)?;

    let cooldown = rate_limit_seconds(&s, "nickname_change").await;

    Ok(Json(ProfileResp {
        user_id: uid.to_string(),
        uid: row.uid.unwrap_or_default(),
        username: row.username.unwrap_or_default(),
        nickname: row.nickname,
        avatar_url: row.avatar_path.map(|_| format!("/api/avatar/{}", uid)),
        tier: row.subscription_tier,
        tier_expires_at: row.subscription_expires_at.map(|t| t.timestamp()),
        nickname_changed_at: row.nickname_changed_at.map(|t| t.timestamp()),
        password_changed_at: row.password_changed_at.map(|t| t.timestamp()),
        nickname_change_cooldown_seconds: cooldown,
        status: if row.status.is_empty() {
            "online".into()
        } else {
            row.status
        },
        status_text: row.status_text,
        bio: row.bio,
        role: row.role,
        role_label: row.role_label,
        is_admin: row.is_admin,
    }))
}

// =====================================================================
// POST /api/profile/nickname
// =====================================================================
#[derive(Deserialize)]
pub struct ChangeNicknameReq {
    pub session_token: String,
    pub new_nickname: String,
}

pub async fn change_nickname(
    State(s): State<Arc<AppState>>,
    Json(req): Json<ChangeNicknameReq>,
) -> Result<StatusCode, (StatusCode, String)> {
    let uid = auth_user(&s, &req.session_token).await?;

    let trimmed = req.new_nickname.trim();
    if trimmed.is_empty() || trimmed.chars().count() > 24 {
        return Err((StatusCode::BAD_REQUEST, "nickname length 1-24".into()));
    }

    let cooldown = rate_limit_seconds(&s, "nickname_change").await as i64;
    let last = sqlx::query_scalar!("SELECT nickname_changed_at FROM users WHERE id=$1", uid)
        .fetch_one(&s.db)
        .await
        .map_err(internal)?;
    if let Some(prev) = last {
        let elapsed = Utc::now().timestamp() - prev.timestamp();
        if elapsed < cooldown {
            return Err((
                StatusCode::TOO_MANY_REQUESTS,
                format!("cooldown {} seconds remaining", cooldown - elapsed),
            ));
        }
    }

    sqlx::query!(
        "UPDATE users SET nickname=$1, nickname_changed_at=now() WHERE id=$2",
        trimmed,
        uid
    )
    .execute(&s.db)
    .await
    .map_err(internal)?;

    sqlx::query!(
        "INSERT INTO audit_log (actor, action, target, metadata) VALUES ($1, 'profile.nickname', $2, $3)",
        uid.to_string(), uid.to_string(),
        serde_json::json!({ "new_nickname": trimmed }))
        .execute(&s.db).await.ok();

    Ok(StatusCode::NO_CONTENT)
}

// =====================================================================
// POST /api/profile/password
// =====================================================================
#[derive(Deserialize)]
pub struct ChangePasswordReq {
    pub session_token: String,
    pub old_password: String,
    pub new_password: String,
}

pub async fn change_password(
    State(s): State<Arc<AppState>>,
    Json(req): Json<ChangePasswordReq>,
) -> Result<StatusCode, (StatusCode, String)> {
    let uid = auth_user(&s, &req.session_token).await?;
    if req.new_password.len() < 8 {
        return Err((StatusCode::BAD_REQUEST, "password >= 8 chars".into()));
    }

    let row = sqlx::query!("SELECT password_hash FROM users WHERE id=$1", uid)
        .fetch_one(&s.db)
        .await
        .map_err(internal)?;

    if !hashing::verify_password(&req.old_password, &row.password_hash).map_err(internal)? {
        return Err((StatusCode::UNAUTHORIZED, "old password wrong".into()));
    }
    let new_hash = hashing::hash_password(
        &req.new_password,
        s.cfg.argon_memory_kib,
        s.cfg.argon_iterations,
    )
    .map_err(internal)?;

    sqlx::query!(
        "UPDATE users SET password_hash=$1, password_changed_at=now() WHERE id=$2",
        new_hash,
        uid
    )
    .execute(&s.db)
    .await
    .map_err(internal)?;

    // 改密后强制所有 session 下线
    sqlx::query!("DELETE FROM sessions WHERE user_id=$1", uid)
        .execute(&s.db)
        .await
        .ok();

    sqlx::query!(
        "INSERT INTO audit_log (actor, action, target, metadata) VALUES ($1, 'profile.password', $2, NULL)",
        uid.to_string(), uid.to_string())
        .execute(&s.db).await.ok();

    Ok(StatusCode::NO_CONTENT)
}

// =====================================================================
// POST /api/profile/avatar  (multipart)
//   file=<image>  session_token=<token>
// =====================================================================
pub async fn upload_avatar(
    State(s): State<Arc<AppState>>,
    mut mp: Multipart,
) -> Result<Json<serde_json::Value>, (StatusCode, String)> {
    let mut token: Option<String> = None;
    let mut bytes: Option<axum::body::Bytes> = None;
    let mut mime: Option<String> = None;

    while let Some(field) = mp
        .next_field()
        .await
        .map_err(|e| (StatusCode::BAD_REQUEST, e.to_string()))?
    {
        let name = field.name().unwrap_or("").to_string();
        match name.as_str() {
            "session_token" => token = field.text().await.ok(),
            "file" => {
                mime = field.content_type().map(|s| s.to_string());
                bytes = field.bytes().await.ok();
            }
            _ => {}
        }
    }

    let token = token.ok_or((StatusCode::BAD_REQUEST, "session_token missing".into()))?;
    let uid = auth_user(&s, &token).await?;
    let bytes = bytes.ok_or((StatusCode::BAD_REQUEST, "file missing".into()))?;
    let _declared_mime = mime.unwrap_or_default();

    // 限制：5MB；只接受 image/png|jpeg|gif
    if bytes.is_empty() {
        return Err((StatusCode::BAD_REQUEST, "file empty".into()));
    }
    if bytes.len() > 5 * 1024 * 1024 {
        return Err((StatusCode::PAYLOAD_TOO_LARGE, "max 5MB".into()));
    }
    let detected = media_policy::validate_avatar(&bytes)?;

    let dir = std::path::Path::new(&s.cfg.avatar_root);
    std::fs::create_dir_all(dir).map_err(internal)?;
    let path = dir.join(format!("{}.{}", uid, detected.ext));
    std::fs::write(&path, &bytes).map_err(internal)?;
    let path_str = path.to_string_lossy().to_string();

    sqlx::query!(
        r#"UPDATE users
              SET avatar_path=$1, avatar_mime=$2, avatar_updated_at=now()
              WHERE id=$3"#,
        path_str,
        detected.mime,
        uid
    )
    .execute(&s.db)
    .await
    .map_err(internal)?;

    sqlx::query!(
        r#"INSERT INTO user_avatar_meta (user_id, version, bytes, updated_at)
              VALUES ($1, 1, $2, now())
              ON CONFLICT (user_id) DO UPDATE
              SET version = user_avatar_meta.version + 1,
                  bytes = $2, updated_at = now()"#,
        uid,
        bytes.len() as i32
    )
    .execute(&s.db)
    .await
    .ok();

    Ok(Json(serde_json::json!({
        "avatar_url": format!("/api/avatar/{}", uid),
        "size": bytes.len(),
    })))
}

// =====================================================================
// GET /api/avatar/:id  (公开返回原图)
// =====================================================================
pub async fn get_avatar(
    State(s): State<Arc<AppState>>,
    axum::extract::Path(id): axum::extract::Path<Uuid>,
) -> impl IntoResponse {
    use axum::http::header;
    let row = match sqlx::query!("SELECT avatar_path, avatar_mime FROM users WHERE id=$1", id)
        .fetch_optional(&s.db)
        .await
    {
        Ok(Some(r)) => r,
        _ => return (StatusCode::NOT_FOUND, "no avatar").into_response(),
    };
    let path = match row.avatar_path {
        Some(p) => p,
        None => return (StatusCode::NOT_FOUND, "no avatar").into_response(),
    };
    let bytes = match std::fs::read(&path) {
        Ok(b) => b,
        Err(_) => return (StatusCode::NOT_FOUND, "missing file").into_response(),
    };
    let mime = row
        .avatar_mime
        .unwrap_or_else(|| "application/octet-stream".into());
    (
        [
            (header::CONTENT_TYPE, mime),
            (header::CACHE_CONTROL, "public, max-age=300".into()),
        ],
        bytes,
    )
        .into_response()
}

// =====================================================================
// GET /api/profile/login-history
// =====================================================================
#[derive(Serialize)]
pub struct LoginEntry {
    pub occurred_at: i64,
    pub success: bool,
    pub failure_reason: Option<String>,
    pub remote_ip: Option<String>,
    pub user_agent: Option<String>,
    pub hwid_short: Option<String>,
    pub client_ver: Option<String>,
    pub geo_country: Option<String>,
    pub geo_city: Option<String>,
}

pub async fn login_history(
    State(s): State<Arc<AppState>>,
    Query(q): Query<ProfileQuery>,
) -> Result<Json<Vec<LoginEntry>>, (StatusCode, String)> {
    let uid = auth_user(&s, &q.session_token).await?;
    let rows = sqlx::query!(
        r#"SELECT occurred_at, success, failure_reason,
                  host(remote_ip) as ip, user_agent, hwid_short,
                  client_ver, geo_country, geo_city
           FROM login_history
           WHERE user_id=$1
           ORDER BY occurred_at DESC LIMIT 100"#,
        uid
    )
    .fetch_all(&s.db)
    .await
    .map_err(internal)?;

    Ok(Json(
        rows.into_iter()
            .map(|r| LoginEntry {
                occurred_at: r.occurred_at.timestamp(),
                success: r.success,
                failure_reason: r.failure_reason,
                remote_ip: r.ip,
                user_agent: r.user_agent,
                hwid_short: r.hwid_short,
                client_ver: r.client_ver,
                geo_country: r.geo_country,
                geo_city: r.geo_city,
            })
            .collect(),
    ))
}

// =====================================================================
// 用户状态 (online/busy/away/sleep/offline)
// 客户端切状态时调一次；其他用户通过 chat history / WS push 看到。
// =====================================================================
#[derive(Deserialize)]
pub struct SetStatusReq {
    pub session_token: String,
    pub status: String,
}

pub async fn set_status(
    State(s): State<Arc<AppState>>,
    Json(req): Json<SetStatusReq>,
) -> Result<StatusCode, (StatusCode, String)> {
    let uid = auth_user(&s, &req.session_token).await?;
    let valid = ["online", "busy", "away", "sleep", "offline"];
    if !valid.contains(&req.status.as_str()) {
        return Err((StatusCode::BAD_REQUEST, "invalid status".into()));
    }
    sqlx::query!(
        "UPDATE users SET status = $1, last_seen = now() WHERE id = $2",
        req.status,
        uid
    )
    .execute(&s.db)
    .await
    .map_err(internal)?;

    // WS 广播：通知所有在线用户该 user 状态变了
    let payload = serde_json::json!({
        "type": "status",
        "user_id": uid.to_string(),
        "status": req.status
    });
    crate::ws::broadcast_all(&s, &payload);
    Ok(StatusCode::NO_CONTENT)
}

// =====================================================================
// 个人标签 — Home view "+添加" chip 后端持久化
// 表: user_tags (user_id UUID, tag TEXT, sort_order INT, PK(user_id, tag))
// =====================================================================
const TAG_PER_USER_LIMIT: i64 = 20;
const TAG_MAX_CHARS: usize = 24;

#[derive(Serialize)]
pub struct TagsResp {
    pub tags: Vec<String>,
}

pub async fn list_tags(
    State(s): State<Arc<AppState>>,
    Query(q): Query<ProfileQuery>,
) -> Result<Json<TagsResp>, (StatusCode, String)> {
    let uid = auth_user(&s, &q.session_token).await?;
    let rows = sqlx::query_scalar!(
        "SELECT tag FROM user_tags WHERE user_id = $1 ORDER BY sort_order, tag",
        uid
    )
    .fetch_all(&s.db)
    .await
    .map_err(internal)?;
    Ok(Json(TagsResp { tags: rows }))
}

#[derive(Deserialize)]
pub struct AddTagReq {
    pub session_token: String,
    pub tag: String,
}

pub async fn add_tag(
    State(s): State<Arc<AppState>>,
    Json(req): Json<AddTagReq>,
) -> Result<Json<TagsResp>, (StatusCode, String)> {
    let uid = auth_user(&s, &req.session_token).await?;
    let trimmed = req.tag.trim();
    if trimmed.is_empty() || trimmed.chars().count() > TAG_MAX_CHARS {
        return Err((
            StatusCode::BAD_REQUEST,
            format!("tag length 1-{}", TAG_MAX_CHARS),
        ));
    }
    let count: i64 = sqlx::query_scalar!("SELECT COUNT(*) FROM user_tags WHERE user_id = $1", uid)
        .fetch_one(&s.db)
        .await
        .map_err(internal)?
        .unwrap_or(0);
    if count >= TAG_PER_USER_LIMIT {
        return Err((
            StatusCode::PAYLOAD_TOO_LARGE,
            format!("最多 {} 个标签", TAG_PER_USER_LIMIT),
        ));
    }
    sqlx::query!(
        r#"INSERT INTO user_tags (user_id, tag, sort_order)
           VALUES ($1, $2, $3) ON CONFLICT DO NOTHING"#,
        uid,
        trimmed,
        (count + 1) as i32 * 10
    )
    .execute(&s.db)
    .await
    .map_err(internal)?;

    let rows = sqlx::query_scalar!(
        "SELECT tag FROM user_tags WHERE user_id = $1 ORDER BY sort_order, tag",
        uid
    )
    .fetch_all(&s.db)
    .await
    .map_err(internal)?;
    Ok(Json(TagsResp { tags: rows }))
}

#[derive(Deserialize)]
pub struct RemoveTagReq {
    pub session_token: String,
    pub tag: String,
}

pub async fn remove_tag(
    State(s): State<Arc<AppState>>,
    Json(req): Json<RemoveTagReq>,
) -> Result<Json<TagsResp>, (StatusCode, String)> {
    let uid = auth_user(&s, &req.session_token).await?;
    sqlx::query!(
        "DELETE FROM user_tags WHERE user_id = $1 AND tag = $2",
        uid,
        req.tag.trim()
    )
    .execute(&s.db)
    .await
    .map_err(internal)?;
    let rows = sqlx::query_scalar!(
        "SELECT tag FROM user_tags WHERE user_id = $1 ORDER BY sort_order, tag",
        uid
    )
    .fetch_all(&s.db)
    .await
    .map_err(internal)?;
    Ok(Json(TagsResp { tags: rows }))
}

// =====================================================================
// POST /api/profile/update — status_text + bio (任意可选)
// =====================================================================
#[derive(Deserialize)]
pub struct ProfileUpdateReq {
    pub session_token: String,
    pub status_text: Option<String>,
    pub bio: Option<String>,
}

pub async fn update_profile(
    State(s): State<Arc<AppState>>,
    Json(req): Json<ProfileUpdateReq>,
) -> Result<StatusCode, (StatusCode, String)> {
    let uid = auth_user(&s, &req.session_token).await?;
    if let Some(t) = req.status_text {
        let trimmed: String = t.chars().take(48).collect();
        sqlx::query!(
            "UPDATE users SET status_text = $1 WHERE id = $2",
            trimmed,
            uid
        )
        .execute(&s.db)
        .await
        .map_err(internal)?;
    }
    if let Some(b) = req.bio {
        let trimmed: String = b.chars().take(240).collect();
        sqlx::query!("UPDATE users SET bio = $1 WHERE id = $2", trimmed, uid)
            .execute(&s.db)
            .await
            .map_err(internal)?;
    }
    Ok(StatusCode::OK)
}

// =====================================================================
// GET /api/profile/peer/:key — 看别人主页 (key = uid 7-digit / username / user_id uuid)
// =====================================================================
#[derive(Serialize)]
pub struct PeerProfileResp {
    pub uid: String,
    pub username: String,
    pub nickname: Option<String>,
    pub avatar_url: Option<String>,
    pub status: String,
    pub status_text: String,
    pub bio: String,
    pub tags: Vec<String>,
    pub role: Option<String>,
    pub role_label: Option<String>,
}

pub async fn get_peer_profile(
    State(s): State<Arc<AppState>>,
    axum::extract::Path(key): axum::extract::Path<String>,
    Query(q): Query<ProfileQuery>,
) -> Result<Json<PeerProfileResp>, (StatusCode, String)> {
    let _ = auth_user(&s, &q.session_token).await?;
    let row = sqlx::query!(
        r#"SELECT id, uid, username, nickname, avatar_path, status,
                  COALESCE(status_text, '') as "status_text!",
                  COALESCE(bio, '') as "bio!",
                  role, role_label
           FROM users
           WHERE uid = $1 OR username = $1 OR id::text = $1"#,
        key
    )
    .fetch_optional(&s.db)
    .await
    .map_err(internal)?
    .ok_or((StatusCode::NOT_FOUND, "user not found".into()))?;
    let tags = sqlx::query_scalar!(
        "SELECT tag FROM user_tags WHERE user_id = $1 ORDER BY sort_order, tag",
        row.id
    )
    .fetch_all(&s.db)
    .await
    .unwrap_or_default();
    Ok(Json(PeerProfileResp {
        uid: row.uid.unwrap_or_default(),
        username: row.username.unwrap_or_default(),
        nickname: row.nickname,
        avatar_url: row.avatar_path.map(|_| format!("/api/avatar/{}", row.id)),
        status: if row.status.is_empty() {
            "offline".into()
        } else {
            row.status
        },
        status_text: row.status_text,
        bio: row.bio,
        tags,
        role: Some(row.role),
        role_label: row.role_label,
    }))
}
