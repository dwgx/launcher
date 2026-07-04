// 用户自服务：改昵称（限频）/ 改密码 / 上传头像 / 看登录历史。
// UID 和 Username 在这里**永远不能改**；admin 走 admin.rs 才能改。

use crate::media_policy;
use crate::state::AppState;
use axum::{
    extract::{Json, Multipart, Query, State},
    http::StatusCode,
    response::IntoResponse,
};
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

use crate::error::internal;

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
        r#"SELECT u.uid, u.username, u.nickname, u.avatar_path,
                  u.subscription_tier, u.subscription_expires_at,
                  u.nickname_changed_at, u.password_changed_at,
                  u.status,
                  COALESCE(u.status_text, '') as "status_text!",
                  COALESCE(u.bio, '') as "bio!",
                  u.role, u.role_label, u.is_admin,
                  COALESCE(m.version, 0) as "avatar_version!"
           FROM users u
           LEFT JOIN user_avatar_meta m ON m.user_id = u.id
           WHERE u.id = $1"#,
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
        avatar_url: row
            .avatar_path
            .map(|_| format!("/api/avatar/{}?v={}", uid, row.avatar_version)),
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

    let cooldown = rate_limit_seconds(&s, "nickname_change").await as f64;
    let result = sqlx::query(
        "UPDATE users SET nickname = $1, nickname_changed_at = now()
         WHERE id = $2
         AND (nickname_changed_at IS NULL OR EXTRACT(EPOCH FROM now() - nickname_changed_at) >= $3)",
    )
    .bind(trimmed)
    .bind(uid)
    .bind(cooldown)
    .execute(&s.db)
    .await
    .map_err(internal)?;
    if result.rows_affected() == 0 {
        return Err((StatusCode::TOO_MANY_REQUESTS, "nickname cooldown active".into()));
    }

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

    let dir = std::path::PathBuf::from(&s.cfg.avatar_root);
    tokio::fs::create_dir_all(&dir).await.map_err(internal)?;
    let path = dir.join(format!("{}.{}", uid, detected.ext));
    tokio::fs::write(&path, &bytes).await.map_err(internal)?;
    let path_str = path.to_string_lossy().to_string();

    // 生成头像缩略图（64/128）。GIF 头像不缩放（保留动图），失败不阻断上传。
    if detected.mime != "image/gif" {
        let _ = crate::media_thumb::generate(
            bytes.clone(),
            dir.clone(),
            uid.to_string(),
            crate::media_thumb::AVATAR_SLOTS,
        )
        .await;
    }

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

    // 记录/自增版本，返回带 ?v= 的 URL，让客户端缓存失效精确到每次换头像。
    let version = sqlx::query_scalar!(
        r#"INSERT INTO user_avatar_meta (user_id, version, bytes, updated_at)
              VALUES ($1, 1, $2, now())
              ON CONFLICT (user_id) DO UPDATE
              SET version = user_avatar_meta.version + 1,
                  bytes = $2, updated_at = now()
              RETURNING version"#,
        uid,
        bytes.len() as i32
    )
    .fetch_optional(&s.db)
    .await
    .ok()
    .flatten()
    .unwrap_or(1);

    Ok(Json(serde_json::json!({
        "avatar_url": format!("/api/avatar/{}?v={}", uid, version),
        "size": bytes.len(),
    })))
}

// =====================================================================
// GET /api/avatar/:id  (公开返回原图 / ?s= 变体)
// =====================================================================
#[derive(Deserialize)]
pub struct AvatarQuery {
    /// 版本号（换头像自增），仅用于缓存失效；服务端不校验值。
    pub v: Option<i32>,
    /// 变体档位（64/128）；命中则返回缩略图，缺失回退原图。
    pub s: Option<u32>,
}

