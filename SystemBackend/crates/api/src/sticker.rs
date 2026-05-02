// 表情包：单个 sticker（基于 media_files 的 image/gif/webp）+
// pack（用户自由组合 sticker）+ 用户安装 / 卸载 pack

use crate::state::AppState;
use crate::media::auth_user;
use axum::{
    extract::{State, Path, Json, Query},
    http::StatusCode,
};
use serde::{Deserialize, Serialize};
use std::sync::Arc;
use uuid::Uuid;

fn internal<E: std::fmt::Display>(e: E) -> (StatusCode, String) {
    (StatusCode::INTERNAL_SERVER_ERROR, e.to_string())
}

// ---------- 创建 sticker（基于已上传的 media） ----------
#[derive(Deserialize)]
pub struct CreateStickerReq {
    pub session_token: String,
    pub media_id:      i64,
    pub emoji_alias:   Option<String>,
    pub label:         Option<String>,
    pub is_animated:   Option<bool>,
}

#[derive(Serialize)]
pub struct StickerOut {
    pub id:          String,
    pub media_id:    i64,
    pub media_url:   String,
    pub emoji_alias: Option<String>,
    pub label:       Option<String>,
    pub is_animated: bool,
}

pub async fn create_sticker(
    State(s): State<Arc<AppState>>,
    Json(req): Json<CreateStickerReq>,
) -> Result<Json<StickerOut>, (StatusCode, String)> {
    let me = auth_user(&s, &req.session_token).await?;

    // 50/user 限制（admin 可在 config.toml 改 sticker_per_user_limit）
    let my_count: i64 = sqlx::query_scalar!(
        "SELECT COUNT(*) FROM stickers WHERE creator_id = $1", me)
        .fetch_one(&s.db).await.map_err(internal)?
        .unwrap_or(0);
    if my_count >= s.cfg.sticker_per_user_limit {
        return Err((StatusCode::PAYLOAD_TOO_LARGE,
            format!("表情包已达上限 {}/{}", my_count, s.cfg.sticker_per_user_limit)));
    }

    let media = sqlx::query!(
        "SELECT sha256, mime FROM media_files WHERE id = $1", req.media_id)
        .fetch_optional(&s.db).await.map_err(internal)?
        .ok_or((StatusCode::BAD_REQUEST, "media not found".into()))?;

    let row = sqlx::query!(
        r#"INSERT INTO stickers (media_id, emoji_alias, label, creator_id, is_animated)
           VALUES ($1, $2, $3, $4, $5) RETURNING id"#,
        req.media_id, req.emoji_alias, req.label, me,
        req.is_animated.unwrap_or(false))
        .fetch_one(&s.db).await.map_err(internal)?;

    let ext = match media.mime.as_str() {
        "image/png" => "png", "image/jpeg" => "jpg", "image/gif" => "gif",
        "image/webp" => "webp", _ => "bin"
    };
    Ok(Json(StickerOut {
        id: row.id.to_string(),
        media_id: req.media_id,
        media_url: format!("/api/media/{}/file.{}", media.sha256, ext),
        emoji_alias: req.emoji_alias,
        label: req.label,
        is_animated: req.is_animated.unwrap_or(false),
    }))
}

// ---------- 创建 pack ----------
#[derive(Deserialize)]
pub struct CreatePackReq {
    pub session_token: String,
    pub name:          String,
    pub short_name:    Option<String>,
    pub description:   Option<String>,
    pub cover_media_id: Option<i64>,
    pub is_public:     Option<bool>,
}

#[derive(Serialize)]
pub struct PackOut {
    pub id:           String,
    pub name:         String,
    pub short_name:   Option<String>,
    pub description:  Option<String>,
    pub cover_url:    Option<String>,
    pub creator_id:   Option<String>,
    pub install_count: i32,
    pub stickers:     Vec<StickerOut>,
}

pub async fn create_pack(
    State(s): State<Arc<AppState>>,
    Json(req): Json<CreatePackReq>,
) -> Result<Json<PackOut>, (StatusCode, String)> {
    let me = auth_user(&s, &req.session_token).await?;
    if req.name.trim().is_empty() {
        return Err((StatusCode::BAD_REQUEST, "name required".into()));
    }
    let row = sqlx::query!(
        r#"INSERT INTO sticker_packs (name, short_name, description, cover_media_id, creator_id, is_public)
           VALUES ($1, $2, $3, $4, $5, $6) RETURNING id"#,
        req.name, req.short_name, req.description, req.cover_media_id, me,
        req.is_public.unwrap_or(true))
        .fetch_one(&s.db).await.map_err(internal)?;

    Ok(Json(PackOut {
        id: row.id.to_string(),
        name: req.name, short_name: req.short_name,
        description: req.description,
        cover_url: None,
        creator_id: Some(me.to_string()),
        install_count: 0,
        stickers: vec![],
    }))
}

