// 聊天：DM / Group / Channel + messages CRUD + 实时通过 ws.rs 推送

use crate::media::auth_user;
use crate::state::AppState;
use crate::ws;
use axum::{
    extract::{Json, Query, State},
    http::StatusCode,
};
use chrono::{DateTime, Duration, Utc};
use serde::{Deserialize, Serialize};
use sqlx::{postgres::PgRow, Row};
use std::sync::Arc;
use uuid::Uuid;

fn internal<E: std::fmt::Display>(e: E) -> (StatusCode, String) {
    (StatusCode::INTERNAL_SERVER_ERROR, e.to_string())
}

async fn can_access_chat(
    state: &AppState,
    user_id: Uuid,
    chat_id: Uuid,
) -> Result<bool, (StatusCode, String)> {
    let is_admin = is_admin_user(state, user_id).await?;
    let is_super_admin = if is_admin {
        is_super_admin_user(state, user_id).await?
    } else {
        false
    };
    if let Some(ticket) = ticket_meta_for_chat(state, chat_id).await? {
        return Ok(ticket.creator_id == Some(user_id)
            || (ticket.visibility == "public" && !ticket.protected)
            || (is_admin && (!ticket.protected || is_super_admin)));
    }
    if let Some((status, _locked)) = forum_status_for_chat(state, chat_id).await? {
        return Ok(status != "hidden" || is_admin);
    }
    let chat_meta = sqlx::query!("SELECT kind, is_official FROM chats WHERE id = $1", chat_id)
        .fetch_optional(&state.db)
        .await
        .map_err(internal)?
        .ok_or((StatusCode::NOT_FOUND, "chat not found".into()))?;
    if chat_meta.kind == "channel" && chat_meta.is_official {
        return Ok(true);
    }
    let member = sqlx::query_scalar!(
        "SELECT 1 as ok FROM chat_members WHERE chat_id=$1 AND user_id=$2",
        chat_id,
        user_id
    )
    .fetch_optional(&state.db)
    .await
    .map_err(internal)?;
    Ok(member.is_some())
}

async fn is_admin_user(state: &AppState, user_id: Uuid) -> Result<bool, (StatusCode, String)> {
    Ok(sqlx::query_scalar::<_, bool>(
        "SELECT COALESCE(is_admin, false) OR role IN ('admin','owner','super_admin') FROM users WHERE id = $1"
    )
        .bind(user_id)
        .fetch_optional(&state.db).await.map_err(internal)?
        .unwrap_or(false))
}

async fn is_super_admin_user(
    state: &AppState,
    user_id: Uuid,
) -> Result<bool, (StatusCode, String)> {
    let row = sqlx::query(
        r#"SELECT username, is_admin, role, role_label
           FROM users WHERE id = $1"#,
    )
    .bind(user_id)
    .fetch_optional(&state.db)
    .await
    .map_err(internal)?
    .ok_or((StatusCode::NOT_FOUND, "user not found".into()))?;
    let username: Option<String> = row.try_get("username").map_err(internal)?;
    let is_admin: bool = row.try_get("is_admin").map_err(internal)?;
    let role: String = row.try_get("role").map_err(internal)?;
    Ok(is_admin
        && (role == "owner" || role == "super_admin" || username.as_deref() == Some("admin")))
}

async fn role_for_user(state: &AppState, user_id: Uuid) -> Result<String, (StatusCode, String)> {
    Ok(
        sqlx::query_scalar::<_, String>("SELECT role FROM users WHERE id = $1")
            .bind(user_id)
            .fetch_optional(&state.db)
            .await
            .map_err(internal)?
            .unwrap_or_else(|| "user".into()),
    )
}

async fn level_for_user(state: &AppState, user_id: Uuid) -> Result<i32, (StatusCode, String)> {
    Ok(
        sqlx::query_scalar::<_, i32>("SELECT level FROM user_progress WHERE user_id = $1")
            .bind(user_id)
            .fetch_optional(&state.db)
            .await
            .map_err(internal)?
            .unwrap_or(1),
    )
}

async fn has_active_subscription(
    state: &AppState,
    user_id: Uuid,
) -> Result<bool, (StatusCode, String)> {
    Ok(sqlx::query_scalar::<_, bool>(
        r#"SELECT EXISTS (
                SELECT 1 FROM subscriptions
                WHERE user_id = $1 AND (expires_at IS NULL OR expires_at > now())
           ) OR EXISTS (
                SELECT 1 FROM users
                WHERE id = $1
                  AND subscription_tier IS NOT NULL
                  AND subscription_tier <> ''
                  AND (subscription_expires_at IS NULL OR subscription_expires_at > now())
           )"#,
    )
    .bind(user_id)
    .fetch_one(&state.db)
    .await
    .map_err(internal)?)
}

fn level_from_xp(xp: i64) -> i32 {
    ((xp / 100 + 1) as i32).clamp(1, 100)
}

async fn grant_xp_event(
    state: &AppState,
    user_id: Uuid,
    event_type: &str,
    amount: i32,
    source_type: &str,
    source_id: String,
) -> Result<(), (StatusCode, String)> {
    sqlx::query(
        r#"INSERT INTO user_xp_events (user_id, event_type, amount, source_type, source_id)
           VALUES ($1, $2, $3, $4, $5)"#,
    )
    .bind(user_id)
    .bind(event_type)
    .bind(amount)
    .bind(source_type)
    .bind(source_id)
    .execute(&state.db)
    .await
    .map_err(internal)?;
    let row = sqlx::query(
        "SELECT COALESCE(SUM(amount), 0)::BIGINT AS xp FROM user_xp_events WHERE user_id = $1",
    )
    .bind(user_id)
    .fetch_one(&state.db)
    .await
    .map_err(internal)?;
    let xp: i64 = row.try_get("xp").map_err(internal)?;
    let level = level_from_xp(xp);
    sqlx::query(
        r#"INSERT INTO user_progress (user_id, level, xp, updated_at)
           VALUES ($1, $2, $3, now())
           ON CONFLICT (user_id) DO UPDATE
             SET level = EXCLUDED.level, xp = EXCLUDED.xp, updated_at = now()"#,
    )
    .bind(user_id)
    .bind(level)
    .bind(xp)
    .execute(&state.db)
    .await
    .map_err(internal)?;
    Ok(())
}

#[derive(Clone)]
struct TicketChatMeta {
    creator_id: Option<Uuid>,
    visibility: String,
    protected: bool,
}

async fn ticket_meta_for_chat(
    state: &AppState,
    chat_id: Uuid,
) -> Result<Option<TicketChatMeta>, (StatusCode, String)> {
    let row = sqlx::query(
        r#"SELECT creator_id, visibility, protected_by_superadmin
           FROM tickets WHERE chat_id = $1"#,
    )
    .bind(chat_id)
    .fetch_optional(&state.db)
    .await
    .map_err(internal)?;
    row.map(|r| {
        Ok(TicketChatMeta {
            creator_id: r.try_get("creator_id").map_err(internal)?,
            visibility: r.try_get("visibility").map_err(internal)?,
            protected: r.try_get("protected_by_superadmin").map_err(internal)?,
        })
    })
    .transpose()
}

async fn forum_status_for_chat(
    state: &AppState,
    chat_id: Uuid,
) -> Result<Option<(String, bool)>, (StatusCode, String)> {
    let row = sqlx::query("SELECT status, locked FROM forum_topics WHERE chat_id = $1")
        .bind(chat_id)
        .fetch_optional(&state.db)
        .await
        .map_err(internal)?;
    row.map(|r| {
        Ok((
            r.try_get("status").map_err(internal)?,
            r.try_get("locked").map_err(internal)?,
        ))
    })
    .transpose()
}

