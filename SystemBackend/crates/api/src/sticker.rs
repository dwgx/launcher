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
    pub creator_name: Option<String>,   // nickname / username — 客户端显示「by xxx」
    pub install_count: i32,
    pub is_public:    bool,
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

    // 创建者自动 install — 否则 /api/sticker/packs/mine 看不到自己创建的 pack
    // (my_packs 走 user_sticker_packs join，需要这一行)
    sqlx::query!(
        r#"INSERT INTO user_sticker_packs (user_id, pack_id) VALUES ($1, $2)
           ON CONFLICT DO NOTHING"#,
        me, row.id)
        .execute(&s.db).await.ok();

    // creator_name = 当前用户的 nickname（fallback username）
    let me_row = sqlx::query!(
        "SELECT nickname, username FROM users WHERE id = $1", me)
        .fetch_optional(&s.db).await.map_err(internal)?;
    let creator_name = me_row.and_then(|r| r.nickname.or(r.username));

    Ok(Json(PackOut {
        id: row.id.to_string(),
        name: req.name, short_name: req.short_name,
        description: req.description,
        cover_url: None,
        creator_id: Some(me.to_string()),
        creator_name,
        install_count: 0,
        is_public: req.is_public.unwrap_or(true),
        stickers: vec![],
    }))
}

// ---------- 设置 pack 缩略图 ----------
// 客户端先 POST /api/media/upload 拿 media_id，再调这里把它绑到 pack.cover_media_id
#[derive(Deserialize)]
pub struct SetCoverReq {
    pub session_token: String,
    pub pack_id:       Uuid,
    pub media_id:      i64,
}

pub async fn set_pack_cover(
    State(s): State<Arc<AppState>>,
    Json(req): Json<SetCoverReq>,
) -> Result<StatusCode, (StatusCode, String)> {
    let me = auth_user(&s, &req.session_token).await?;
    let pack = sqlx::query!(
        "SELECT creator_id FROM sticker_packs WHERE id = $1", req.pack_id)
        .fetch_optional(&s.db).await.map_err(internal)?
        .ok_or((StatusCode::NOT_FOUND, "pack not found".into()))?;
    if pack.creator_id != Some(me) {
        return Err((StatusCode::FORBIDDEN, "not your pack".into()));
    }
    // 校验 media 存在
    let exists = sqlx::query_scalar!(
        "SELECT 1 as ok FROM media_files WHERE id = $1", req.media_id)
        .fetch_optional(&s.db).await.map_err(internal)?
        .is_some();
    if !exists {
        return Err((StatusCode::BAD_REQUEST, "media not found".into()));
    }
    sqlx::query!(
        "UPDATE sticker_packs SET cover_media_id = $1 WHERE id = $2",
        req.media_id, req.pack_id)
        .execute(&s.db).await.map_err(internal)?;
    Ok(StatusCode::NO_CONTENT)
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
        r#"SELECT p.id, p.name, p.short_name, p.description, p.creator_id,
                  p.install_count, p.is_public,
                  m.sha256 as "cover_sha?", m.mime as "cover_mime?",
                  u.nickname as "creator_nick?", u.username as "creator_user?"
           FROM sticker_packs p
             LEFT JOIN media_files m ON m.id = p.cover_media_id
             LEFT JOIN users u ON u.id = p.creator_id
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

    let creator_name = pack.creator_nick.or(pack.creator_user);
    Ok(Json(PackOut {
        id: pack.id.to_string(),
        name: pack.name, short_name: pack.short_name,
        description: pack.description, cover_url,
        creator_id: pack.creator_id.map(|u| u.to_string()),
        creator_name,
        install_count: pack.install_count,
        is_public: pack.is_public,
        stickers: stickers_out,
    }))
}

// ---------- 通过 short_name 拿 pack（公开分享链接） ----------
// launcher://pack/<short_name> 解码后调这个端点拿详情 + stickers
pub async fn get_pack_by_short(
    State(s): State<Arc<AppState>>,
    Path(short_name): Path<String>,
) -> Result<Json<PackOut>, (StatusCode, String)> {
    let trimmed = short_name.trim().to_lowercase();
    if trimmed.is_empty() || trimmed.len() > 64 {
        return Err((StatusCode::BAD_REQUEST, "bad short_name".into()));
    }
    let id = sqlx::query_scalar!(
        r#"SELECT id FROM sticker_packs
           WHERE short_name = $1 AND is_public = TRUE"#,
        trimmed)
        .fetch_optional(&s.db).await.map_err(internal)?
        .ok_or((StatusCode::NOT_FOUND, "pack not found or not public".into()))?;
    get_pack(State(s), Path(id)).await
}

// ---------- 公开 pack 列表（按安装量降序） ----------
#[derive(Deserialize)]
pub struct ListQ { pub limit: Option<i64> }

#[derive(Serialize)]
pub struct PackBrief {
    pub id: String, pub name: String, pub short_name: Option<String>,
    pub install_count: i32, pub cover_url: Option<String>,
    pub creator_name: Option<String>,   // nickname / username — 客户端显示「by xxx」
    pub is_public:    bool,
    pub is_owner:     bool,             // 当前 session 是否是 pack 创建人
}