// ---------- 给 pack 加 sticker ----------
#[derive(Deserialize)]
pub struct AddItemReq {
    pub session_token: String,
    pub pack_id:       Uuid,
    pub sticker_id:    Uuid,
    pub sort_order:    Option<i32>,
}

pub async fn add_to_pack(
    State(s): State<Arc<AppState>>,
    Json(req): Json<AddItemReq>,
) -> Result<StatusCode, (StatusCode, String)> {
    let me = auth_user(&s, &req.session_token).await?;
    let pack = sqlx::query!(
        "SELECT creator_id FROM sticker_packs WHERE id = $1", req.pack_id)
        .fetch_optional(&s.db).await.map_err(internal)?
        .ok_or((StatusCode::NOT_FOUND, "pack not found".into()))?;
    if pack.creator_id != Some(me) {
        return Err((StatusCode::FORBIDDEN, "not your pack".into()));
    }
    sqlx::query!(
        r#"INSERT INTO sticker_pack_items (pack_id, sticker_id, sort_order)
           VALUES ($1, $2, $3) ON CONFLICT DO NOTHING"#,
        req.pack_id, req.sticker_id, req.sort_order.unwrap_or(0))
        .execute(&s.db).await.map_err(internal)?;
    Ok(StatusCode::NO_CONTENT)
}

// ---------- 移除 sticker from pack ----------
#[derive(Deserialize)]
pub struct RemoveItemReq {
    pub session_token: String,
    pub pack_id:       Uuid,
    pub sticker_id:    Uuid,
}

pub async fn remove_from_pack(
    State(s): State<Arc<AppState>>,
    Json(req): Json<RemoveItemReq>,
) -> Result<StatusCode, (StatusCode, String)> {
    let me = auth_user(&s, &req.session_token).await?;
    let pack = sqlx::query!(
        "SELECT creator_id FROM sticker_packs WHERE id = $1", req.pack_id)
        .fetch_optional(&s.db).await.map_err(internal)?
        .ok_or((StatusCode::NOT_FOUND, "pack not found".into()))?;
    if pack.creator_id != Some(me) {
        return Err((StatusCode::FORBIDDEN, "not your pack".into()));
    }
    sqlx::query!(
        "DELETE FROM sticker_pack_items WHERE pack_id = $1 AND sticker_id = $2",
        req.pack_id, req.sticker_id).execute(&s.db).await.map_err(internal)?;
    Ok(StatusCode::NO_CONTENT)
}

