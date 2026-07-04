// 媒体上传 + 公开下载。内容寻址（sha256），同样内容自动去重。
//
// POST /api/media/upload  multipart: file=<bytes>  + session_token form/header
//   返回 { media_id, sha256, mime, size, url }
// GET  /api/media/:sha256/:filename       公开（带文件名缓存友好），需 session
// 文件存 /opt/systembackend/media/<sha256[:2]>/<sha256>.<ext>

use crate::media_policy;
use crate::media_thumb;
use crate::state::AppState;
use axum::{
    extract::{Multipart, Path, Query, State},
    http::{header, HeaderMap, StatusCode},
    response::IntoResponse,
    Json,
};
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::path::PathBuf;
use std::sync::Arc;
use tokio::io::AsyncWriteExt;
use uuid::Uuid;

fn max_bytes_for(state: &AppState, category: &str) -> u64 {
    match category {
        "image" => state.cfg.media_image_max_bytes,
        "video" => state.cfg.media_video_max_bytes,
        _ => state.cfg.media_generic_max_bytes,
    }
}

use crate::error::internal;

pub async fn auth_user(state: &AppState, token: &str) -> Result<Uuid, (StatusCode, String)> {
    sqlx::query_scalar!(
        "SELECT user_id FROM sessions WHERE token = $1 AND expires_at > now()",
        token
    )
    .fetch_optional(&state.db)
    .await
    .map_err(internal)?
    .ok_or((StatusCode::UNAUTHORIZED, "no session".into()))
}

#[derive(Serialize)]
pub struct UploadResp {
    pub media_id: i64,
    pub sha256: String,
    pub mime: String,
    pub size: i64,
    pub url: String,
    /// BlurHash 占位串（生成失败或非位图时为 None）。
    #[serde(skip_serializing_if = "Option::is_none")]
    pub blurhash: Option<String>,
    /// 图像内在宽高（位图解码成功时填充），客户端据此定占位符尺寸、免 reflow。
    #[serde(skip_serializing_if = "Option::is_none")]
    pub width: Option<i32>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub height: Option<i32>,
}

pub async fn upload(
    State(s): State<Arc<AppState>>,
    mut mp: Multipart,
) -> Result<Json<UploadResp>, (StatusCode, String)> {
    let mut token: Option<String> = None;
    let mut file_bytes: Option<bytes::Bytes> = None;

    while let Some(field) = mp
        .next_field()
        .await
        .map_err(|e| (StatusCode::BAD_REQUEST, e.to_string()))?
    {
        let name = field.name().unwrap_or("").to_string();
        match name.as_str() {
            "session_token" => token = field.text().await.ok(),
            "file" => {
                file_bytes = field.bytes().await.ok();
            }
            _ => {}
        }
    }

    let token = token.ok_or((StatusCode::BAD_REQUEST, "session_token missing".into()))?;
    let uid = auth_user(&s, &token).await?;
    let bytes = file_bytes.ok_or((StatusCode::BAD_REQUEST, "file missing".into()))?;
    if bytes.is_empty() {
        return Err((StatusCode::BAD_REQUEST, "file empty".into()));
    }
    let detected = media_policy::validate_upload(&bytes)?;
    let limit = max_bytes_for(&s, detected.category);
    if (bytes.len() as u64) > limit {
        return Err((
            StatusCode::PAYLOAD_TOO_LARGE,
            format!("{} max {} bytes", detected.category, limit),
        ));
    }

    // 计算 sha256
    let mut hasher = Sha256::new();
    hasher.update(&bytes);
    let digest = hasher.finalize();
    let sha = hex::encode(digest);

    // 检查是否已存在 (同样 sha256)
    if let Some(row) = sqlx::query!(
        "SELECT id, mime, size_bytes, width, height, blurhash FROM media_files WHERE sha256 = $1",
        sha
    )
    .fetch_optional(&s.db)
    .await
    .map_err(internal)?
    {
        return Ok(Json(UploadResp {
            media_id: row.id,
            sha256: sha.clone(),
            mime: row.mime,
            size: row.size_bytes,
            url: format!("/api/media/{}/file.{}", sha, detected.ext),
            blurhash: row.blurhash,
            width: row.width,
            height: row.height,
        }));
    }

    // 写文件 /opt/systembackend/media/<sha[:2]>/<sha>.<ext>
    let dir = PathBuf::from(&s.cfg.media_root).join(&sha[..2]);
    tokio::fs::create_dir_all(&dir).await.map_err(internal)?;
    let path = dir.join(format!("{}.{}", sha, detected.ext));
    let mut f = tokio::fs::File::create(&path).await.map_err(internal)?;
    f.write_all(&bytes).await.map_err(internal)?;
    let rel_path = format!("{}/{}.{}", &sha[..2], sha, detected.ext);

    // 生成缩略图 + BlurHash（仅位图；GIF/视频跳过 —— 动图不缩放、视频无解码器）。
    // 失败不阻断上传：只存原图，has_thumbs=false。
    let mut width: Option<i32> = None;
    let mut height: Option<i32> = None;
    let mut blurhash: Option<String> = None;
    let mut has_thumbs = false;
    if detected.category == "image" && detected.mime != "image/gif" {
        if let Some(t) = media_thumb::generate(
            bytes.clone(),
            dir.clone(),
            sha.clone(),
            media_thumb::MEDIA_SLOTS,
        )
        .await
        {
            width = i32::try_from(t.width).ok();
            height = i32::try_from(t.height).ok();
            if !t.blurhash.is_empty() {
                blurhash = Some(t.blurhash);
            }
            has_thumbs = !t.variants.is_empty();
        }
    }

    // ON CONFLICT：并发同 sha 上传时不 500，返回已存在行的元数据。
    let row = sqlx::query!(
        r#"INSERT INTO media_files
               (sha256, mime, size_bytes, relative_path, uploader_id, width, height, blurhash, has_thumbs)
           VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9)
           ON CONFLICT (sha256) DO UPDATE SET sha256 = EXCLUDED.sha256
           RETURNING id, width, height, blurhash"#,
        sha,
        detected.mime,
        bytes.len() as i64,
        rel_path,
        uid,
        width,
        height,
        blurhash,
        has_thumbs
    )
    .fetch_one(&s.db)
    .await
    .map_err(internal)?;

    Ok(Json(UploadResp {
        media_id: row.id,
        sha256: sha.clone(),
        mime: detected.mime.into(),
        size: bytes.len() as i64,
        url: format!("/api/media/{}/file.{}", sha, detected.ext),
        blurhash: row.blurhash,
        width: row.width,
        height: row.height,
    }))
}

