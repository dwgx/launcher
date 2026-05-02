// 媒体上传 + 公开下载。内容寻址（sha256），同样内容自动去重。
//
// POST /api/media/upload  multipart: file=<bytes>  + session_token form/header
//   返回 { media_id, sha256, mime, size, url }
// GET  /api/media/:sha256/:filename       公开（带文件名缓存友好），需 session
// 文件存 /opt/systembackend/media/<sha256[:2]>/<sha256>.<ext>

use crate::state::AppState;
use axum::{
    extract::{State, Multipart, Path, Query},
    http::{StatusCode, header},
    response::IntoResponse,
    Json,
};
use serde::{Deserialize, Serialize};
use std::sync::Arc;
use std::path::PathBuf;
use uuid::Uuid;
use sha2::{Digest, Sha256};
use tokio::io::AsyncWriteExt;

const MEDIA_ROOT: &str = "/opt/systembackend/media";

fn category_for(mime: &str) -> &'static str {
    if mime.starts_with("image/") { "image" }
    else if mime.starts_with("video/") { "video" }
    else { "generic" }
}

fn max_bytes_for(state: &AppState, mime: &str) -> u64 {
    match category_for(mime) {
        "image" => state.cfg.media_image_max_bytes,
        "video" => state.cfg.media_video_max_bytes,
        _       => state.cfg.media_generic_max_bytes,
    }
}

fn ext_for(mime: &str) -> &'static str {
    match mime {
        "image/png"        => "png",
        "image/jpeg"       => "jpg",
        "image/gif"        => "gif",
        "image/webp"       => "webp",
        "video/mp4"        => "mp4",
        "video/webm"       => "webm",
        "image/svg+xml"    => "svg",
        "application/json" => "json",   // lottie sticker
        _                  => "bin",
    }
}

fn internal<E: std::fmt::Display>(e: E) -> (StatusCode, String) {
    (StatusCode::INTERNAL_SERVER_ERROR, e.to_string())
}

pub async fn auth_user(state: &AppState, token: &str) -> Result<Uuid, (StatusCode, String)> {
    sqlx::query_scalar!(
        "SELECT user_id FROM sessions WHERE token = $1 AND expires_at > now()", token)
        .fetch_optional(&state.db).await
        .map_err(internal)?
        .ok_or((StatusCode::UNAUTHORIZED, "no session".into()))
}

#[derive(Serialize)]
pub struct UploadResp {
    pub media_id: i64,
    pub sha256:   String,
    pub mime:     String,
    pub size:     i64,
    pub url:      String,
}

pub async fn upload(
    State(s): State<Arc<AppState>>,
    mut mp: Multipart,
) -> Result<Json<UploadResp>, (StatusCode, String)> {
    let mut token: Option<String> = None;
    let mut file_bytes: Option<bytes::Bytes> = None;
    let mut file_mime:  Option<String> = None;

    while let Some(field) = mp.next_field().await
        .map_err(|e| (StatusCode::BAD_REQUEST, e.to_string()))? {
        let name = field.name().unwrap_or("").to_string();
        match name.as_str() {
            "session_token" => token = field.text().await.ok(),
            "file" => {
                file_mime = field.content_type().map(|s| s.to_string());
                file_bytes = field.bytes().await.ok();
            }
            _ => {}
        }
    }

    let token = token.ok_or((StatusCode::BAD_REQUEST, "session_token missing".into()))?;
    let uid = auth_user(&s, &token).await?;
    let bytes = file_bytes.ok_or((StatusCode::BAD_REQUEST, "file missing".into()))?;
    let mime = file_mime.unwrap_or_else(|| "application/octet-stream".into());
    let limit = max_bytes_for(&s, &mime);
    if (bytes.len() as u64) > limit {
        return Err((StatusCode::PAYLOAD_TOO_LARGE,
                    format!("{} max {} bytes", category_for(&mime), limit)));
    }

    // 计算 sha256
    let mut hasher = Sha256::new();
    hasher.update(&bytes);
    let digest = hasher.finalize();
    let sha = hex::encode(digest);
    let ext = ext_for(&mime);

    // 检查是否已存在 (同样 sha256)
    if let Some(row) = sqlx::query!(
        "SELECT id, mime, size_bytes FROM media_files WHERE sha256 = $1", sha)
        .fetch_optional(&s.db).await.map_err(internal)? {
        return Ok(Json(UploadResp {
            media_id: row.id,
            sha256:   sha.clone(),
            mime:     row.mime,
            size:     row.size_bytes,
            url:      format!("/api/media/{}/file.{}", sha, ext),
        }));
    }

    // 写文件 /opt/systembackend/media/<sha[:2]>/<sha>.<ext>
    let dir = PathBuf::from(MEDIA_ROOT).join(&sha[..2]);
    tokio::fs::create_dir_all(&dir).await.map_err(internal)?;
    let path = dir.join(format!("{}.{}", sha, ext));
    let mut f = tokio::fs::File::create(&path).await.map_err(internal)?;
    f.write_all(&bytes).await.map_err(internal)?;
    let rel_path = format!("{}/{}.{}", &sha[..2], sha, ext);

    let row = sqlx::query!(
        r#"INSERT INTO media_files (sha256, mime, size_bytes, relative_path, uploader_id)
           VALUES ($1, $2, $3, $4, $5) RETURNING id"#,
        sha, mime, bytes.len() as i64, rel_path, uid)
        .fetch_one(&s.db).await.map_err(internal)?;

    Ok(Json(UploadResp {
        media_id: row.id,
        sha256:   sha.clone(),
        mime,
        size:     bytes.len() as i64,
        url:      format!("/api/media/{}/file.{}", sha, ext),
    }))
}

#[derive(Deserialize)]
pub struct DlQuery { pub session_token: Option<String> }

// GET /api/media/:sha256/:filename   公开下载（要 session, 防爬）
pub async fn download(
    State(s): State<Arc<AppState>>,
    Path((sha, _name)): Path<(String, String)>,
    Query(q): Query<DlQuery>,
) -> impl IntoResponse {
    // session 要求宽松：未登录拿不到（返回 401 而不是 404 让客户端易调）
    if let Some(t) = &q.session_token {
        if auth_user(&s, t).await.is_err() {
            return (StatusCode::UNAUTHORIZED, "no session").into_response();
        }
    } else {
        return (StatusCode::UNAUTHORIZED, "session_token required").into_response();
    }

    let row = match sqlx::query!(
        "SELECT relative_path, mime FROM media_files WHERE sha256 = $1", sha)
        .fetch_optional(&s.db).await {
        Ok(Some(r)) => r,
        _ => return (StatusCode::NOT_FOUND, "not found").into_response(),
    };
    let path = PathBuf::from(MEDIA_ROOT).join(&row.relative_path);
    let bytes = match tokio::fs::read(&path).await {
        Ok(b) => b,
        Err(_) => return (StatusCode::NOT_FOUND, "missing").into_response(),
    };
    ([(header::CONTENT_TYPE, row.mime),
      (header::CACHE_CONTROL, "public, max-age=2592000".into())],
      bytes).into_response()
}