struct ChannelPolicy {
    write_policy: String,
    allowed_role: Option<String>,
    min_level: i32,
    slowmode_seconds: i32,
    requires_subscription: bool,
    is_readonly: bool,
    is_locked: bool,
}

async fn channel_policy_for_chat(
    state: &AppState,
    chat_id: Uuid,
) -> Result<Option<ChannelPolicy>, (StatusCode, String)> {
    let row = sqlx::query(
        r#"SELECT write_policy, allowed_role, min_level, slowmode_seconds,
                  requires_subscription, is_readonly, is_locked
           FROM channel_settings WHERE chat_id = $1"#,
    )
    .bind(chat_id)
    .fetch_optional(&state.db)
    .await
    .map_err(internal)?;
    row.map(|r| {
        Ok(ChannelPolicy {
            write_policy: r.try_get("write_policy").map_err(internal)?,
            allowed_role: r.try_get("allowed_role").map_err(internal)?,
            min_level: r.try_get("min_level").map_err(internal)?,
            slowmode_seconds: r.try_get("slowmode_seconds").map_err(internal)?,
            requires_subscription: r.try_get("requires_subscription").map_err(internal)?,
            is_readonly: r.try_get("is_readonly").map_err(internal)?,
            is_locked: r.try_get("is_locked").map_err(internal)?,
        })
    })
    .transpose()
}

#[derive(Clone, Debug, Serialize)]
pub struct UserBrief {
    pub user_id: String,
    pub uid: String,
    pub username: String,
    pub nickname: Option<String>,
    pub role: Option<String>,
    pub role_label: Option<String>,
}

async fn user_brief(
    state: &AppState,
    user_id: Uuid,
) -> Result<Option<UserBrief>, (StatusCode, String)> {
    let row = sqlx::query(
        r#"SELECT uid, username, nickname, role, role_label
           FROM users WHERE id = $1"#,
    )
    .bind(user_id)
    .fetch_optional(&state.db)
    .await
    .map_err(internal)?;
    row.map(|r| {
        Ok(UserBrief {
            user_id: user_id.to_string(),
            uid: r
                .try_get::<Option<String>, _>("uid")
                .map_err(internal)?
                .unwrap_or_default(),
            username: r
                .try_get::<Option<String>, _>("username")
                .map_err(internal)?
                .unwrap_or_default(),
            nickname: r.try_get("nickname").map_err(internal)?,
            role: Some(r.try_get::<String, _>("role").map_err(internal)?),
            role_label: r.try_get("role_label").map_err(internal)?,
        })
    })
    .transpose()
}

#[derive(Clone, Debug)]
struct ActiveMute {
    id: i64,
    target_user_id: Uuid,
    muted_by: Option<Uuid>,
    reason: String,
    muted_until: DateTime<Utc>,
    created_at: DateTime<Utc>,
}

async fn active_mute_for_user(
    state: &AppState,
    user_id: Uuid,
) -> Result<Option<ActiveMute>, (StatusCode, String)> {
    let row = sqlx::query(
        r#"SELECT id, target_user_id, muted_by, reason, muted_until, created_at
           FROM user_mutes
           WHERE target_user_id = $1
             AND revoked_at IS NULL
             AND muted_until > now()
           ORDER BY muted_until DESC, id DESC
           LIMIT 1"#,
    )
    .bind(user_id)
    .fetch_optional(&state.db)
    .await
    .map_err(internal)?;
    row.map(|r| {
        Ok(ActiveMute {
            id: r.try_get("id").map_err(internal)?,
            target_user_id: r.try_get("target_user_id").map_err(internal)?,
            muted_by: r.try_get("muted_by").map_err(internal)?,
            reason: r.try_get("reason").map_err(internal)?,
            muted_until: r.try_get("muted_until").map_err(internal)?,
            created_at: r.try_get("created_at").map_err(internal)?,
        })
    })
    .transpose()
}

async fn normalize_mentions(
    state: &AppState,
    chat_id: Uuid,
    mut mentions: Vec<Uuid>,
) -> Result<Vec<Uuid>, (StatusCode, String)> {
    mentions.sort();
    mentions.dedup();
    if mentions.len() > 20 {
        return Err((StatusCode::BAD_REQUEST, "too many mentions".into()));
    }
    for uid in &mentions {
        let exists = sqlx::query_scalar!("SELECT 1 as ok FROM users WHERE id = $1", uid)
            .fetch_optional(&state.db)
            .await
            .map_err(internal)?
            .is_some();
        if !exists {
            return Err((StatusCode::BAD_REQUEST, "mentioned user not found".into()));
        }
        if !can_access_chat(state, *uid, chat_id).await? {
            return Err((
                StatusCode::BAD_REQUEST,
                "mentioned user cannot access chat".into(),
            ));
        }
    }
    Ok(mentions)
}

async fn ensure_can_write_chat(
    state: &AppState,
    user_id: Uuid,
    chat_id: Uuid,
) -> Result<(), (StatusCode, String)> {
    if let Some(active) = active_mute_for_user(state, user_id).await? {
        return Err((
            StatusCode::FORBIDDEN,
            format!(
                "muted until {}: {}",
                active.muted_until.timestamp(),
                active.reason
            ),
        ));
    }

    let chat_meta = sqlx::query!(
        r#"SELECT kind, write_role, is_official FROM chats WHERE id = $1"#,
        chat_id
    )
    .fetch_optional(&state.db)
    .await
    .map_err(internal)?
    .ok_or((StatusCode::NOT_FOUND, "chat not found".into()))?;

    if !can_access_chat(state, user_id, chat_id).await? {
        return Err((StatusCode::FORBIDDEN, "not a member".into()));
    }

    let is_admin = is_admin_user(state, user_id).await?;
    if let Some(ticket) = ticket_meta_for_chat(state, chat_id).await? {
        if ticket.protected && is_admin && !is_super_admin_user(state, user_id).await? {
            return Err((StatusCode::FORBIDDEN, "ticket is protected".into()));
        }
        if ticket.creator_id != Some(user_id) && !is_admin {
            return Err((
                StatusCode::FORBIDDEN,
                "ticket replies are restricted".into(),
            ));
        }
    }
    if let Some((status, locked)) = forum_status_for_chat(state, chat_id).await? {
        if status == "locked" || locked {
            return Err((StatusCode::FORBIDDEN, "topic is locked".into()));
        }
    }
    let channel_policy = channel_policy_for_chat(state, chat_id).await?;
    if let Some(policy) = channel_policy.as_ref() {
        if policy.is_locked {
            return Err((StatusCode::FORBIDDEN, "channel is locked".into()));
        }
        if policy.is_readonly
            || policy.write_policy == "readonly"
            || policy.write_policy == "locked"
        {
            return Err((StatusCode::FORBIDDEN, "channel is read only".into()));
        }
        if policy.requires_subscription || policy.write_policy == "subscriber_only" {
            if !has_active_subscription(state, user_id).await? {
                return Err((StatusCode::FORBIDDEN, "subscription required".into()));
            }
        }
        if policy.write_policy == "admin_only" && !is_admin {
            return Err((StatusCode::FORBIDDEN, "channel is admin only".into()));
        }
        if policy.write_policy == "role_only" {
            let role = role_for_user(state, user_id).await?;
            if policy.allowed_role.as_deref() != Some(role.as_str()) && !is_admin {
                return Err((StatusCode::FORBIDDEN, "role is not allowed".into()));
            }
        }
        if policy.write_policy == "min_level"
            && level_for_user(state, user_id).await? < policy.min_level
        {
            return Err((StatusCode::FORBIDDEN, "level is too low".into()));
        }
        if policy.slowmode_seconds > 0 && !is_admin {
            let recent = sqlx::query_scalar::<_, i64>(
                r#"SELECT COUNT(*) FROM messages
                   WHERE chat_id = $1 AND sender_id = $2
                     AND created_at > now() - ($3::TEXT || ' seconds')::INTERVAL"#,
            )
            .bind(chat_id)
            .bind(user_id)
            .bind(policy.slowmode_seconds)
            .fetch_one(&state.db)
            .await
            .map_err(internal)?;
            if recent > 0 {
                return Err((StatusCode::TOO_MANY_REQUESTS, "slowmode active".into()));
            }
        }
    }

    if channel_policy.is_none()
        && chat_meta.kind == "channel"
        && chat_meta.write_role == "admin_only"
        && !is_admin
    {
        return Err((StatusCode::FORBIDDEN, "channel is admin only".into()));
    }
    Ok(())
}

