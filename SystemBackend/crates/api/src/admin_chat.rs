use crate::state::AppState;
use crate::ui;
use crate::ws;
use askama::Template;
use axum::{
    extract::{Form, Query, State},
    http::HeaderMap,
    response::{IntoResponse, Redirect, Response},
    routing::{get, post},
    Router,
};
use chrono::{DateTime, Utc};
use launcher_shared::{hashing, uid as shared_uid};
use serde::Deserialize;
use sqlx::Row;
use std::sync::Arc;
use uuid::Uuid;

const SUPERADMIN_USERNAME: &str = "SuperAdmin";
const SUPERADMIN_SYSTEM_USERNAME: &str = "launcher/system/superadmin";
const SUPERADMIN_NICKNAME: &str = "SuperAdmin";
const SUPERADMIN_ROLE: &str = "owner";
const SUPERADMIN_ROLE_LABEL: &str = "最高管理员";
const SUPERADMIN_SYSTEM_NOTE: &str = "launcher.system.superadmin.v1";

#[derive(Clone)]
pub struct AdminChatChannelVm {
    pub id: String,
    pub display_title: String,
    pub group_label_zh: String,
    pub message_count: i64,
    pub last_at: String,
    pub is_official: bool,
    pub write_policy_label: String,
    pub active: bool,
}

pub struct AdminChatMessageVm {
    pub id: i64,
    pub sender: String,
    pub role_label: String,
    pub msg_type_label: String,
    pub text: String,
    pub created_at: String,
    pub edited: bool,
    pub deleted: bool,
    pub reply_to: String,
}

#[derive(Template)]
#[template(path = "admin_chat_content.html")]
pub struct AdminChatPage {
    pub title: String,
    pub subtitle: Option<String>,
    pub notice: Option<ui::AdminNotice>,
    pub host: &'static str,
    pub route: &'static str,
    pub channels: Vec<AdminChatChannelVm>,
    pub selected: Option<AdminChatChannelVm>,
    pub messages: Vec<AdminChatMessageVm>,
    pub superadmin_id: String,
    pub superadmin_uid: String,
}

#[derive(Deserialize, Default)]
pub struct ChatPageQuery {
    pub chat_id: Option<Uuid>,
    pub ok: Option<String>,
    pub err: Option<String>,
}

#[derive(Deserialize)]
pub struct SendForm {
    pub chat_id: Uuid,
    pub msg_type: String,
    pub text: String,
    pub reply_to_id: Option<String>,
}

struct AdminSender {
    id: Uuid,
    uid: String,
}

fn internal<E: std::fmt::Display>(e: E) -> String {
    e.to_string()
}

fn notice(q: &ChatPageQuery) -> Option<ui::AdminNotice> {
    match (q.ok.as_deref(), q.err.as_deref()) {
        (Some("sent"), _) => Some(ui::AdminNotice::success("消息已通过 SuperAdmin 发送")),
        (_, Some("bad_text")) => Some(ui::AdminNotice::warning("消息内容不能为空，最多 4000 字")),
        (_, Some("bad_type")) => Some(ui::AdminNotice::warning("消息类型无效")),
        (_, Some("bad_reply")) => Some(ui::AdminNotice::warning("回复目标不存在或不在当前频道")),
        (_, Some("bad_chat")) => Some(ui::AdminNotice::warning("频道不存在")),
        (_, Some("failed")) => Some(ui::AdminNotice::error("发送失败，请检查服务日志")),
        _ => None,
    }
}

fn fmt_ts(ts: Option<DateTime<Utc>>) -> String {
    ts.map(|t| t.format("%Y-%m-%d %H:%M:%S").to_string())
        .unwrap_or_else(|| "-".into())
}

fn group_label_zh(v: &str) -> &'static str {
    match v {
        "IMPORTANT" => "重要",
        "GENERAL" => "通用",
        "GAMES" => "游戏",
        "SHOP" => "交易",
        _ => "其他",
    }
}

fn write_policy_label(v: &str) -> &'static str {
    match v {
        "everyone" => "所有人",
        "admin_only" => "仅管理员",
        "role_only" => "指定角色",
        "min_level" => "最低等级",
        "subscriber_only" => "订阅用户",
        "readonly" => "只读",
        "locked" => "锁定",
        _ => "未知",
    }
}

fn msg_type_label(v: &str) -> &'static str {
    match v {
        "text" => "普通文本",
        "system" => "系统消息",
        "image" => "图片",
        "video" => "视频",
        "gif" => "GIF",
        "sticker" => "表情",
        "pack_share" => "表情包分享",
        _ => "未知",
    }
}