#[derive(Deserialize)]
pub struct DlQuery {
    pub session_token: Option<String>,
    /// 变体档位（64/128/256/400/512）；命中则返回对应缩略图，缺失回退原图。
    pub s: Option<u32>,
}

// GET /api/media/:sha256/:filename   公开下载（要 session, 防爬）
pub async fn download(
    State(s): State<Arc<AppState>>,
    Path((sha, _name)): Path<(String, String)>,
    Query(q): Query<DlQuery>,
    headers: HeaderMap,
) -> impl IntoResponse {
    if !media_policy::is_hex_sha256(&sha) {
        return (StatusCode::BAD_REQUEST, "bad sha256").into_response();
    }

    // session 要求宽松：未登录拿不到（返回 401 而不是 404 让客户端易调）
    if let Some(t) = &q.session_token {
        if auth_user(&s, t).await.is_err() {
            return (StatusCode::UNAUTHORIZED, "no session").into_response();
        }
    } else {
        return (StatusCode::UNAUTHORIZED, "session_token required").into_response();
    }

    let row = match sqlx::query!(
        "SELECT relative_path, mime FROM media_files WHERE sha256 = $1",
        sha
    )
    .fetch_optional(&s.db)
    .await
    {
        Ok(Some(r)) => r,
        _ => return (StatusCode::NOT_FOUND, "not found").into_response(),
    };

    // 解析变体档位；命中的话找兄弟缩略图，缺失（旧上传/超限）回退原图。
    let base_dir = PathBuf::from(&s.cfg.media_root).join(&sha[..2]);
    let mut path = PathBuf::from(&s.cfg.media_root).join(&row.relative_path);
    let mut mime = row.mime;
    // ETag 带变体后缀，避免不同档位共用缓存。sha256 本身即强 ETag。
    let mut etag_core = sha.clone();

    if let Some(req_slot) = q.s {
        if let Some(slot) = media_thumb::resolve_slot(req_slot) {
            // 缩略图扩展名为 jpg 或 png，落盘时二选一；这里两种都探测。
            for ext in ["jpg", "png"] {
                let cand = base_dir.join(media_thumb::variant_filename(&sha, slot, ext));
                if tokio::fs::try_exists(&cand).await.unwrap_or(false) {
                    path = cand;
                    mime = if ext == "png" {
                        "image/png".into()
                    } else {
                        "image/jpeg".into()
                    };
                    etag_core = format!("{}_s{}", sha, slot);
                    break;
                }
            }
        }
        // 未命中白名单或文件缺失：path/mime 保持原图，正常回退。
    }

    // 内容寻址 ⇒ 不可变。强 ETag = "\"<core>\""。
    let etag = format!("\"{}\"", etag_core);
    if let Some(inm) = headers.get(header::IF_NONE_MATCH).and_then(|v| v.to_str().ok()) {
        // 支持逗号分隔的多值与 W/ 弱标记；只做子串宽松匹配即可（内容不可变）。
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
            tracing::warn!(sha = %sha, path = %path.display(), "media file missing on disk (db/disk divergence)");
            return (StatusCode::NOT_FOUND, "missing").into_response();
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