async fn message_chat(state: &AppState, message_id: i64) -> Result<Uuid, (StatusCode, String)> {
    sqlx::query_scalar!(
        "SELECT chat_id FROM messages WHERE id = $1 AND deleted_at IS NULL",
        message_id
    )
    .fetch_optional(&state.db)
    .await
    .map_err(internal)?
    .ok_or((StatusCode::NOT_FOUND, "msg not found".into()))
}

async fn broadcast_chat_event(
    state: &AppState,
    chat_id: Uuid,
    payload: &serde_json::Value,
) -> Result<(), (StatusCode, String)> {
    let chat_meta = sqlx::query("SELECT kind, is_official FROM chats WHERE id = $1")
        .bind(chat_id)
        .fetch_optional(&state.db)
        .await
        .map_err(internal)?
        .ok_or((StatusCode::NOT_FOUND, "chat not found".into()))?;
    let kind: String = chat_meta.try_get("kind").map_err(internal)?;
    let is_official: bool = chat_meta.try_get("is_official").map_err(internal)?;
    if kind == "channel" && is_official {
        ws::broadcast_all(state, payload);
    } else if ticket_meta_for_chat(state, chat_id).await?.is_some()
        || forum_status_for_chat(state, chat_id).await?.is_some()
    {
        let users = sqlx::query_scalar::<_, Uuid>("SELECT id FROM users")
            .fetch_all(&state.db)
            .await
            .map_err(internal)?;
        for uid in users {
            if can_access_chat(state, uid, chat_id).await.unwrap_or(false) {
                ws::push_to(state, uid, payload);
            }
        }
    } else {
        let members =
            sqlx::query_scalar::<_, Uuid>("SELECT user_id FROM chat_members WHERE chat_id = $1")
                .bind(chat_id)
                .fetch_all(&state.db)
                .await
                .map_err(internal)?;
        for uid in members {
            ws::push_to(state, uid, payload);
        }
    }
    Ok(())
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
           RETURNING id"#,
    )
    .bind(chat_id)
    .bind(event_type)
    .bind(message_id)
    .bind(actor_id)
    .bind(payload)
    .fetch_one(&state.db)
    .await
    .map_err(internal)?;
    Ok(row.try_get("id").map_err(internal)?)
}

async fn message_event_id(
    state: &AppState,
    message_id: i64,
) -> Result<Option<i64>, (StatusCode, String)> {
    let row = sqlx::query(
        r#"SELECT id FROM chat_events
           WHERE message_id = $1 AND event_type = 'message'
           ORDER BY id ASC LIMIT 1"#,
    )
    .bind(message_id)
    .fetch_optional(&state.db)
    .await
    .map_err(internal)?;
    row.map(|r| r.try_get("id").map_err(internal)).transpose()
}

#[derive(Serialize, Clone)]
pub struct ReplySnapshot {
    pub id: i64,
    pub sender_id: Option<String>,
    pub sender_uid: Option<String>,
    pub sender_username: Option<String>,
    pub sender_nickname: Option<String>,
    pub msg_type: String,
    pub preview: String,
    pub deleted: bool,
}

#[derive(Serialize, Clone)]
pub struct MentionOut {
    pub user_id: String,
    pub uid: String,
    pub username: String,
    pub nickname: Option<String>,
}

fn message_preview(payload: &serde_json::Value, msg_type: &str) -> String {
    if let Some(text) = payload.get("text").and_then(|v| v.as_str()) {
        return text.chars().take(140).collect();
    }
    if let Some(s) = payload.as_str() {
        return s.chars().take(140).collect();
    }
    match msg_type {
        "image" => "[image]".into(),
        "video" => "[video]".into(),
        "gif" => "[gif]".into(),
        "sticker" => "[sticker]".into(),
        "pack_share" => "[sticker pack]".into(),
        "system" => "[system]".into(),
        _ => payload.to_string().chars().take(140).collect(),
    }
}

async fn reply_snapshot_for(
    state: &AppState,
    reply_to_id: Option<i64>,
) -> Result<Option<ReplySnapshot>, (StatusCode, String)> {
    let Some(reply_id) = reply_to_id else {
        return Ok(None);
    };
    let row = sqlx::query(
        r#"SELECT m.id, m.sender_id, m.msg_type, m.payload, m.deleted_at,
                  u.uid as sender_uid, u.username as sender_username, u.nickname as sender_nickname
           FROM messages m
           LEFT JOIN users u ON u.id = m.sender_id
           WHERE m.id = $1"#,
    )
    .bind(reply_id)
    .fetch_optional(&state.db)
    .await
    .map_err(internal)?;
    row.map(|r| {
        let payload: serde_json::Value = r.try_get("payload").map_err(internal)?;
        let msg_type: String = r.try_get("msg_type").map_err(internal)?;
        let deleted = r
            .try_get::<Option<DateTime<Utc>>, _>("deleted_at")
            .map_err(internal)?
            .is_some();
        let preview = if deleted {
            "[deleted]".into()
        } else {
            message_preview(&payload, &msg_type)
        };
        Ok(ReplySnapshot {
            id: r.try_get("id").map_err(internal)?,
            sender_id: r
                .try_get::<Option<Uuid>, _>("sender_id")
                .map_err(internal)?
                .map(|u| u.to_string()),
            sender_uid: r
                .try_get::<Option<String>, _>("sender_uid")
                .map_err(internal)?,
            sender_username: r
                .try_get::<Option<String>, _>("sender_username")
                .map_err(internal)?,
            sender_nickname: r.try_get("sender_nickname").map_err(internal)?,
            msg_type,
            preview,
            deleted,
        })
    })
    .transpose()
}

async fn mentions_for_message(
    state: &AppState,
    message_id: i64,
) -> Result<Vec<MentionOut>, (StatusCode, String)> {
    let rows = sqlx::query(
        r#"SELECT u.id, u.uid, u.username, u.nickname
           FROM message_mentions mm
           JOIN users u ON u.id = mm.user_id
           WHERE mm.message_id = $1
           ORDER BY u.nickname NULLS LAST, u.username"#,
    )
    .bind(message_id)
    .fetch_all(&state.db)
    .await
    .map_err(internal)?;
    let mut out = Vec::with_capacity(rows.len());
    for r in rows {
        let id: Uuid = r.try_get("id").map_err(internal)?;
        out.push(MentionOut {
            user_id: id.to_string(),
            uid: r
                .try_get::<Option<String>, _>("uid")
                .map_err(internal)?
                .unwrap_or_default(),
            username: r
                .try_get::<Option<String>, _>("username")
                .map_err(internal)?
                .unwrap_or_default(),
            nickname: r.try_get("nickname").map_err(internal)?,
        });
    }
    Ok(out)
}