pub async fn list_public_packs(
    State(s): State<Arc<AppState>>,
    Query(q): Query<ListQ>,
) -> Result<Json<Vec<PackBrief>>, (StatusCode, String)> {
    let limit = q.limit.unwrap_or(50).clamp(1, 200);
    let rows = sqlx::query!(
        r#"SELECT p.id, p.name, p.short_name, p.install_count, p.is_public,
                  m.sha256 as "cover_sha?", m.mime as "cover_mime?",
                  u.nickname as "creator_nick?", u.username as "creator_user?"
           FROM sticker_packs p
             LEFT JOIN media_files m ON m.id = p.cover_media_id
             LEFT JOIN users u ON u.id = p.creator_id
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
        creator_name: r.creator_nick.or(r.creator_user),
        is_public: r.is_public,
        is_owner: false,    // public list 不区分；客户端不需要这里展示按钮
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

// ---------- 重命名 pack ----------
#[derive(Deserialize)]
pub struct RenamePackReq {
    pub session_token: String,
    pub pack_id:       Uuid,
    pub new_name:      String,
}

pub async fn rename_pack(
    State(s): State<Arc<AppState>>,
    Json(req): Json<RenamePackReq>,
) -> Result<StatusCode, (StatusCode, String)> {
    let me = auth_user(&s, &req.session_token).await?;
    let trimmed = req.new_name.trim();
    if trimmed.is_empty() || trimmed.chars().count() > 24 {
        return Err((StatusCode::BAD_REQUEST, "name 1-24 chars".into()));
    }
    let pack = sqlx::query!(
        "SELECT creator_id FROM sticker_packs WHERE id = $1", req.pack_id)
        .fetch_optional(&s.db).await.map_err(internal)?
        .ok_or((StatusCode::NOT_FOUND, "pack not found".into()))?;
    if pack.creator_id != Some(me) {
        return Err((StatusCode::FORBIDDEN, "not your pack".into()));
    }
    sqlx::query!(
        "UPDATE sticker_packs SET name = $1 WHERE id = $2", trimmed, req.pack_id)
        .execute(&s.db).await.map_err(internal)?;
    Ok(StatusCode::NO_CONTENT)
}

// ---------- 删除 pack ----------
// 级联：sticker_pack_items / user_sticker_packs 由 FK ON DELETE CASCADE 收
// （0005_chat.sql 里 sticker_pack_items.pack_id REFERENCES sticker_packs ON DELETE CASCADE）
#[derive(Deserialize)]
pub struct DeletePackReq {
    pub session_token: String,
    pub pack_id:       Uuid,
}

pub async fn delete_pack(
    State(s): State<Arc<AppState>>,
    Json(req): Json<DeletePackReq>,
) -> Result<StatusCode, (StatusCode, String)> {
    let me = auth_user(&s, &req.session_token).await?;
    let pack = sqlx::query!(
        "SELECT creator_id FROM sticker_packs WHERE id = $1", req.pack_id)
        .fetch_optional(&s.db).await.map_err(internal)?
        .ok_or((StatusCode::NOT_FOUND, "pack not found".into()))?;
    if pack.creator_id != Some(me) {
        return Err((StatusCode::FORBIDDEN, "not your pack".into()));
    }
    // 先把这个 pack 里 user 创建的孤儿 stickers 也删掉（user 视图里"删除分组 = 云端也删"）
    sqlx::query!(
        r#"DELETE FROM stickers
           WHERE creator_id = $1
             AND id IN (SELECT sticker_id FROM sticker_pack_items WHERE pack_id = $2)
             AND id NOT IN (SELECT sticker_id FROM sticker_pack_items WHERE pack_id != $2)"#,
        me, req.pack_id)
        .execute(&s.db).await.ok();
    sqlx::query!("DELETE FROM sticker_packs WHERE id = $1", req.pack_id)
        .execute(&s.db).await.map_err(internal)?;
    Ok(StatusCode::NO_CONTENT)
}

// ---------- 分享 pack ----------
// 切 is_public = true，并自动生成 short_name (8 字 base32) 供分享链接
#[derive(Deserialize)]
pub struct SharePackReq {
    pub session_token: String,
    pub pack_id:       Uuid,
    pub is_public:     Option<bool>,   // 默认 true
}

#[derive(Serialize)]
pub struct ShareResp {
    pub short_name: String,
    pub is_public:  bool,
}

pub async fn share_pack(
    State(s): State<Arc<AppState>>,
    Json(req): Json<SharePackReq>,
) -> Result<Json<ShareResp>, (StatusCode, String)> {
    let me = auth_user(&s, &req.session_token).await?;
    let pack = sqlx::query!(
        r#"SELECT creator_id, short_name, cover_media_id
           FROM sticker_packs WHERE id = $1"#, req.pack_id)
        .fetch_optional(&s.db).await.map_err(internal)?
        .ok_or((StatusCode::NOT_FOUND, "pack not found".into()))?;
    // 取消 owner check — 装了的别人 pack 也允许再分享（链接共用同一个 short_name），
    // 但不允许通过这个端点把别人的 pack 改成 private。
    let want_public = req.is_public.unwrap_or(true);
    let is_owner = pack.creator_id == Some(me);
    if !is_owner && !want_public {
        return Err((StatusCode::FORBIDDEN,
            "non-owner can only share, not unshare".into()));
    }
    // 已有 short_name 就复用；没有就生成（pack_id 取前 12 字 base32-friendly）
    let short = match pack.short_name {
        Some(s) if !s.is_empty() => s,
        _ => {
            let raw = req.pack_id.simple().to_string();
            raw[..12].to_string()
        }
    };
    // 自动给 pack 设 cover：如果 owner 没设过，挑第一个 sticker 当封面（按 sort_order）
    if is_owner && pack.cover_media_id.is_none() {
        if let Ok(Some(first)) = sqlx::query_scalar!(
            r#"SELECT s.media_id FROM sticker_pack_items spi
                 JOIN stickers s ON s.id = spi.sticker_id
               WHERE spi.pack_id = $1
               ORDER BY spi.sort_order, s.created_at LIMIT 1"#,
            req.pack_id)
            .fetch_optional(&s.db).await
        {
            sqlx::query!(
                "UPDATE sticker_packs SET cover_media_id = $1 WHERE id = $2",
                first, req.pack_id)
                .execute(&s.db).await.ok();
        }
    }
    // owner 才允许翻 is_public（非 owner 即使传 want_public=true 也只刷 short_name）
    if is_owner {
        sqlx::query!(
            r#"UPDATE sticker_packs SET is_public = $1, short_name = $2 WHERE id = $3"#,
            want_public, short, req.pack_id)
            .execute(&s.db).await.map_err(internal)?;
    } else {
        // 非 owner 只补 short_name（如果还没有的话）
        sqlx::query!(
            r#"UPDATE sticker_packs SET short_name = COALESCE(short_name, $1)
               WHERE id = $2"#, short, req.pack_id)
            .execute(&s.db).await.map_err(internal)?;
    }
    Ok(Json(ShareResp { short_name: short, is_public: want_public }))
}

// ---------- 我自己上传的 stickers（无关 pack）----------
// 客户端启动时拉一次：把同账号在另一台机器上传的图也同步到本地 userPack。
#[derive(Deserialize)]
pub struct MyStickersQ { pub session_token: String }

pub async fn my_stickers(
    State(s): State<Arc<AppState>>,
    Query(q): Query<MyStickersQ>,
) -> Result<Json<Vec<StickerOut>>, (StatusCode, String)> {
    let me = auth_user(&s, &q.session_token).await?;
    let rows = sqlx::query!(
        r#"SELECT s.id, s.media_id, s.emoji_alias, s.label, s.is_animated,
                  m.sha256, m.mime
           FROM stickers s JOIN media_files m ON m.id = s.media_id
           WHERE s.creator_id = $1
           ORDER BY s.created_at DESC"#, me)
        .fetch_all(&s.db).await.map_err(internal)?;

    Ok(Json(rows.into_iter().map(|r| {
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
    }).collect()))
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
        r#"SELECT p.id, p.name, p.short_name, p.install_count, p.is_public,
                  p.creator_id,
                  m.sha256 as "cover_sha?", m.mime as "cover_mime?",
                  u.nickname as "creator_nick?", u.username as "creator_user?"
           FROM user_sticker_packs usp
             JOIN sticker_packs p   ON p.id = usp.pack_id
             LEFT JOIN media_files m ON m.id = p.cover_media_id
             LEFT JOIN users u ON u.id = p.creator_id
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
        creator_name: r.creator_nick.or(r.creator_user),
        is_public:    r.is_public,
        is_owner:     r.creator_id == Some(me),
    }).collect()))
}

// ---------- 拖拽排序 ----------
// 客户端把 picker 里"我的"已安装 pack 的最新顺序发上来 — 服务器写入
// user_sticker_packs.sort_order，下次 my_packs 按这个顺序返。系统 emoji
// 那个本地伪 pack 没 UUID，不参与，由客户端本地 persist 单独保管。
#[derive(Deserialize)]
pub struct ReorderReq {
    pub session_token: String,
    pub pack_ids:      Vec<Uuid>,
}

pub async fn reorder_packs(
    State(s): State<Arc<AppState>>,
    Json(req): Json<ReorderReq>,
) -> Result<StatusCode, (StatusCode, String)> {
    let me = auth_user(&s, &req.session_token).await?;
    if req.pack_ids.len() > 200 {
        return Err((StatusCode::BAD_REQUEST, "too many".into()));
    }
    let mut tx = s.db.begin().await.map_err(internal)?;
    for (idx, pid) in req.pack_ids.iter().enumerate() {
        sqlx::query!(
            r#"UPDATE user_sticker_packs SET sort_order = $1
               WHERE user_id = $2 AND pack_id = $3"#,
            idx as i32, me, pid)
            .execute(&mut *tx).await.map_err(internal)?;
    }
    tx.commit().await.map_err(internal)?;
    Ok(StatusCode::NO_CONTENT)
}