fn channel_display_title(slug: &str, title: &str) -> String {
    let label = match slug {
        "cs2" => Some("CS2"),
        "general" => Some("综合"),
        "helpdesk" => Some("帮助"),
        "random" => Some("闲聊"),
        "announcements" => Some("公告"),
        "rules" => Some("规则"),
        "market" => Some("市场"),
        "trades" => Some("交易"),
        _ => None,
    };
    label
        .map(str::to_string)
        .unwrap_or_else(|| title.trim().to_string())
}

fn text_from_payload(payload: &serde_json::Value, msg_type: &str) -> String {
    if let Some(text) = payload.get("text").and_then(|v| v.as_str()) {
        return text.to_string();
    }
    if let Some(s) = payload.as_str() {
        return s.to_string();
    }
    match msg_type {
        "image" => "[图片]".into(),
        "video" => "[视频]".into(),
        "gif" => "[GIF]".into(),
        "sticker" => "[表情]".into(),
        "pack_share" => "[表情包分享]".into(),
        "system" => "[系统消息]".into(),
        _ => payload.to_string(),
    }
}

async fn unique_uid(state: &AppState) -> Result<String, String> {
    for _ in 0..10 {
        let candidate = shared_uid::generate();
        let exists = sqlx::query_scalar::<_, i32>("SELECT 1 FROM users WHERE uid = $1")
            .bind(&candidate)
            .fetch_optional(&state.db)
            .await
            .map_err(internal)?;
        if exists.is_none() {
            return Ok(candidate);
        }
    }
    Err("UID gen failed".into())
}

async fn ensure_superadmin_user(state: &AppState) -> Result<AdminSender, String> {
    let username_hash = hashing::salt_hwid(SUPERADMIN_SYSTEM_USERNAME, b"launcher.user.salt.v1");
    if let Some(row) = sqlx::query(
        r#"SELECT id, uid FROM users
           WHERE note = $1
           ORDER BY created_at ASC
           LIMIT 1"#,
    )
    .bind(SUPERADMIN_SYSTEM_NOTE)
    .fetch_optional(&state.db)
    .await
    .map_err(internal)?
    {
        let id: Uuid = row.try_get("id").map_err(internal)?;
        let uid = match row.try_get::<Option<String>, _>("uid").map_err(internal)? {
            Some(v) if !v.trim().is_empty() => v,
            _ => unique_uid(state).await?,
        };
        sqlx::query(
            r#"UPDATE users
               SET username_hash = $1,
                   username = $2,
                   nickname = $3,
                   uid = $4,
                   role = $5,
                   role_label = $6,
                   is_admin = TRUE,
                   note = $7
               WHERE id = $8"#,
        )
        .bind(&username_hash)
        .bind(SUPERADMIN_SYSTEM_USERNAME)
        .bind(SUPERADMIN_NICKNAME)
        .bind(&uid)
        .bind(SUPERADMIN_ROLE)
        .bind(SUPERADMIN_ROLE_LABEL)
        .bind(SUPERADMIN_SYSTEM_NOTE)
        .bind(id)
        .execute(&state.db)
        .await
        .map_err(internal)?;
        return Ok(AdminSender { id, uid });
    }

    let occupied = sqlx::query_scalar::<_, i32>(
        "SELECT 1 FROM users WHERE username_hash = $1 OR username = $2 LIMIT 1",
    )
    .bind(&username_hash)
    .bind(SUPERADMIN_SYSTEM_USERNAME)
    .fetch_optional(&state.db)
    .await
    .map_err(internal)?;
    if occupied.is_some() {
        return Err("SuperAdmin system username is occupied by a non-system account".into());
    }

    let uid = unique_uid(state).await?;
    let random_password = Uuid::new_v4().to_string();
    let pw_hash = hashing::hash_password(
        &random_password,
        state.cfg.argon_memory_kib,
        state.cfg.argon_iterations,
    )
    .map_err(internal)?;
    let row = sqlx::query(
        r#"INSERT INTO users
              (username_hash, password_hash, hwid_bound, uid, username, nickname,
               role, role_label, is_admin, password_changed_at, created_at, note)
           VALUES ($1, $2, NULL, $3, $4, $5, $6, $7, TRUE, now(), now(), $8)
           RETURNING id"#,
    )
    .bind(username_hash)
    .bind(pw_hash)
    .bind(&uid)
    .bind(SUPERADMIN_SYSTEM_USERNAME)
    .bind(SUPERADMIN_NICKNAME)
    .bind(SUPERADMIN_ROLE)
    .bind(SUPERADMIN_ROLE_LABEL)
    .bind(SUPERADMIN_SYSTEM_NOTE)
    .fetch_one(&state.db)
    .await
    .map_err(internal)?;
    Ok(AdminSender {
        id: row.try_get("id").map_err(internal)?,
        uid,
    })
}