async fn message_out_from_row(
    state: &AppState,
    row: &PgRow,
    chat_id: Uuid,
    sender_id: Option<Uuid>,
    event_id: Option<i64>,
) -> Result<MessageOut, (StatusCode, String)> {
    let id: i64 = row.try_get("id").map_err(internal)?;
    let reply_to_id: Option<i64> = row.try_get("reply_to_id").map_err(internal)?;
    let sender = match sender_id {
        Some(uid) => user_brief(state, uid).await?,
        None => None,
    };
    Ok(MessageOut {
        id,
        chat_id: chat_id.to_string(),
        sender_id: sender_id.map(|u| u.to_string()),
        sender_uid: sender.as_ref().map(|u| u.uid.clone()),
        sender_username: sender.as_ref().map(|u| u.username.clone()),
        sender_nickname: sender.as_ref().and_then(|u| u.nickname.clone()),
        sender_role: sender.as_ref().and_then(|u| u.role.clone()),
        sender_role_label: sender.as_ref().and_then(|u| u.role_label.clone()),
        msg_type: row.try_get("msg_type").map_err(internal)?,
        payload: row.try_get("payload").map_err(internal)?,
        reply_to_id,
        reply_snapshot: reply_snapshot_for(state, reply_to_id).await?,
        created_at: row
            .try_get::<DateTime<Utc>, _>("created_at")
            .map_err(internal)?
            .timestamp(),
        edited_at: row
            .try_get::<Option<DateTime<Utc>>, _>("edited_at")
            .map_err(internal)?
            .map(|t| t.timestamp()),
        deleted: row
            .try_get::<Option<DateTime<Utc>>, _>("deleted_at")
            .map_err(internal)?
            .is_some(),
        client_msg_id: row
            .try_get::<Option<Uuid>, _>("client_msg_id")
            .map_err(internal)?
            .map(|v| v.to_string()),
        sender_device_id: row.try_get("sender_device_id").map_err(internal)?,
        mentions: mentions_for_message(state, id).await?,
        event_id,
    })
}

// ---------------- 创建 / 打开 DM ----------------
#[derive(Deserialize)]
pub struct OpenDmReq {
    pub session_token: String,
    pub other_user_id: Uuid,
}

#[derive(Serialize)]
pub struct ChatBrief {
    pub id: String,
    pub kind: String,
    pub title: Option<String>,
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
    let (lo, hi) = if me < req.other_user_id {
        (me, req.other_user_id)
    } else {
        (req.other_user_id, me)
    };

    // 已存在？
    if let Some(row) = sqlx::query!(
        r#"SELECT c.id, c.title, c.last_message_at
           FROM chat_dm_index d JOIN chats c ON c.id = d.chat_id
           WHERE d.user_lo = $1 AND d.user_hi = $2"#,
        lo,
        hi
    )
    .fetch_optional(&s.db)
    .await
    .map_err(internal)?
    {
        return Ok(Json(ChatBrief {
            id: row.id.to_string(),
            kind: "dm".into(),
            title: row.title,
            last_message_at: row.last_message_at.map(|t| t.timestamp()),
        }));
    }

    let chat = sqlx::query!(
        "INSERT INTO chats (kind, created_by) VALUES ('dm', $1) RETURNING id",
        me
    )
    .fetch_one(&s.db)
    .await
    .map_err(internal)?;
    sqlx::query!(
        "INSERT INTO chat_dm_index (chat_id, user_lo, user_hi) VALUES ($1, $2, $3)",
        chat.id,
        lo,
        hi
    )
    .execute(&s.db)
    .await
    .map_err(internal)?;
    sqlx::query!(
        r#"INSERT INTO chat_members (chat_id, user_id, role) VALUES ($1, $2, 'member'), ($1, $3, 'member')"#,
        chat.id, me, req.other_user_id).execute(&s.db).await.map_err(internal)?;

    Ok(Json(ChatBrief {
        id: chat.id.to_string(),
        kind: "dm".into(),
        title: None,
        last_message_at: None,
    }))
}

// ---------------- 创建 group/channel ----------------
#[derive(Deserialize)]
pub struct CreateGroupReq {
    pub session_token: String,
    pub title: String,
    pub kind: String, // 'group' | 'channel'
    pub member_ids: Vec<Uuid>,
}

pub async fn create_group(
    State(s): State<Arc<AppState>>,
    Json(req): Json<CreateGroupReq>,
) -> Result<Json<ChatBrief>, (StatusCode, String)> {
    let me = auth_user(&s, &req.session_token).await?;
    if !["group", "channel"].contains(&req.kind.as_str()) {
        return Err((
            StatusCode::BAD_REQUEST,
            "kind must be group or channel".into(),
        ));
    }
    if req.title.trim().is_empty() {
        return Err((StatusCode::BAD_REQUEST, "title required".into()));
    }
    let write_role = if req.kind == "channel" {
        "user"
    } else {
        "admin_only"
    };
    let chat = sqlx::query!(
        "INSERT INTO chats (kind, title, created_by, write_role) VALUES ($1, $2, $3, $4) RETURNING id",
        req.kind, req.title, me, write_role).fetch_one(&s.db).await.map_err(internal)?;

    // owner = me
    sqlx::query!(
        "INSERT INTO chat_members (chat_id, user_id, role) VALUES ($1, $2, 'owner')",
        chat.id,
        me
    )
    .execute(&s.db)
    .await
    .map_err(internal)?;
    for mid in req.member_ids {
        if mid == me {
            continue;
        }
        let _ = sqlx::query!(
            "INSERT INTO chat_members (chat_id, user_id, role) VALUES ($1, $2, 'member') ON CONFLICT DO NOTHING",
            chat.id, mid).execute(&s.db).await;
    }

    Ok(Json(ChatBrief {
        id: chat.id.to_string(),
        kind: req.kind,
        title: Some(req.title),
        last_message_at: None,
    }))
}

// ---------------- 我的会话列表 ----------------
#[derive(Deserialize)]
pub struct ListChatsQ {
    pub session_token: String,
}

#[derive(Serialize)]
pub struct ChatListItem {
    pub id: String,
    pub kind: String,
    pub title: Option<String>,
    pub avatar_url: Option<String>,
    pub last_message: Option<MessageOut>,
    pub last_message_at: Option<i64>,
    pub unread_count: i64,
}

// ---------------- 官方频道列表 (任何登录用户可见) ----------------
#[derive(Deserialize)]
pub struct OfficialQ {
    pub session_token: String,
}

#[derive(Serialize)]
pub struct OfficialChan {
    pub id: String,
    pub slug: String,
    pub title: String,
    pub group_label: Option<String>,
    pub write_role: String,
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
           ORDER BY group_label NULLS LAST, title"#
    )
    .fetch_all(&s.db)
    .await
    .map_err(internal)?;
    Ok(Json(
        rows.into_iter()
            .map(|r| OfficialChan {
                id: r.id.to_string(),
                slug: r.slug.unwrap_or_default(),
                title: r.title.unwrap_or_default(),
                group_label: r.group_label,
                write_role: r.write_role,
            })
            .collect(),
    ))
}