pub async fn get_avatar(
    State(s): State<Arc<AppState>>,
    axum::extract::Path(id): axum::extract::Path<Uuid>,
    Query(q): Query<AvatarQuery>,
    headers: axum::http::HeaderMap,
) -> impl IntoResponse {
    use axum::http::header;
    let row = match sqlx::query!(
        r#"SELECT u.avatar_path, u.avatar_mime,
                  COALESCE(m.version, 0) AS "version!"
           FROM users u
           LEFT JOIN user_avatar_meta m ON m.user_id = u.id
           WHERE u.id = $1"#,
        id
    )
    .fetch_optional(&s.db)
    .await
    {
        Ok(Some(r)) => r,
        _ => return (StatusCode::NOT_FOUND, "no avatar").into_response(),
    };
    let orig_path = match row.avatar_path {
        Some(p) => p,
        None => return (StatusCode::NOT_FOUND, "no avatar").into_response(),
    };

    // ETag = uid + 版本 (+ 变体档)。换头像 version 自增 ⇒ ETag 变化 ⇒ 客户端重取。
    let version = row.version;
    let mut path = std::path::PathBuf::from(&orig_path);
    let mut mime = row
        .avatar_mime
        .clone()
        .unwrap_or_else(|| "application/octet-stream".into());
    let mut etag_core = format!("{}v{}", id, version);

    if let Some(req_slot) = q.s {
        if let Some(slot) = crate::media_thumb::resolve_slot(req_slot) {
            let dir = std::path::PathBuf::from(&s.cfg.avatar_root);
            for ext in ["jpg", "png"] {
                let cand = dir.join(crate::media_thumb::variant_filename(
                    &id.to_string(),
                    slot,
                    ext,
                ));
                if tokio::fs::try_exists(&cand).await.unwrap_or(false) {
                    path = cand;
                    mime = if ext == "png" {
                        "image/png".into()
                    } else {
                        "image/jpeg".into()
                    };
                    etag_core = format!("{}v{}_s{}", id, version, slot);
                    break;
                }
            }
        }
    }
    let _ = q.v; // v 只影响 URL 缓存键，逻辑上无需读取。

    let etag = format!("\"{}\"", etag_core);
    if let Some(inm) = headers
        .get(header::IF_NONE_MATCH)
        .and_then(|v| v.to_str().ok())
    {
        if inm.contains(&etag) || inm.trim() == "*" {
            return (
                StatusCode::NOT_MODIFIED,
                [
                    (header::ETAG, etag.clone()),
                    (
                        header::CACHE_CONTROL,
                        "public, max-age=31536000, immutable".into(),
                    ),
                ],
            )
                .into_response();
        }
    }

    let bytes = match tokio::fs::read(&path).await {
        Ok(b) => b,
        Err(_) => {
            tracing::warn!(uid = %id, path = %path.display(), "avatar file missing on disk");
            return (StatusCode::NOT_FOUND, "missing file").into_response();
        }
    };
    (
        [
            (header::CONTENT_TYPE, mime),
            (
                header::CACHE_CONTROL,
                "public, max-age=31536000, immutable".into(),
            ),
            (header::ETAG, etag),
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
    /// 真实 users.id（UUID）——客户端开 DM 需要严格 UUID（chat::open_dm）。
    pub user_id: String,
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
        r#"SELECT u.id, u.uid, u.username, u.nickname, u.avatar_path, u.status,
                  COALESCE(u.status_text, '') as "status_text!",
                  COALESCE(u.bio, '') as "bio!",
                  u.role, u.role_label,
                  COALESCE(m.version, 0) as "avatar_version!"
           FROM users u
           LEFT JOIN user_avatar_meta m ON m.user_id = u.id
           WHERE u.uid = $1 OR u.username = $1 OR u.id::text = $1"#,
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
        user_id: row.id.to_string(),
        nickname: row.nickname,
        avatar_url: row
            .avatar_path
            .map(|_| format!("/api/avatar/{}?v={}", row.id, row.avatar_version)),
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