async fn list_channels(
    state: &AppState,
    selected_id: Option<Uuid>,
) -> Result<Vec<AdminChatChannelVm>, String> {
    let rows = sqlx::query(
        r#"SELECT c.id, c.slug, c.title, c.group_label, c.is_official, c.last_message_at,
                  COALESCE(cs.write_policy, CASE WHEN c.write_role = 'admin_only' THEN 'admin_only' ELSE 'everyone' END) AS write_policy,
                  (SELECT COUNT(*) FROM messages m WHERE m.chat_id = c.id AND m.deleted_at IS NULL) AS msg_count
           FROM chats c
           LEFT JOIN channel_settings cs ON cs.chat_id = c.id
           WHERE c.kind = 'channel'
           ORDER BY c.is_official DESC, c.group_label NULLS LAST, c.title ASC"#,
    )
    .fetch_all(&state.db)
    .await
    .map_err(internal)?;

    let mut out = Vec::with_capacity(rows.len());
    for r in rows {
        let id: Uuid = r.try_get("id").map_err(internal)?;
        let group_label = r
            .try_get::<Option<String>, _>("group_label")
            .map_err(internal)?
            .unwrap_or_default();
        let write_policy: String = r.try_get("write_policy").map_err(internal)?;
        let slug = r
            .try_get::<Option<String>, _>("slug")
            .map_err(internal)?
            .unwrap_or_default();
        let title = r
            .try_get::<Option<String>, _>("title")
            .map_err(internal)?
            .unwrap_or_default();
        let display_title = channel_display_title(&slug, &title);
        out.push(AdminChatChannelVm {
            id: id.to_string(),
            display_title,
            group_label_zh: group_label_zh(&group_label).into(),
            message_count: r
                .try_get::<Option<i64>, _>("msg_count")
                .map_err(internal)?
                .unwrap_or(0),
            last_at: fmt_ts(r.try_get("last_message_at").map_err(internal)?),
            is_official: r.try_get("is_official").map_err(internal)?,
            write_policy_label: write_policy_label(&write_policy).into(),
            active: selected_id == Some(id),
        });
    }
    Ok(out)
}

async fn list_messages(state: &AppState, chat_id: Uuid) -> Result<Vec<AdminChatMessageVm>, String> {
    let rows = sqlx::query(
        r#"SELECT m.id, m.msg_type, m.payload, m.reply_to_id, m.created_at, m.edited_at, m.deleted_at,
                  u.username, u.nickname, u.role_label
           FROM messages m
           LEFT JOIN users u ON u.id = m.sender_id
           WHERE m.chat_id = $1
           ORDER BY m.id DESC
           LIMIT 120"#,
    )
    .bind(chat_id)
    .fetch_all(&state.db)
    .await
    .map_err(internal)?;

    let mut out = Vec::with_capacity(rows.len());
    for r in rows.into_iter().rev() {
        let payload: serde_json::Value = r.try_get("payload").map_err(internal)?;
        let msg_type: String = r.try_get("msg_type").map_err(internal)?;
        let nickname: Option<String> = r.try_get("nickname").map_err(internal)?;
        let username: Option<String> = r.try_get("username").map_err(internal)?;
        let role_label: Option<String> = r.try_get("role_label").map_err(internal)?;
        let reply_to: Option<i64> = r.try_get("reply_to_id").map_err(internal)?;
        let edited_at: Option<DateTime<Utc>> = r.try_get("edited_at").map_err(internal)?;
        let deleted_at: Option<DateTime<Utc>> = r.try_get("deleted_at").map_err(internal)?;
        out.push(AdminChatMessageVm {
            id: r.try_get("id").map_err(internal)?,
            sender: nickname
                .filter(|v| !v.trim().is_empty())
                .or(username)
                .unwrap_or_else(|| "系统".into()),
            role_label: role_label.unwrap_or_default(),
            text: text_from_payload(&payload, &msg_type),
            msg_type_label: msg_type_label(&msg_type).into(),
            created_at: fmt_ts(Some(r.try_get("created_at").map_err(internal)?)),
            edited: edited_at.is_some(),
            deleted: deleted_at.is_some(),
            reply_to: reply_to
                .map(|v| format!("#{v}"))
                .unwrap_or_else(|| "-".into()),
        });
    }
    Ok(out)
}