pub async fn list_chats(
    State(s): State<Arc<AppState>>,
    Query(q): Query<ListChatsQ>,
) -> Result<Json<Vec<ChatListItem>>, (StatusCode, String)> {
    let me = auth_user(&s, &q.session_token).await?;
    let rows = sqlx::query(
        r#"SELECT c.id, c.kind, c.title,
                  (SELECT m.created_at
                   FROM messages m
                   WHERE m.chat_id = c.id AND m.deleted_at IS NULL
                   ORDER BY m.id DESC
                   LIMIT 1) AS last_visible_message_at,
                  cm.last_read_message_id
           FROM chats c
           LEFT JOIN chat_members cm
             ON cm.chat_id = c.id AND cm.user_id = $1
           WHERE (c.kind = 'channel' AND c.is_official = TRUE)
              OR (
                  cm.user_id = $1
                  AND NOT EXISTS (SELECT 1 FROM tickets t_guard WHERE t_guard.chat_id = c.id)
                  AND NOT EXISTS (SELECT 1 FROM forum_topics ft_guard WHERE ft_guard.chat_id = c.id)
              )
              OR EXISTS (
                  SELECT 1 FROM forum_topics ft
                  WHERE ft.chat_id = c.id AND ft.status <> 'hidden'
              )
              OR EXISTS (
                  SELECT 1 FROM tickets t
                  WHERE t.chat_id = c.id
                    AND (
                      t.creator_id = $1
                      OR (t.visibility = 'public' AND t.protected_by_superadmin = FALSE)
                      OR (
                          (SELECT COALESCE(is_admin, false) OR role IN ('admin','owner','super_admin') FROM users WHERE id = $1)
                          AND (
                              t.protected_by_superadmin = FALSE
                              OR (SELECT COALESCE(is_admin, false) AND role IN ('owner','super_admin') FROM users WHERE id = $1)
                          )
                      )
                    )
              )
           ORDER BY last_visible_message_at DESC NULLS LAST, c.title ASC
           LIMIT 100"#)
        .bind(me)
        .fetch_all(&s.db).await.map_err(internal)?;

    let mut out = Vec::with_capacity(rows.len());
    for r in rows {
        // 最后一条消息
        let last = sqlx::query(
            r#"SELECT id, sender_id, msg_type, payload, reply_to_id, created_at,
                      edited_at, deleted_at, client_msg_id, sender_device_id
               FROM messages WHERE chat_id = $1 AND deleted_at IS NULL
               ORDER BY id DESC LIMIT 1"#,
        )
        .bind(r.try_get::<Uuid, _>("id").map_err(internal)?)
        .fetch_optional(&s.db)
        .await
        .map_err(internal)?;
        let chat_id: Uuid = r.try_get("id").map_err(internal)?;
        let last_out = match last {
            Some(m) => {
                let sender_id = m
                    .try_get::<Option<Uuid>, _>("sender_id")
                    .map_err(internal)?;
                Some(message_out_from_row(&s, &m, chat_id, sender_id, None).await?)
            }
            None => None,
        };
        // 未读数
        let last_read_message_id: Option<i64> =
            r.try_get("last_read_message_id").map_err(internal)?;
        let unread =
            if let Some(lr) = last_read_message_id {
                sqlx::query_scalar!(
                "SELECT COUNT(*) FROM messages WHERE chat_id=$1 AND id > $2 AND deleted_at IS NULL",
                chat_id, lr)
                .fetch_one(&s.db)
                .await
                .unwrap_or(Some(0))
                .unwrap_or(0)
            } else {
                sqlx::query_scalar!(
                    "SELECT COUNT(*) FROM messages WHERE chat_id=$1 AND deleted_at IS NULL",
                    chat_id
                )
                .fetch_one(&s.db)
                .await
                .unwrap_or(Some(0))
                .unwrap_or(0)
            };
        let kind: String = r.try_get("kind").map_err(internal)?;
        let title: Option<String> = r.try_get("title").map_err(internal)?;
        let last_visible_message_at: Option<DateTime<Utc>> =
            r.try_get("last_visible_message_at").map_err(internal)?;
        out.push(ChatListItem {
            id: chat_id.to_string(),
            kind,
            title,
            avatar_url: None,
            last_message: last_out,
            last_message_at: last_visible_message_at.map(|t| t.timestamp()),
            unread_count: unread,
        });
    }
    Ok(Json(out))
}

// ---------------- 发消息 ----------------
#[derive(Deserialize)]
pub struct SendReq {
    pub session_token: String,
    pub chat_id: Uuid,
    pub msg_type: String, // text/image/video/gif/sticker/pack_share
    pub payload: serde_json::Value,
    pub reply_to_id: Option<i64>,
    pub client_msg_id: Option<Uuid>,
    pub device_id: Option<String>,
    #[serde(default)]
    pub mentions: Vec<Uuid>,
}

#[derive(Serialize, Clone)]
pub struct MessageOut {
    pub id: i64,
    pub chat_id: String,
    pub sender_id: Option<String>,
    pub sender_uid: Option<String>,
    pub sender_username: Option<String>,
    pub sender_nickname: Option<String>,
    pub sender_role: Option<String>,
    pub sender_role_label: Option<String>,
    pub msg_type: String,
    pub payload: serde_json::Value,
    pub reply_to_id: Option<i64>,
    pub reply_snapshot: Option<ReplySnapshot>,
    pub created_at: i64,
    pub edited_at: Option<i64>,
    pub deleted: bool,
    pub client_msg_id: Option<String>,
    pub sender_device_id: Option<String>,
    pub mentions: Vec<MentionOut>,
    pub event_id: Option<i64>,
}

#[derive(Serialize)]
pub struct ChatEventOut {
    pub id: i64,
    pub chat_id: Option<String>,
    pub event_type: String,
    pub message_id: Option<i64>,
    pub actor_id: Option<String>,
    pub payload: serde_json::Value,
    pub created_at: i64,
}

#[derive(Deserialize)]
pub struct MuteReq {
    pub session_token: String,
    pub chat_id: Uuid,
    pub target_user_id: Uuid,
    pub duration_seconds: i64,
    pub reason: String,
}

#[derive(Deserialize)]
pub struct UnmuteReq {
    pub session_token: String,
    pub chat_id: Uuid,
    pub target_user_id: Uuid,
    pub reason: Option<String>,
}

#[derive(Deserialize)]
pub struct ModerationMemberQ {
    pub session_token: String,
    pub chat_id: Uuid,
    pub target_user_id: Uuid,
}

#[derive(Serialize)]
pub struct ModerationMemberResp {
    pub target: Option<UserBrief>,
    pub active: bool,
    pub mute_id: Option<i64>,
    pub muted_until: Option<i64>,
    pub reason: Option<String>,
    pub muted_by: Option<UserBrief>,
    pub can_mute: bool,
    pub can_unmute: bool,
    pub is_super_admin: bool,
}

fn trim_reason(reason: &str) -> String {
    let s = reason.trim();
    if s.is_empty() {
        "No reason provided".into()
    } else {
        s.chars().take(160).collect()
    }
}

async fn ensure_moderator(state: &AppState, actor: Uuid) -> Result<bool, (StatusCode, String)> {
    if !is_admin_user(state, actor).await? {
        return Err((StatusCode::FORBIDDEN, "admin required".into()));
    }
    is_super_admin_user(state, actor).await
}

fn duration_label(seconds: i64) -> String {
    if seconds % 86_400 == 0 {
        format!("{} 天", seconds / 86_400)
    } else if seconds % 3_600 == 0 {
        format!("{} 小时", seconds / 3_600)
    } else if seconds % 60 == 0 {
        format!("{} 分钟", seconds / 60)
    } else {
        format!("{} 秒", seconds)
    }
}

pub async fn moderation_member(
    State(s): State<Arc<AppState>>,
    Query(q): Query<ModerationMemberQ>,
) -> Result<Json<ModerationMemberResp>, (StatusCode, String)> {
    let me = auth_user(&s, &q.session_token).await?;
    if !can_access_chat(&s, me, q.chat_id).await? {
        return Err((StatusCode::FORBIDDEN, "not a member".into()));
    }
    let is_super = ensure_moderator(&s, me).await?;
    let target_is_admin = is_admin_user(&s, q.target_user_id).await?;
    let target = user_brief(&s, q.target_user_id).await?;
    let active = active_mute_for_user(&s, q.target_user_id).await?;
    let muted_by = match active.as_ref().and_then(|m| m.muted_by) {
        Some(uid) => user_brief(&s, uid).await?,
        None => None,
    };
    let can_unmute = active
        .as_ref()
        .map(|m| is_super || m.muted_by == Some(me))
        .unwrap_or(false);
    Ok(Json(ModerationMemberResp {
        target,
        active: active.is_some(),
        mute_id: active.as_ref().map(|m| m.id),
        muted_until: active.as_ref().map(|m| m.muted_until.timestamp()),
        reason: active.as_ref().map(|m| m.reason.clone()),
        muted_by,
        can_mute: me != q.target_user_id && (!target_is_admin || is_super),
        can_unmute,
        is_super_admin: is_super,
    }))
}

pub async fn mute_user(
    State(s): State<Arc<AppState>>,
    Json(req): Json<MuteReq>,
) -> Result<Json<ModerationMemberResp>, (StatusCode, String)> {
    let me = auth_user(&s, &req.session_token).await?;
    let is_super = ensure_moderator(&s, me).await?;
    if !can_access_chat(&s, me, req.chat_id).await? {
        return Err((StatusCode::FORBIDDEN, "not a member".into()));
    }
    if me == req.target_user_id {
        return Err((StatusCode::BAD_REQUEST, "cannot mute self".into()));
    }
    if is_admin_user(&s, req.target_user_id).await? && !is_super {
        return Err((
            StatusCode::FORBIDDEN,
            "only super admin can mute admins".into(),
        ));
    }
    let duration = req.duration_seconds.clamp(1, 60 * 60 * 24 * 365);
    let reason = trim_reason(&req.reason);
    let muted_until = Utc::now() + Duration::seconds(duration);
    sqlx::query!(
        r#"UPDATE user_mutes
           SET revoked_at = now(), revoked_by = $1, revoke_reason = 'superseded'
           WHERE target_user_id = $2 AND revoked_at IS NULL"#,
        me,
        req.target_user_id
    )
    .execute(&s.db)
    .await
    .map_err(internal)?;
    let row = sqlx::query!(
        r#"INSERT INTO user_mutes (target_user_id, muted_by, reason, muted_until)
           VALUES ($1, $2, $3, $4)
           RETURNING id"#,
        req.target_user_id,
        me,
        &reason,
        muted_until
    )
    .fetch_one(&s.db)
    .await
    .map_err(internal)?;
    let target = user_brief(&s, req.target_user_id).await?;
    let moderator = user_brief(&s, me).await?;
    let event_payload = serde_json::json!({
        "mute_id": row.id,
        "scope": "global",
        "target": target,
        "moderator": moderator,
        "reason": reason,
        "duration_seconds": duration,
        "duration_label": duration_label(duration),
        "muted_until": muted_until.timestamp()
    });
    let event_id = create_event(
        &s,
        Some(req.chat_id),
        "member_muted",
        None,
        Some(me),
        event_payload.clone(),
    )
    .await?;
    let payload = serde_json::json!({
        "type": "event",
        "event_id": event_id,
        "event_type": "member_muted",
        "chat_id": req.chat_id.to_string(),
        "actor_id": me.to_string(),
        "data": event_payload,
        "legacy_type": "member_muted"
    });
    broadcast_chat_event(&s, req.chat_id, &payload).await?;

    moderation_member(
        State(s),
        Query(ModerationMemberQ {
            session_token: req.session_token,
            chat_id: req.chat_id,
            target_user_id: req.target_user_id,
        }),
    )
    .await
}

pub async fn unmute_user(
    State(s): State<Arc<AppState>>,
    Json(req): Json<UnmuteReq>,
) -> Result<Json<ModerationMemberResp>, (StatusCode, String)> {
    let me = auth_user(&s, &req.session_token).await?;
    let is_super = ensure_moderator(&s, me).await?;
    if !can_access_chat(&s, me, req.chat_id).await? {
        return Err((StatusCode::FORBIDDEN, "not a member".into()));
    }
    let active = active_mute_for_user(&s, req.target_user_id)
        .await?
        .ok_or((StatusCode::NOT_FOUND, "active mute not found".into()))?;
    if active.muted_by != Some(me) && !is_super {
        return Err((
            StatusCode::FORBIDDEN,
            "only original moderator or super admin can unmute".into(),
        ));
    }
    let reason = req.reason.as_deref().map(trim_reason);
    let revoke_reason = reason.clone();
    let updated = sqlx::query!(
        r#"UPDATE user_mutes
           SET revoked_at = now(), revoked_by = $1, revoke_reason = $2
           WHERE id = $3 AND revoked_at IS NULL"#,
        me,
        revoke_reason,
        active.id
    )
    .execute(&s.db)
    .await
    .map_err(internal)?;
    if updated.rows_affected() == 0 {
        return Err((StatusCode::NOT_FOUND, "active mute not found".into()));
    }
    let target = user_brief(&s, req.target_user_id).await?;
    let moderator = user_brief(&s, me).await?;
    let event_payload = serde_json::json!({
        "mute_id": active.id,
        "scope": "global",
        "target": target,
        "moderator": moderator,
        "reason": reason.unwrap_or_else(|| "Unmuted".into())
    });
    let event_id = create_event(
        &s,
        Some(req.chat_id),
        "member_unmuted",
        None,
        Some(me),
        event_payload.clone(),
    )
    .await?;
    let payload = serde_json::json!({
        "type": "event",
        "event_id": event_id,
        "event_type": "member_unmuted",
        "chat_id": req.chat_id.to_string(),
        "actor_id": me.to_string(),
        "data": event_payload,
        "legacy_type": "member_unmuted"
    });
    broadcast_chat_event(&s, req.chat_id, &payload).await?;

    moderation_member(
        State(s),
        Query(ModerationMemberQ {
            session_token: req.session_token,
            chat_id: req.chat_id,
            target_user_id: req.target_user_id,
        }),
    )
    .await
}

pub async fn send(
    State(s): State<Arc<AppState>>,
    Json(req): Json<SendReq>,
) -> Result<Json<MessageOut>, (StatusCode, String)> {
    let me = auth_user(&s, &req.session_token).await?;
    let allowed_types = ["text", "image", "video", "gif", "sticker", "pack_share"];
    if !allowed_types.contains(&req.msg_type.as_str()) {
        return Err((StatusCode::BAD_REQUEST, "bad msg_type".into()));
    }
    ensure_can_write_chat(&s, me, req.chat_id).await?;
    let mentions = normalize_mentions(&s, req.chat_id, req.mentions).await?;

    let device_id = req
        .device_id
        .as_deref()
        .map(str::trim)
        .filter(|s| !s.is_empty())
        .map(|s| s.chars().take(128).collect::<String>());

    if let Some(client_msg_id) = req.client_msg_id {
        if let Some(existing) = sqlx::query(
            r#"SELECT id, created_at, msg_type, payload, reply_to_id, edited_at, deleted_at,
                      client_msg_id, sender_device_id
               FROM messages
               WHERE chat_id = $1 AND sender_id = $2 AND client_msg_id = $3"#,
        )
        .bind(req.chat_id)
        .bind(me)
        .bind(client_msg_id)
        .fetch_optional(&s.db)
        .await
        .map_err(internal)?
        {
            let message_id: i64 = existing.try_get("id").map_err(internal)?;
            let existing_event_id = message_event_id(&s, message_id).await?;
            let out = message_out_from_row(&s, &existing, req.chat_id, Some(me), existing_event_id)
                .await?;
            return Ok(Json(out));
        }
    }

    if let Some(reply_to_id) = req.reply_to_id {
        let reply_exists = sqlx::query_scalar!(
            "SELECT 1 as ok FROM messages WHERE id = $1 AND chat_id = $2 AND deleted_at IS NULL",
            reply_to_id,
            req.chat_id
        )
        .fetch_optional(&s.db)
        .await
        .map_err(internal)?
        .is_some();
        if !reply_exists {
            return Err((
                StatusCode::BAD_REQUEST,
                "reply target is not available".into(),
            ));
        }
    }

    let row = if let Some(client_msg_id) = req.client_msg_id {
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
    } else {
        sqlx::query(
            r#"INSERT INTO messages
                  (chat_id, sender_id, msg_type, payload, reply_to_id, sender_device_id)
               VALUES ($1, $2, $3, $4, $5, $6)
               RETURNING id, created_at, msg_type, payload, reply_to_id, edited_at, deleted_at,
                         client_msg_id, sender_device_id"#,
        )
        .bind(req.chat_id)
        .bind(me)
        .bind(&req.msg_type)
        .bind(&req.payload)
        .bind(req.reply_to_id)
        .bind(&device_id)
        .fetch_one(&s.db)
        .await
        .map_err(internal)?
    };

    let message_id: i64 = row.try_get("id").map_err(internal)?;
    for mentioned_user_id in &mentions {
        sqlx::query!(
            r#"INSERT INTO message_mentions (message_id, user_id)
               VALUES ($1, $2)
               ON CONFLICT DO NOTHING"#,
            message_id,
            mentioned_user_id
        )
        .execute(&s.db)
        .await
        .map_err(internal)?;
    }
    let existing_event_id = message_event_id(&s, message_id).await?;
    let out_without_event =
        message_out_from_row(&s, &row, req.chat_id, Some(me), existing_event_id).await?;

    let event_id = create_event(
        &s,
        Some(req.chat_id),
        "message",
        Some(message_id),
        Some(me),
        serde_json::json!({ "message": &out_without_event }),
    )
    .await?;

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
    broadcast_chat_event(&s, req.chat_id, &payload).await?;
    sqlx::query("UPDATE tickets SET updated_at = now() WHERE chat_id = $1")
        .bind(req.chat_id)
        .execute(&s.db)
        .await
        .map_err(internal)?;
    sqlx::query(
        "UPDATE forum_topics SET last_reply_at = now(), updated_at = now() WHERE chat_id = $1",
    )
    .bind(req.chat_id)
    .execute(&s.db)
    .await
    .map_err(internal)?;
    let _ = grant_xp_event(&s, me, "message_sent", 1, "message", message_id.to_string()).await;
    Ok(Json(out))
}

// ---------------- 历史消息 ----------------
#[derive(Deserialize)]
pub struct HistoryQ {
    pub session_token: String,
    pub chat_id: Uuid,
    pub before_id: Option<i64>,
    pub limit: Option<i64>,
}

pub async fn history(
    State(s): State<Arc<AppState>>,
    Query(q): Query<HistoryQ>,
) -> Result<Json<Vec<MessageOut>>, (StatusCode, String)> {
    let me = auth_user(&s, &q.session_token).await?;
    if !can_access_chat(&s, me, q.chat_id).await? {
        return Err((StatusCode::FORBIDDEN, "not a member".into()));
    }
    let limit = q.limit.unwrap_or(50).clamp(1, 200);
    let before = q.before_id.unwrap_or(i64::MAX);
    let rows = sqlx::query(
        r#"SELECT id, sender_id, msg_type, payload, reply_to_id, created_at, edited_at, deleted_at,
                  client_msg_id, sender_device_id
           FROM messages WHERE chat_id = $1 AND id < $2 AND deleted_at IS NULL
           ORDER BY id DESC LIMIT $3"#,
    )
    .bind(q.chat_id)
    .bind(before)
    .bind(limit)
    .fetch_all(&s.db)
    .await
    .map_err(internal)?;

    let mut out = Vec::with_capacity(rows.len());
    for r in rows {
        let sender_id = r
            .try_get::<Option<Uuid>, _>("sender_id")
            .map_err(internal)?;
        out.push(message_out_from_row(&s, &r, q.chat_id, sender_id, None).await?);
    }
    Ok(Json(out))
}

// ---------------- event sync ----------------
#[derive(Deserialize)]
pub struct SyncQ {
    pub session_token: String,
    pub after_event_id: Option<i64>,
    pub limit: Option<i64>,
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
                e.event_type <> 'message'
                OR EXISTS (
                    SELECT 1 FROM messages m
                    WHERE m.id = e.message_id AND m.deleted_at IS NULL
                )
             )
             AND (
                e.chat_id IS NULL
                OR (c.kind = 'channel' AND c.is_official = TRUE)
                OR (
                    EXISTS (
                        SELECT 1 FROM chat_members cm
                        WHERE cm.chat_id = e.chat_id AND cm.user_id = $2
                    )
                    AND NOT EXISTS (
                        SELECT 1 FROM tickets t_guard WHERE t_guard.chat_id = e.chat_id
                    )
                    AND NOT EXISTS (
                        SELECT 1 FROM forum_topics ft_guard WHERE ft_guard.chat_id = e.chat_id
                    )
                )
                OR EXISTS (
                    SELECT 1 FROM forum_topics ft
                    WHERE ft.chat_id = e.chat_id AND ft.status <> 'hidden'
                )
                OR EXISTS (
                    SELECT 1 FROM tickets t
                    WHERE t.chat_id = e.chat_id
                      AND (
                        t.creator_id = $2
                        OR (t.visibility = 'public' AND t.protected_by_superadmin = FALSE)
                        OR (
                            (SELECT COALESCE(is_admin, false) OR role IN ('admin','owner','super_admin') FROM users WHERE id = $2)
                            AND (
                                t.protected_by_superadmin = FALSE
                                OR (SELECT COALESCE(is_admin, false) AND role IN ('owner','super_admin') FROM users WHERE id = $2)
                            )
                        )
                      )
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
            chat_id: r
                .try_get::<Option<Uuid>, _>("chat_id")
                .map_err(internal)?
                .map(|v| v.to_string()),
            event_type: r.try_get("event_type").map_err(internal)?,
            message_id: r.try_get("message_id").map_err(internal)?,
            actor_id: r
                .try_get::<Option<Uuid>, _>("actor_id")
                .map_err(internal)?
                .map(|v| v.to_string()),
            payload: r.try_get("payload").map_err(internal)?,
            created_at: r
                .try_get::<DateTime<Utc>, _>("created_at")
                .map_err(internal)?
                .timestamp(),
        });
    }
    Ok(Json(out))
}