// ---------- 看 pack 详情 ----------
pub async fn get_pack(
    State(s): State<Arc<AppState>>,
    Path(id): Path<Uuid>,
) -> Result<Json<PackOut>, (StatusCode, String)> {
    let pack = sqlx::query!(
        r#"SELECT p.id, p.name, p.short_name, p.description, p.creator_id, p.install_count,
                  m.sha256 as "cover_sha?", m.mime as "cover_mime?"
           FROM sticker_packs p LEFT JOIN media_files m ON m.id = p.cover_media_id
           WHERE p.id = $1"#, id)
        .fetch_optional(&s.db).await.map_err(internal)?
        .ok_or((StatusCode::NOT_FOUND, "pack not found".into()))?;

    let stickers = sqlx::query!(
        r#"SELECT s.id, s.media_id, s.emoji_alias, s.label, s.is_animated,
                  m.sha256, m.mime
           FROM sticker_pack_items spi
             JOIN stickers s    ON s.id = spi.sticker_id
             JOIN media_files m ON m.id = s.media_id
           WHERE spi.pack_id = $1
           ORDER BY spi.sort_order"#, id)
        .fetch_all(&s.db).await.map_err(internal)?;

    let stickers_out: Vec<StickerOut> = stickers.into_iter().map(|r| {
        let ext = match r.mime.as_str() {
            "image/png" => "png", "image/jpeg" => "jpg", "image/gif" => "gif",
            "image/webp" => "webp", _ => "bin"
        };
        StickerOut {
            id: r.id.to_string(), media_id: r.media_id,
            media_url: format!("/api/media/{}/file.{}", r.sha256, ext),
            emoji_alias: r.emoji_alias, label: r.label,
            is_animated: r.is_animated,
        }
    }).collect();

    let cover_url = pack.cover_sha.zip(pack.cover_mime).map(|(sha, mime)| {
        let ext = match mime.as_str() {
            "image/png" => "png", "image/jpeg" => "jpg", "image/gif" => "gif",
            "image/webp" => "webp", _ => "bin"
        };
        format!("/api/media/{}/file.{}", sha, ext)
    });

    Ok(Json(PackOut {
        id: pack.id.to_string(),
        name: pack.name, short_name: pack.short_name,
        description: pack.description, cover_url,
        creator_id: pack.creator_id.map(|u| u.to_string()),
        install_count: pack.install_count,
        stickers: stickers_out,
    }))
}

// ---------- 公开 pack 列表（按安装量降序） ----------
#[derive(Deserialize)]
pub struct ListQ { pub limit: Option<i64> }

#[derive(Serialize)]
pub struct PackBrief {
    pub id: String, pub name: String, pub short_name: Option<String>,
    pub install_count: i32, pub cover_url: Option<String>,
}

pub async fn list_public_packs(
    State(s): State<Arc<AppState>>,
    Query(q): Query<ListQ>,
) -> Result<Json<Vec<PackBrief>>, (StatusCode, String)> {
    let limit = q.limit.unwrap_or(50).clamp(1, 200);
    let rows = sqlx::query!(
        r#"SELECT p.id, p.name, p.short_name, p.install_count,
                  m.sha256 as "cover_sha?", m.mime as "cover_mime?"
           FROM sticker_packs p LEFT JOIN media_files m ON m.id = p.cover_media_id
           WHERE p.is_public = TRUE
           ORDER BY p.install_count DESC, p.created_at DESC LIMIT $1"#, limit)
        .fetch_all(&s.db).await.map_err(internal)?;

    Ok(Json(rows.into_iter().map(|r| PackBrief {
        id: r.id.to_string(), name: r.name, short_name: r.short_name,
        install_count: r.install_count,
        cover_url: r.cover_sha.zip(r.cover_mime).map(|(sha, mime)| {
            let ext = match mime.as_str() {
                "image/png" => "png", "image/jpeg" => "jpg", "image/gif" => "gif",
                "image/webp" => "webp", _ => "bin"
            };
            format!("/api/media/{}/file.{}", sha, ext)
        }),
    }).collect()))
}

// ---------- 安装 / 卸载 ----------
#[derive(Deserialize)]
pub struct InstallReq { pub session_token: String, pub pack_id: Uuid }

pub async fn install(
    State(s): State<Arc<AppState>>,
    Json(req): Json<InstallReq>,
) -> Result<StatusCode, (StatusCode, String)> {
    let me = auth_user(&s, &req.session_token).await?;
    let r = sqlx::query!(
        r#"INSERT INTO user_sticker_packs (user_id, pack_id) VALUES ($1, $2)
           ON CONFLICT DO NOTHING"#, me, req.pack_id)
        .execute(&s.db).await.map_err(internal)?;
    if r.rows_affected() > 0 {
        sqlx::query!(
            "UPDATE sticker_packs SET install_count = install_count + 1 WHERE id = $1",
            req.pack_id).execute(&s.db).await.ok();
    }
    Ok(StatusCode::NO_CONTENT)
}

pub async fn uninstall(
    State(s): State<Arc<AppState>>,
    Json(req): Json<InstallReq>,
) -> Result<StatusCode, (StatusCode, String)> {
    let me = auth_user(&s, &req.session_token).await?;
    let r = sqlx::query!(
        "DELETE FROM user_sticker_packs WHERE user_id = $1 AND pack_id = $2",
        me, req.pack_id).execute(&s.db).await.map_err(internal)?;
    if r.rows_affected() > 0 {
        sqlx::query!(
            "UPDATE sticker_packs SET install_count = GREATEST(install_count - 1, 0) WHERE id = $1",
            req.pack_id).execute(&s.db).await.ok();
    }
    Ok(StatusCode::NO_CONTENT)
}

// ---------- 我的 packs ----------
#[derive(Deserialize)]
pub struct MyPacksQ { pub session_token: String }

pub async fn my_packs(
    State(s): State<Arc<AppState>>,
    Query(q): Query<MyPacksQ>,
) -> Result<Json<Vec<PackBrief>>, (StatusCode, String)> {
    let me = auth_user(&s, &q.session_token).await?;
    let rows = sqlx::query!(
        r#"SELECT p.id, p.name, p.short_name, p.install_count,
                  m.sha256 as "cover_sha?", m.mime as "cover_mime?"
           FROM user_sticker_packs usp
             JOIN sticker_packs p   ON p.id = usp.pack_id
             LEFT JOIN media_files m ON m.id = p.cover_media_id
           WHERE usp.user_id = $1
           ORDER BY usp.sort_order, usp.installed_at DESC"#, me)
        .fetch_all(&s.db).await.map_err(internal)?;

    Ok(Json(rows.into_iter().map(|r| PackBrief {
        id: r.id.to_string(), name: r.name, short_name: r.short_name,
        install_count: r.install_count,
        cover_url: r.cover_sha.zip(r.cover_mime).map(|(sha, mime)| {
            let ext = match mime.as_str() {
                "image/png" => "png", "image/jpeg" => "jpg", "image/gif" => "gif",
                "image/webp" => "webp", _ => "bin"
            };
            format!("/api/media/{}/file.{}", sha, ext)
        }),
    }).collect()))
}