async fn validate_channel(state: &AppState, chat_id: Uuid) -> Result<bool, String> {
    let exists =
        sqlx::query_scalar::<_, i32>("SELECT 1 FROM chats WHERE id = $1 AND kind = 'channel'")
            .bind(chat_id)
            .fetch_optional(&state.db)
            .await
            .map_err(internal)?;
    Ok(exists.is_some())
}

async fn reply_target_ok(
    state: &AppState,
    chat_id: Uuid,
    reply_to_id: Option<i64>,
) -> Result<bool, String> {
    let Some(message_id) = reply_to_id else {
        return Ok(true);
    };
    let exists = sqlx::query_scalar::<_, i32>(
        "SELECT 1 FROM messages WHERE id = $1 AND chat_id = $2 AND deleted_at IS NULL",
    )
    .bind(message_id)
    .bind(chat_id)
    .fetch_optional(&state.db)
    .await
    .map_err(internal)?;
    Ok(exists.is_some())
}

async fn broadcast_admin_message(
    state: &AppState,
    chat_id: Uuid,
    payload: &serde_json::Value,
) -> Result<(), String> {
    let row = sqlx::query("SELECT is_official FROM chats WHERE id = $1")
        .bind(chat_id)
        .fetch_optional(&state.db)
        .await
        .map_err(internal)?
        .ok_or_else(|| "chat not found".to_string())?;
    let is_official: bool = row.try_get("is_official").map_err(internal)?;
    if is_official {
        ws::broadcast_all(state, payload);
    } else {
        let users =
            sqlx::query_scalar::<_, Uuid>("SELECT user_id FROM chat_members WHERE chat_id = $1")
                .bind(chat_id)
                .fetch_all(&state.db)
                .await
                .map_err(internal)?;
        for uid in users {
            ws::push_to(state, uid, payload);
        }
    }
    Ok(())
}

async fn insert_admin_message(
    state: &AppState,
    sender: &AdminSender,
    actor_name: &str,
    chat_id: Uuid,
    msg_type: &str,
    text: &str,
    reply_to_id: Option<i64>,
) -> Result<(), String> {
    let payload = if msg_type == "text" {
        serde_json::json!(text)
    } else {
        serde_json::json!({
            "text": text,
            "source": "admin_web",
        })
    };
    let row = sqlx::query(
        r#"INSERT INTO messages
              (chat_id, sender_id, msg_type, payload, reply_to_id, sender_device_id)
           VALUES ($1, $2, $3, $4, $5, 'admin-web')
           RETURNING id, created_at"#,
    )
    .bind(chat_id)
    .bind(sender.id)
    .bind(msg_type)
    .bind(&payload)
    .bind(reply_to_id)
    .fetch_one(&state.db)
    .await
    .map_err(internal)?;
    let message_id: i64 = row.try_get("id").map_err(internal)?;
    let created_at: DateTime<Utc> = row.try_get("created_at").map_err(internal)?;
    let message = serde_json::json!({
        "id": message_id,
        "chat_id": chat_id.to_string(),
        "sender_id": sender.id.to_string(),
        "sender_uid": sender.uid.clone(),
        "sender_username": SUPERADMIN_USERNAME,
        "sender_nickname": SUPERADMIN_NICKNAME,
        "sender_role": SUPERADMIN_ROLE,
        "sender_role_label": SUPERADMIN_ROLE_LABEL,
        "msg_type": msg_type,
        "payload": payload,
        "reply_to_id": reply_to_id,
        "reply_snapshot": null,
        "created_at": created_at.timestamp(),
        "edited_at": null,
        "deleted": false,
        "client_msg_id": null,
        "sender_device_id": "admin-web",
        "mentions": [],
        "event_id": null
    });
    let event_row = sqlx::query(
        r#"INSERT INTO chat_events (chat_id, event_type, message_id, actor_id, payload)
           VALUES ($1, 'message', $2, $3, $4)
           RETURNING id"#,
    )
    .bind(chat_id)
    .bind(message_id)
    .bind(sender.id)
    .bind(serde_json::json!({ "message": &message }))
    .fetch_one(&state.db)
    .await
    .map_err(internal)?;
    let event_id: i64 = event_row.try_get("id").map_err(internal)?;
    let mut pushed_message = message;
    if let Some(obj) = pushed_message.as_object_mut() {
        obj.insert("event_id".into(), serde_json::json!(event_id));
    }
    let push_payload = serde_json::json!({
        "type": "event",
        "event_id": event_id,
        "event_type": "message",
        "chat_id": chat_id.to_string(),
        "server_time": created_at.timestamp(),
        "data": pushed_message,
        "legacy_type": "message"
    });
    broadcast_admin_message(state, chat_id, &push_payload).await?;
    sqlx::query(
        "INSERT INTO audit_log (actor, action, target, metadata) VALUES ($1, 'admin.chat_send', $2, $3)",
    )
    .bind(actor_name)
    .bind(chat_id.to_string())
    .bind(serde_json::json!({
        "message_id": message_id,
        "msg_type": msg_type,
        "sender": SUPERADMIN_USERNAME
    }))
    .execute(&state.db)
    .await
    .ok();
    Ok(())
}