// ---------------- 标记已读 ----------------
#[derive(Deserialize)]
pub struct ReadReq {
    pub session_token: String,
    pub chat_id: Uuid,
    pub up_to_message_id: i64,
}

pub async fn mark_read(
    State(s): State<Arc<AppState>>,
    Json(req): Json<ReadReq>,
) -> Result<StatusCode, (StatusCode, String)> {
    let me = auth_user(&s, &req.session_token).await?;
    if !can_access_chat(&s, me, req.chat_id).await? {
        return Err((StatusCode::FORBIDDEN, "not a member".into()));
    }
    let target_chat = message_chat(&s, req.up_to_message_id).await?;
    if target_chat != req.chat_id {
        return Err((
            StatusCode::BAD_REQUEST,
            "read target is in another chat".into(),
        ));
    }
    sqlx::query!(
        r#"INSERT INTO chat_members (chat_id, user_id, last_read_message_id)
           VALUES ($1, $2, $3)
           ON CONFLICT (chat_id, user_id) DO UPDATE
           SET last_read_message_id = GREATEST(
               COALESCE(chat_members.last_read_message_id, 0),
               EXCLUDED.last_read_message_id
           )"#,
        req.chat_id,
        me,
        req.up_to_message_id
    )
    .execute(&s.db)
    .await
    .map_err(internal)?;
    let event_id = create_event(
        &s,
        Some(req.chat_id),
        "read",
        Some(req.up_to_message_id),
        Some(me),
        serde_json::json!({ "up_to_message_id": req.up_to_message_id }),
    )
    .await?;
    let payload = serde_json::json!({
        "type": "event",
        "event_id": event_id,
        "event_type": "read",
        "message_id": req.up_to_message_id,
        "chat_id": req.chat_id.to_string(),
        "actor_id": me.to_string(),
        "data": { "up_to_message_id": req.up_to_message_id },
        "legacy_type": "read"
    });
    broadcast_chat_event(&s, req.chat_id, &payload).await?;
    Ok(StatusCode::NO_CONTENT)
}

