// 聊天：DM / Group / Channel + messages CRUD + 实时通过 ws.rs 推送

use crate::state::AppState;
use crate::media::auth_user;
use crate::ws;
use axum::{
    extract::{Json, Query, State},
    http::StatusCode,
};
use serde::{Deserialize, Serialize};
use std::sync::Arc;
use sqlx::{postgres::PgRow, Row};
use uuid::Uuid;
use chrono::{DateTime, Utc};

fn internal<E: std::fmt::Display>(e: E) -> (StatusCode, String) {
    (StatusCode::INTERNAL_SERVER_ERROR, e.to_string())
}

async fn can_access_chat(
    state: &AppState,
    user_id: Uuid,
    chat_id: Uuid,
) -> Result<bool, (StatusCode, String)> {
    let chat_meta = sqlx::query!(
        "SELECT kind, is_official FROM chats WHERE id = $1", chat_id)
        .fetch_optional(&state.db).await.map_err(internal)?
        .ok_or((StatusCode::NOT_FOUND, "chat not found".into()))?;
    if chat_meta.kind == "channel" && chat_meta.is_official {
        return Ok(true);
    }
    let member = sqlx::query_scalar!(
        "SELECT 1 as ok FROM chat_members WHERE chat_id=$1 AND user_id=$2",
        chat_id, user_id)
        .fetch_optional(&state.db).await.map_err(internal)?;
    Ok(member.is_some())
}

async fn message_chat(
    state: &AppState,
    message_id: i64,
) -> Result<Uuid, (StatusCode, String)> {
    sqlx::query_scalar!("SELECT chat_id FROM messages WHERE id = $1", message_id)
        .fetch_optional(&state.db).await.map_err(internal)?
        .ok_or((StatusCode::NOT_FOUND, "msg not found".into()))
}