async fn page(
    State(s): State<Arc<AppState>>,
    headers: HeaderMap,
    Query(q): Query<ChatPageQuery>,
) -> Response {
    let actor =
        match crate::admin_customization::require_actor(&headers, &s, "admin.chat.read").await {
            Ok(v) => v,
            Err(resp) => return resp,
        };
    let sender = match ensure_superadmin_user(&s).await {
        Ok(v) => v,
        Err(_) => {
            return ui::render(&AdminChatPage {
                title: "聊天控制台".into(),
                subtitle: Some("SuperAdmin 后台频道发言与消息查看".into()),
                notice: Some(ui::AdminNotice::error("SuperAdmin 初始化失败")),
                host: ui::host(),
                route: ui::ROUTE_CHAT,
                channels: vec![],
                selected: None,
                messages: vec![],
                superadmin_id: "-".into(),
                superadmin_uid: "-".into(),
            })
            .into_response();
        }
    };

    let initial_channels = list_channels(&s, q.chat_id).await.unwrap_or_default();
    let selected_uuid = q.chat_id.or_else(|| {
        initial_channels
            .first()
            .and_then(|c| Uuid::parse_str(&c.id).ok())
    });
    let channels = list_channels(&s, selected_uuid).await.unwrap_or_default();
    let selected =
        selected_uuid.and_then(|id| channels.iter().find(|c| c.id == id.to_string()).cloned());
    let messages = match selected_uuid {
        Some(id) => list_messages(&s, id).await.unwrap_or_default(),
        None => vec![],
    };

    ui::render(&AdminChatPage {
        title: "聊天控制台".into(),
        subtitle: Some(format!(
            "{} · {} · 后台专用 SuperAdmin 账号，可查看频道并发送消息",
            actor.display_name, actor.role
        )),
        notice: notice(&q),
        host: ui::host(),
        route: ui::ROUTE_CHAT,
        channels,
        selected,
        messages,
        superadmin_id: sender.id.to_string(),
        superadmin_uid: sender.uid,
    })
    .into_response()
}

async fn send_submit(
    State(s): State<Arc<AppState>>,
    headers: HeaderMap,
    Form(form): Form<SendForm>,
) -> Response {
    let actor =
        match crate::admin_customization::require_actor(&headers, &s, "admin.chat.send").await {
            Ok(v) => v,
            Err(resp) => return resp,
        };
    let chat_id = form.chat_id;
    let base = format!("/admin/chat?chat_id={chat_id}");
    if !matches!(form.msg_type.as_str(), "text" | "system") {
        return Redirect::to(&format!("{base}&err=bad_type")).into_response();
    }
    let text = form.text.trim().chars().take(4000).collect::<String>();
    if text.is_empty() {
        return Redirect::to(&format!("{base}&err=bad_text")).into_response();
    }
    let reply_to_id = form
        .reply_to_id
        .as_deref()
        .map(str::trim)
        .filter(|v| !v.is_empty())
        .and_then(|v| v.parse::<i64>().ok());

    let result = async {
        if !validate_channel(&s, chat_id).await? {
            return Err("bad_chat".to_string());
        }
        if !reply_target_ok(&s, chat_id, reply_to_id).await? {
            return Err("bad_reply".to_string());
        }
        let sender = ensure_superadmin_user(&s).await?;
        insert_admin_message(
            &s,
            &sender,
            &actor.name,
            chat_id,
            &form.msg_type,
            &text,
            reply_to_id,
        )
        .await?;
        Ok::<(), String>(())
    }
    .await;

    match result {
        Ok(()) => Redirect::to(&format!("{base}&ok=sent")).into_response(),
        Err(e) if e == "bad_chat" => Redirect::to("/admin/chat?err=bad_chat").into_response(),
        Err(e) if e == "bad_reply" => {
            Redirect::to(&format!("{base}&err=bad_reply")).into_response()
        }
        Err(_) => Redirect::to(&format!("{base}&err=failed")).into_response(),
    }
}

pub fn routes() -> Router<Arc<AppState>> {
    Router::new()
        .route("/admin/chat", get(page))
        .route("/admin/chat/send", post(send_submit))
}