// ---------------- emoji react ----------------
#[derive(Deserialize)]
pub struct ReactReq {
    pub session_token: String,
    pub message_id: i64,
    pub emoji: String,
    pub remove: bool,
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
            req.message_id,
            me,
            emoji
        )
        .execute(&s.db)
        .await
        .map_err(internal)?;
    } else {
        let inserted = sqlx::query!(
            r#"INSERT INTO message_reactions (message_id, user_id, emoji)
               VALUES ($1,$2,$3) ON CONFLICT DO NOTHING"#,
            req.message_id,
            me,
            emoji
        )
        .execute(&s.db)
        .await
        .map_err(internal)?;
        if inserted.rows_affected() > 0 {
            if let Some(sender_id) = sqlx::query_scalar::<_, Option<Uuid>>(
                "SELECT sender_id FROM messages WHERE id = $1 AND sender_id IS NOT NULL",
            )
            .bind(req.message_id)
            .fetch_optional(&s.db)
            .await
            .map_err(internal)?
            .flatten()
            {
                if sender_id != me {
                    let _ = grant_xp_event(
                        &s,
                        sender_id,
                        "message_liked",
                        2,
                        "message",
                        req.message_id.to_string(),
                    )
                    .await;
                }
            }
        }
    }
    let event_id = create_event(
        &s,
        Some(chat_id),
        "reaction",
        Some(req.message_id),
        Some(me),
        serde_json::json!({ "emoji": emoji, "remove": req.remove }),
    )
    .await?;
    let payload = serde_json::json!({
        "type": "event",
        "event_id": event_id,
        "event_type": "reaction",
        "message_id": req.message_id,
        "chat_id": chat_id.to_string(),
        "actor_id": me.to_string(),
        "data": { "emoji": emoji, "remove": req.remove },
        "legacy_type": "reaction"
    });
    broadcast_chat_event(&s, chat_id, &payload).await?;
    Ok(StatusCode::NO_CONTENT)
}