async fn create_event(
    state: &AppState,
    chat_id: Option<Uuid>,
    event_type: &str,
    message_id: Option<i64>,
    actor_id: Option<Uuid>,
    payload: serde_json::Value,
) -> Result<i64, (StatusCode, String)> {
    let row = sqlx::query(
        r#"INSERT INTO chat_events (chat_id, event_type, message_id, actor_id, payload)
           VALUES ($1, $2, $3, $4, $5)
           RETURNING id"#)
        .bind(chat_id)
        .bind(event_type)
        .bind(message_id)
        .bind(actor_id)
        .bind(payload)
        .fetch_one(&state.db).await.map_err(internal)?;
    Ok(row.try_get("id").map_err(internal)?)
}

async fn message_event_id(
    state: &AppState,
    message_id: i64,
) -> Result<Option<i64>, (StatusCode, String)> {
    let row = sqlx::query(
        r#"SELECT id FROM chat_events
           WHERE message_id = $1 AND event_type = 'message'
           ORDER BY id ASC LIMIT 1"#)
        .bind(message_id)
        .fetch_optional(&state.db).await.map_err(internal)?;
    row.map(|r| r.try_get("id").map_err(internal)).transpose()
}

fn message_out_from_row(
    row: &PgRow,
    chat_id: Uuid,
    sender_id: Option<Uuid>,
    event_id: Option<i64>,
) -> Result<MessageOut, (StatusCode, String)> {
    Ok(MessageOut {
        id: row.try_get("id").map_err(internal)?,
        chat_id: chat_id.to_string(),
        sender_id: sender_id.map(|u| u.to_string()),
        msg_type: row.try_get("msg_type").map_err(internal)?,
        payload: row.try_get("payload").map_err(internal)?,
        reply_to_id: row.try_get("reply_to_id").map_err(internal)?,
        created_at: row.try_get::<DateTime<Utc>, _>("created_at").map_err(internal)?.timestamp(),
        edited_at: row.try_get::<Option<DateTime<Utc>>, _>("edited_at").map_err(internal)?.map(|t| t.timestamp()),
        deleted: row.try_get::<Option<DateTime<Utc>>, _>("deleted_at").map_err(internal)?.is_some(),
        client_msg_id: row.try_get::<Option<Uuid>, _>("client_msg_id").map_err(internal)?.map(|v| v.to_string()),
        sender_device_id: row.try_get("sender_device_id").map_err(internal)?,
        event_id,
    })
}

// ---------------- 创建 / 打开 DM ----------------
#[derive(Deserialize)]
pub struct OpenDmReq { pub session_token: String, pub other_user_id: Uuid }

#[derive(Serialize)]
pub struct ChatBrief {
    pub id:       String,
    pub kind:     String,
    pub title:    Option<String>,
    pub last_message_at: Option<i64>,
}

pub async fn open_dm(
    State(s): State<Arc<AppState>>,
    Json(req): Json<OpenDmReq>,
) -> Result<Json<ChatBrief>, (StatusCode, String)> {
    let me = auth_user(&s, &req.session_token).await?;
    if me == req.other_user_id {
        return Err((StatusCode::BAD_REQUEST, "cannot DM self".into()));
    }
    let (lo, hi) = if me < req.other_user_id { (me, req.other_user_id) } else { (req.other_user_id, me) };

    // 已存在？
    if let Some(row) = sqlx::query!(
        r#"SELECT c.id, c.title, c.last_message_at
           FROM chat_dm_index d JOIN chats c ON c.id = d.chat_id
           WHERE d.user_lo = $1 AND d.user_hi = $2"#, lo, hi)
        .fetch_optional(&s.db).await.map_err(internal)? {
        return Ok(Json(ChatBrief {
            id: row.id.to_string(), kind: "dm".into(),
            title: row.title,
            last_message_at: row.last_message_at.map(|t| t.timestamp()),
        }));
    }

    let chat = sqlx::query!(
        "INSERT INTO chats (kind, created_by) VALUES ('dm', $1) RETURNING id", me)
        .fetch_one(&s.db).await.map_err(internal)?;
    sqlx::query!(
        "INSERT INTO chat_dm_index (chat_id, user_lo, user_hi) VALUES ($1, $2, $3)",
        chat.id, lo, hi).execute(&s.db).await.map_err(internal)?;
    sqlx::query!(
        r#"INSERT INTO chat_members (chat_id, user_id, role) VALUES ($1, $2, 'member'), ($1, $3, 'member')"#,
        chat.id, me, req.other_user_id).execute(&s.db).await.map_err(internal)?;

    Ok(Json(ChatBrief {
        id: chat.id.to_string(), kind: "dm".into(),
        title: None, last_message_at: None,
    }))
}

// ---------------- 创建 group/channel ----------------
#[derive(Deserialize)]
pub struct CreateGroupReq {
    pub session_token: String,
    pub title:         String,
    pub kind:          String,                // 'group' | 'channel'
    pub member_ids:    Vec<Uuid>,
}

pub async fn create_group(
    State(s): State<Arc<AppState>>,
    Json(req): Json<CreateGroupReq>,
) -> Result<Json<ChatBrief>, (StatusCode, String)> {
    let me = auth_user(&s, &req.session_token).await?;
    if !["group","channel"].contains(&req.kind.as_str()) {
        return Err((StatusCode::BAD_REQUEST, "kind must be group or channel".into()));
    }
    if req.title.trim().is_empty() {
        return Err((StatusCode::BAD_REQUEST, "title required".into()));
    }
    let chat = sqlx::query!(
        "INSERT INTO chats (kind, title, created_by) VALUES ($1, $2, $3) RETURNING id",
        req.kind, req.title, me).fetch_one(&s.db).await.map_err(internal)?;

    // owner = me
    sqlx::query!(
        "INSERT INTO chat_members (chat_id, user_id, role) VALUES ($1, $2, 'owner')",
        chat.id, me).execute(&s.db).await.map_err(internal)?;
    for mid in req.member_ids {
        if mid == me { continue; }
        let _ = sqlx::query!(
            "INSERT INTO chat_members (chat_id, user_id, role) VALUES ($1, $2, 'member') ON CONFLICT DO NOTHING",
            chat.id, mid).execute(&s.db).await;
    }

    Ok(Json(ChatBrief {
        id: chat.id.to_string(), kind: req.kind,
        title: Some(req.title), last_message_at: None,
    }))
}

// ---------------- 我的会话列表 ----------------
#[derive(Deserialize)]
pub struct ListChatsQ { pub session_token: String }

#[derive(Serialize)]
pub struct ChatListItem {
    pub id:               String,
    pub kind:             String,
    pub title:            Option<String>,
    pub avatar_url:       Option<String>,
    pub last_message:     Option<MessageOut>,
    pub last_message_at:  Option<i64>,
    pub unread_count:     i64,
}

// ---------------- 官方频道列表 (任何登录用户可见) ----------------
#[derive(Deserialize)]
pub struct OfficialQ { pub session_token: String }

#[derive(Serialize)]
pub struct OfficialChan {
    pub id:           String,
    pub slug:         String,
    pub title:        String,
    pub group_label:  Option<String>,
    pub write_role:   String,
}

pub async fn list_official(
    State(s): State<Arc<AppState>>,
    Query(q): Query<OfficialQ>,
) -> Result<Json<Vec<OfficialChan>>, (StatusCode, String)> {
    let _me = auth_user(&s, &q.session_token).await?;
    let rows = sqlx::query!(
        r#"SELECT id, slug, title, group_label, write_role
           FROM chats
           WHERE kind = 'channel' AND is_official = TRUE
           ORDER BY group_label NULLS LAST, title"#)
        .fetch_all(&s.db).await.map_err(internal)?;
    Ok(Json(rows.into_iter().map(|r| OfficialChan {
        id: r.id.to_string(),
        slug: r.slug.unwrap_or_default(),
        title: r.title.unwrap_or_default(),
        group_label: r.group_label,
        write_role: r.write_role,
    }).collect()))
}

pub async fn list_chats(
    State(s): State<Arc<AppState>>,
    Query(q): Query<ListChatsQ>,
) -> Result<Json<Vec<ChatListItem>>, (StatusCode, String)> {
    let me = auth_user(&s, &q.session_token).await?;
    let rows = sqlx::query!(
        r#"SELECT c.id, c.kind, c.title, c.last_message_at,
                  cm.last_read_message_id
           FROM chat_members cm JOIN chats c ON c.id = cm.chat_id
           WHERE cm.user_id = $1
           ORDER BY c.last_message_at DESC NULLS LAST LIMIT 100"#, me)
        .fetch_all(&s.db).await.map_err(internal)?;

    let mut out = Vec::with_capacity(rows.len());
    for r in rows {
        // 最后一条消息
        let last = sqlx::query!(
            r#"SELECT id, sender_id, msg_type, payload, created_at
               FROM messages WHERE chat_id = $1 AND deleted_at IS NULL
               ORDER BY id DESC LIMIT 1"#, r.id)
            .fetch_optional(&s.db).await.map_err(internal)?;
        let last_out = last.map(|m| MessageOut {
            id: m.id, chat_id: r.id.to_string(),
            sender_id: m.sender_id.map(|u| u.to_string()),
            msg_type: m.msg_type, payload: m.payload,
            reply_to_id: None,
            created_at: m.created_at.timestamp(),
            edited_at: None, deleted: false,
            client_msg_id: None,
            sender_device_id: None,
            event_id: None,
        });
        // 未读数
        let unread = if let Some(lr) = r.last_read_message_id {
            sqlx::query_scalar!(
                "SELECT COUNT(*) FROM messages WHERE chat_id=$1 AND id > $2 AND deleted_at IS NULL",
                r.id, lr).fetch_one(&s.db).await.unwrap_or(Some(0)).unwrap_or(0)
        } else {
            sqlx::query_scalar!(
                "SELECT COUNT(*) FROM messages WHERE chat_id=$1 AND deleted_at IS NULL",
                r.id).fetch_one(&s.db).await.unwrap_or(Some(0)).unwrap_or(0)
        };
        out.push(ChatListItem {
            id: r.id.to_string(), kind: r.kind, title: r.title,
            avatar_url: None,
            last_message: last_out,
            last_message_at: r.last_message_at.map(|t| t.timestamp()),
            unread_count: unread,
        });
    }
    Ok(Json(out))
}

// ---------------- 发消息 ----------------
#[derive(Deserialize)]
pub struct SendReq {
    pub session_token: String,
    pub chat_id:       Uuid,
    pub msg_type:      String,                 // text/image/video/gif/sticker/pack_share
    pub payload:       serde_json::Value,
    pub reply_to_id:   Option<i64>,
    pub client_msg_id: Option<Uuid>,
    pub device_id:     Option<String>,
}

#[derive(Serialize, Clone)]
pub struct MessageOut {
    pub id:          i64,
    pub chat_id:     String,
    pub sender_id:   Option<String>,
    pub msg_type:    String,
    pub payload:     serde_json::Value,
    pub reply_to_id: Option<i64>,
    pub created_at:  i64,
    pub edited_at:   Option<i64>,
    pub deleted:     bool,
    pub client_msg_id: Option<String>,
    pub sender_device_id: Option<String>,
    pub event_id:    Option<i64>,
}

#[derive(Serialize)]
pub struct ChatEventOut {
    pub id:         i64,
    pub chat_id:    Option<String>,
    pub event_type: String,
    pub message_id: Option<i64>,
    pub actor_id:   Option<String>,
    pub payload:    serde_json::Value,
    pub created_at: i64,
}

pub async fn send(
    State(s): State<Arc<AppState>>,
    Json(req): Json<SendReq>,
) -> Result<Json<MessageOut>, (StatusCode, String)> {
    let me = auth_user(&s, &req.session_token).await?;
    let allowed_types = ["text","image","video","gif","sticker","pack_share"];
    if !allowed_types.contains(&req.msg_type.as_str()) {
        return Err((StatusCode::BAD_REQUEST, "bad msg_type".into()));
    }
    // 拿 chat 元信息（kind / write_role / is_official）
    let chat_meta = sqlx::query!(
        r#"SELECT kind, write_role, is_official FROM chats WHERE id = $1"#,
        req.chat_id)
        .fetch_optional(&s.db).await.map_err(internal)?
        .ok_or((StatusCode::NOT_FOUND, "chat not found".into()))?;
    // 拿当前用户角色
    let user_admin = sqlx::query_scalar!(
        "SELECT is_admin FROM users WHERE id = $1", me)
        .fetch_optional(&s.db).await.map_err(internal)?
        .unwrap_or(false);

    if chat_meta.kind == "channel" && chat_meta.is_official {
        // 官方频道按 write_role 控权
        if chat_meta.write_role == "admin_only" && !user_admin {
            return Err((StatusCode::FORBIDDEN,
                "此频道只允许管理员发言".into()));
        }
        // user 频道：所有登录用户可发，无需 chat_members
    } else {
        // dm / group / 非官方 channel — 仍需 member 关系
        let member = sqlx::query_scalar!(
            "SELECT 1 as ok FROM chat_members WHERE chat_id=$1 AND user_id=$2",
            req.chat_id, me)
            .fetch_optional(&s.db).await.map_err(internal)?;
        if member.is_none() {
            return Err((StatusCode::FORBIDDEN, "not a member".into()));
        }
    }

    let device_id = req.device_id
        .as_deref()
        .map(str::trim)
        .filter(|s| !s.is_empty())
        .map(|s| s.chars().take(128).collect::<String>());

    let mut inserted = true;
    let row = if let Some(client_msg_id) = req.client_msg_id {
        if let Some(existing) = sqlx::query(
            r#"SELECT id, created_at, msg_type, payload, reply_to_id, edited_at, deleted_at,
                      client_msg_id, sender_device_id
               FROM messages
               WHERE chat_id = $1 AND sender_id = $2 AND client_msg_id = $3"#)
            .bind(req.chat_id)
            .bind(me)
            .bind(client_msg_id)
            .fetch_optional(&s.db).await.map_err(internal)?
        {
            inserted = false;
            existing
        } else {
            sqlx::query(
            r#"INSERT INTO messages
                  (chat_id, sender_id, msg_type, payload, reply_to_id, client_msg_id, sender_device_id)
               VALUES ($1, $2, $3, $4, $5, $6, $7)
               RETURNING id, created_at, msg_type, payload, reply_to_id, edited_at, deleted_at,
                         client_msg_id, sender_device_id"#)
            .bind(req.chat_id)
            .bind(me)
            .bind(&req.msg_type)
            .bind(&req.payload)
            .bind(req.reply_to_id)
            .bind(client_msg_id)
            .bind(&device_id)
            .fetch_one(&s.db).await.map_err(internal)?
        }
    } else {
        sqlx::query(
            r#"INSERT INTO messages
                  (chat_id, sender_id, msg_type, payload, reply_to_id, sender_device_id)
               VALUES ($1, $2, $3, $4, $5, $6)
               RETURNING id, created_at, msg_type, payload, reply_to_id, edited_at, deleted_at,
                         client_msg_id, sender_device_id"#)
            .bind(req.chat_id)
            .bind(me)
            .bind(&req.msg_type)
            .bind(&req.payload)
            .bind(req.reply_to_id)
            .bind(&device_id)
            .fetch_one(&s.db).await.map_err(internal)?
    };

    let message_id: i64 = row.try_get("id").map_err(internal)?;
    let existing_event_id = message_event_id(&s, message_id).await?;
    let out_without_event = message_out_from_row(&row, req.chat_id, Some(me), existing_event_id)?;

    if !inserted {
        return Ok(Json(out_without_event));
    }

    let event_id = create_event(
        &s,
        Some(req.chat_id),
        "message",
        Some(message_id),
        Some(me),
        serde_json::json!({ "message": &out_without_event }),
    ).await?;

    let mut out = out_without_event;
    out.event_id = Some(event_id);

    // 推送：官方频道无 chat_members 行，要广播给所有在线 WS；非官方走成员表
    let payload = serde_json::json!({
        "type": "event",
        "event_id": event_id,
        "event_type": "message",
        "chat_id": req.chat_id.to_string(),
        "server_time": out.created_at,
        "data": &out,
        "legacy_type": "message"
    });
    if chat_meta.kind == "channel" && chat_meta.is_official {
        ws::broadcast_all(&s, &payload);
    } else {
        let members = sqlx::query_scalar!(
            "SELECT user_id FROM chat_members WHERE chat_id = $1", req.chat_id)
            .fetch_all(&s.db).await.map_err(internal)?;
        for uid in members {
            ws::push_to(&s, uid, &payload);
        }
    }
    Ok(Json(out))
}

// ---------------- 历史消息 ----------------
#[derive(Deserialize)]
pub struct HistoryQ {
    pub session_token: String,
    pub chat_id:       Uuid,
    pub before_id:     Option<i64>,
    pub limit:         Option<i64>,
}

pub async fn history(
    State(s): State<Arc<AppState>>,
    Query(q): Query<HistoryQ>,
) -> Result<Json<Vec<MessageOut>>, (StatusCode, String)> {
    let me = auth_user(&s, &q.session_token).await?;
    // 官方频道任何登录用户可读；其他需 member
    let chat_meta = sqlx::query!(
        "SELECT kind, is_official FROM chats WHERE id = $1", q.chat_id)
        .fetch_optional(&s.db).await.map_err(internal)?
        .ok_or((StatusCode::NOT_FOUND, "chat not found".into()))?;
    if !(chat_meta.kind == "channel" && chat_meta.is_official) {
        let member = sqlx::query_scalar!(
            "SELECT 1 as ok FROM chat_members WHERE chat_id=$1 AND user_id=$2", q.chat_id, me)
            .fetch_optional(&s.db).await.map_err(internal)?;
        if member.is_none() {
            return Err((StatusCode::FORBIDDEN, "not a member".into()));
        }
    }
    let limit = q.limit.unwrap_or(50).clamp(1, 200);
    let before = q.before_id.unwrap_or(i64::MAX);
    let rows = sqlx::query(
        r#"SELECT id, sender_id, msg_type, payload, reply_to_id, created_at, edited_at, deleted_at,
                  client_msg_id, sender_device_id
           FROM messages WHERE chat_id = $1 AND id < $2
           ORDER BY id DESC LIMIT $3"#)
        .bind(q.chat_id)
        .bind(before)
        .bind(limit)
        .fetch_all(&s.db).await.map_err(internal)?;

    let mut out = Vec::with_capacity(rows.len());
    for r in rows {
        let sender_id = r.try_get::<Option<Uuid>, _>("sender_id").map_err(internal)?;
        out.push(message_out_from_row(&r, q.chat_id, sender_id, None)?);
    }
    Ok(Json(out))
}

// ---------------- event sync ----------------
#[derive(Deserialize)]
pub struct SyncQ {
    pub session_token:   String,
    pub after_event_id:  Option<i64>,
    pub limit:           Option<i64>,
}

pub async fn sync_events(
    State(s): State<Arc<AppState>>,
    Query(q): Query<SyncQ>,
) -> Result<Json<Vec<ChatEventOut>>, (StatusCode, String)> {
    let me = auth_user(&s, &q.session_token).await?;
    let after = q.after_event_id.unwrap_or(0).max(0);
    let limit = q.limit.unwrap_or(100).clamp(1, 500);
    let rows = sqlx::query(
        r#"SELECT e.id, e.chat_id, e.event_type, e.message_id, e.actor_id, e.payload, e.created_at
           FROM chat_events e
           LEFT JOIN chats c ON c.id = e.chat_id
           WHERE e.id > $1
             AND (
                e.chat_id IS NULL
                OR (c.kind = 'channel' AND c.is_official = TRUE)
                OR EXISTS (
                    SELECT 1 FROM chat_members cm
                    WHERE cm.chat_id = e.chat_id AND cm.user_id = $2
                )
             )
           ORDER BY e.id ASC
           LIMIT $3"#)
        .bind(after)
        .bind(me)
        .bind(limit)
        .fetch_all(&s.db).await.map_err(internal)?;

    let mut out = Vec::with_capacity(rows.len());
    for r in rows {
        out.push(ChatEventOut {
            id: r.try_get("id").map_err(internal)?,
            chat_id: r.try_get::<Option<Uuid>, _>("chat_id").map_err(internal)?.map(|v| v.to_string()),
            event_type: r.try_get("event_type").map_err(internal)?,
            message_id: r.try_get("message_id").map_err(internal)?,
            actor_id: r.try_get::<Option<Uuid>, _>("actor_id").map_err(internal)?.map(|v| v.to_string()),
            payload: r.try_get("payload").map_err(internal)?,
            created_at: r.try_get::<DateTime<Utc>, _>("created_at").map_err(internal)?.timestamp(),
        });
    }
    Ok(Json(out))
}

// ---------------- 标记已读 ----------------
#[derive(Deserialize)]
pub struct ReadReq { pub session_token: String, pub chat_id: Uuid, pub up_to_message_id: i64 }

pub async fn mark_read(
    State(s): State<Arc<AppState>>,
    Json(req): Json<ReadReq>,
) -> Result<StatusCode, (StatusCode, String)> {
    let me = auth_user(&s, &req.session_token).await?;
    if !can_access_chat(&s, me, req.chat_id).await? {
        return Err((StatusCode::FORBIDDEN, "not a member".into()));
    }
    sqlx::query!(
        "UPDATE chat_members SET last_read_message_id = $1 WHERE chat_id = $2 AND user_id = $3",
        req.up_to_message_id, req.chat_id, me)
        .execute(&s.db).await.map_err(internal)?;
    let _ = create_event(
        &s,
        Some(req.chat_id),
        "read",
        Some(req.up_to_message_id),
        Some(me),
        serde_json::json!({ "up_to_message_id": req.up_to_message_id }),
    ).await;
    Ok(StatusCode::NO_CONTENT)
}

// ---------------- emoji react ----------------
#[derive(Deserialize)]
pub struct ReactReq {
    pub session_token: String,
    pub message_id:    i64,
    pub emoji:         String,
    pub remove:        bool,
}

pub async fn react(
    State(s): State<Arc<AppState>>,
    Json(req): Json<ReactReq>,
) -> Result<StatusCode, (StatusCode, String)> {
    let me = auth_user(&s, &req.session_token).await?;
    let chat_id = message_chat(&s, req.message_id).await?;
    if !can_access_chat(&s, me, chat_id).await? {
        return Err((StatusCode::FORBIDDEN, "not a member".into()));
    }
    let emoji = req.emoji.trim();
    if emoji.is_empty() || emoji.chars().count() > 16 {
        return Err((StatusCode::BAD_REQUEST, "bad emoji".into()));
    }
    if req.remove {
        sqlx::query!(
            "DELETE FROM message_reactions WHERE message_id=$1 AND user_id=$2 AND emoji=$3",
            req.message_id, me, emoji).execute(&s.db).await.map_err(internal)?;
    } else {
        sqlx::query!(
            r#"INSERT INTO message_reactions (message_id, user_id, emoji)
               VALUES ($1,$2,$3) ON CONFLICT DO NOTHING"#,
            req.message_id, me, emoji).execute(&s.db).await.map_err(internal)?;
    }
    let _ = create_event(
        &s,
        Some(chat_id),
        "reaction",
        Some(req.message_id),
        Some(me),
        serde_json::json!({ "emoji": emoji, "remove": req.remove }),
    ).await;
    Ok(StatusCode::NO_CONTENT)
}

// ---------------- 软删除消息 ----------------
#[derive(Deserialize)]
pub struct DeleteReq { pub session_token: String, pub message_id: i64 }

pub async fn delete_msg(
    State(s): State<Arc<AppState>>,
    Json(req): Json<DeleteReq>,
) -> Result<StatusCode, (StatusCode, String)> {
    let me = auth_user(&s, &req.session_token).await?;
    let row = sqlx::query!(
        "SELECT sender_id, chat_id FROM messages WHERE id = $1", req.message_id)
        .fetch_optional(&s.db).await.map_err(internal)?
        .ok_or((StatusCode::NOT_FOUND, "msg not found".into()))?;
    if !can_access_chat(&s, me, row.chat_id).await? {
        return Err((StatusCode::FORBIDDEN, "not a member".into()));
    }
    if row.sender_id != Some(me) {
        return Err((StatusCode::FORBIDDEN, "not your message".into()));
    }
    sqlx::query!("UPDATE messages SET deleted_at = now() WHERE id = $1", req.message_id)
        .execute(&s.db).await.map_err(internal)?;
    let event_id = create_event(
        &s,
        Some(row.chat_id),
        "message_deleted",
        Some(req.message_id),
        Some(me),
        serde_json::json!({ "message_id": req.message_id }),
    ).await?;
    let payload = serde_json::json!({
        "type": "event",
        "event_id": event_id,
        "event_type": "message_deleted",
        "message_id": req.message_id,
        "chat_id": row.chat_id.to_string(),
        "legacy_type": "message_deleted"
    });
    let members = sqlx::query_scalar!(
        "SELECT user_id FROM chat_members WHERE chat_id = $1", row.chat_id)
        .fetch_all(&s.db).await.map_err(internal)?;
    if members.is_empty() {
        ws::broadcast_all(&s, &payload);
    } else {
        for uid in members {
            ws::push_to(&s, uid, &payload);
        }
    }
    Ok(StatusCode::NO_CONTENT)
}

// ---------------- 全文搜索 ----------------
// Ctrl+F 触发 — 模糊匹配 messages.payload (text/sticker_alias) 在所有当前用户可见的频道。
// 公开频道 + 用户加入的非公开频道。
#[derive(Deserialize)]
pub struct SearchQ {
    pub session_token: String,
    pub q:             String,
    pub limit:         Option<i64>,
}

#[derive(Serialize)]
pub struct SearchHit {
    pub id:         i64,
    pub chat_id:    String,
    pub chat_slug:  Option<String>,
    pub sender_id:  Option<String>,
    pub msg_type:   String,
    pub payload:    String,
    pub created_at: i64,
}

pub async fn search(
    State(s): State<Arc<AppState>>,
    Query(q): Query<SearchQ>,
) -> Result<Json<Vec<SearchHit>>, (StatusCode, String)> {
    let me = auth_user(&s, &q.session_token).await?;
    let term = q.q.trim();
    if term.chars().count() < 2 {
        return Ok(Json(vec![]));
    }
    let limit = q.limit.unwrap_or(50).clamp(1, 200);
    let pattern = format!("%{}%", term.replace('\\', "\\\\").replace('%', "\\%").replace('_', "\\_"));
    let rows = sqlx::query!(
        r#"SELECT m.id, m.chat_id, m.sender_id, m.msg_type,
                  m.payload::text as "payload!",
                  m.created_at, c.slug
           FROM messages m JOIN chats c ON c.id = m.chat_id
           WHERE m.deleted_at IS NULL
             AND m.msg_type IN ('text', 'sticker', 'system')
             AND (
                  (c.kind = 'channel' AND c.is_official = TRUE)
                  OR m.chat_id IN (SELECT chat_id FROM chat_members WHERE user_id = $1)
             )
             AND m.payload::text ILIKE $2 ESCAPE '\'
           ORDER BY m.id DESC
           LIMIT $3"#,
        me, pattern, limit)
        .fetch_all(&s.db).await.map_err(internal)?;
    Ok(Json(rows.into_iter().map(|r| {
        // payload::text 会带引号 — 简单 trim
        let mut p = r.payload;
        if p.len() >= 2 && p.starts_with('"') && p.ends_with('"') {
            p = p[1..p.len()-1].to_string();
        }
        SearchHit {
            id: r.id,
            chat_id: r.chat_id.to_string(),
            chat_slug: r.slug,
            sender_id: r.sender_id.map(|u| u.to_string()),
            msg_type: r.msg_type,
            payload: p,
            created_at: r.created_at.timestamp(),
        }
    }).collect()))
}