// ---------------- 软删除消息 ----------------
#[derive(Deserialize)]
pub struct DeleteReq {
    pub session_token: String,
    pub message_id: i64,
}

pub async fn delete_msg(
    State(s): State<Arc<AppState>>,
    Json(req): Json<DeleteReq>,
) -> Result<StatusCode, (StatusCode, String)> {
    let me = auth_user(&s, &req.session_token).await?;
    let row = sqlx::query!(
        "SELECT sender_id, chat_id FROM messages WHERE id = $1 AND deleted_at IS NULL",
        req.message_id
    )
    .fetch_optional(&s.db)
    .await
    .map_err(internal)?
    .ok_or((StatusCode::NOT_FOUND, "msg not found".into()))?;
    if !can_access_chat(&s, me, row.chat_id).await? {
        return Err((StatusCode::FORBIDDEN, "not a member".into()));
    }
    if row.sender_id != Some(me) {
        return Err((StatusCode::FORBIDDEN, "not your message".into()));
    }
    let updated = sqlx::query!(
        "UPDATE messages SET deleted_at = now() WHERE id = $1 AND deleted_at IS NULL",
        req.message_id
    )
    .execute(&s.db)
    .await
    .map_err(internal)?;
    if updated.rows_affected() == 0 {
        return Err((StatusCode::NOT_FOUND, "msg not found".into()));
    }
    let event_id = create_event(
        &s,
        Some(row.chat_id),
        "message_deleted",
        Some(req.message_id),
        Some(me),
        serde_json::json!({ "message_id": req.message_id }),
    )
    .await?;
    let payload = serde_json::json!({
        "type": "event",
        "event_id": event_id,
        "event_type": "message_deleted",
        "message_id": req.message_id,
        "chat_id": row.chat_id.to_string(),
        "actor_id": me.to_string(),
        "data": { "message_id": req.message_id },
        "legacy_type": "message_deleted"
    });
    broadcast_chat_event(&s, row.chat_id, &payload).await?;
    Ok(StatusCode::NO_CONTENT)
}

// ---------------- 全文搜索 ----------------
// Ctrl+F 触发 — 模糊匹配 messages.payload (text/sticker_alias) 在所有当前用户可见的频道。
// 公开频道 + 用户加入的非公开频道。
#[derive(Deserialize)]
pub struct SearchQ {
    pub session_token: String,
    pub q: String,
    pub limit: Option<i64>,
}

#[derive(Serialize)]
pub struct SearchHit {
    pub id: i64,
    pub chat_id: String,
    pub chat_slug: Option<String>,
    pub sender_id: Option<String>,
    pub sender_uid: Option<String>,
    pub sender_username: Option<String>,
    pub sender_nickname: Option<String>,
    pub msg_type: String,
    pub payload: String,
    pub reply_to_id: Option<i64>,
    pub reply_snapshot: Option<ReplySnapshot>,
    pub mentions: Vec<MentionOut>,
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
    let pattern = format!(
        "%{}%",
        term.replace('\\', "\\\\")
            .replace('%', "\\%")
            .replace('_', "\\_")
    );
    let rows = sqlx::query!(
        r#"SELECT m.id, m.chat_id, m.sender_id, m.msg_type, m.reply_to_id,
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
        me,
        pattern,
        limit
    )
    .fetch_all(&s.db)
    .await
    .map_err(internal)?;
    let mut out = Vec::with_capacity(rows.len());
    for r in rows {
        let p = serde_json::from_str::<serde_json::Value>(&r.payload)
            .ok()
            .map(|v| match v {
                serde_json::Value::String(s) => s,
                other => other.to_string(),
            })
            .unwrap_or(r.payload);
        let sender = match r.sender_id {
            Some(uid) => user_brief(&s, uid).await?,
            None => None,
        };
        out.push(SearchHit {
            id: r.id,
            chat_id: r.chat_id.to_string(),
            chat_slug: r.slug,
            sender_id: r.sender_id.map(|u| u.to_string()),
            sender_uid: sender.as_ref().map(|u| u.uid.clone()),
            sender_username: sender.as_ref().map(|u| u.username.clone()),
            sender_nickname: sender.as_ref().and_then(|u| u.nickname.clone()),
            msg_type: r.msg_type,
            payload: p,
            reply_to_id: r.reply_to_id,
            reply_snapshot: reply_snapshot_for(&s, r.reply_to_id).await?,
            mentions: mentions_for_message(&s, r.id).await?,
            created_at: r.created_at.timestamp(),
        });
    }
    Ok(Json(out))
}
