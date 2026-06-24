// 官方频道 admin SSR — 列出 / 改名 / 清空消息（不允许删，因为客户端写死）。
// DaisyUI dropdown 行级操作 + dialog modal 改名。

use crate::state::AppState;
use crate::ui;
use askama::Template;
use axum::{
    extract::{Form, Path, Query, State},
    http::HeaderMap,
    response::{IntoResponse, Redirect, Response},
    routing::{get, post},
    Router,
};
use serde::Deserialize;
use sqlx::Row;
use std::sync::Arc;
use uuid::Uuid;

pub struct ChannelVm {
    pub id: String,
    pub slug: String,
    pub title: String,
    pub display_title: String,
    pub group_label_zh: String,
    pub message_count: i64,
    pub last_at: String,
    pub is_official: bool,
    pub write_policy: String,
    pub write_policy_label: String,
    pub allowed_role: String,
    pub min_level: i32,
    pub slowmode_seconds: i32,
    pub requires_subscription: bool,
    pub is_readonly: bool,
    pub is_locked: bool,
    pub has_settings: bool,
    pub settings_enabled: bool,
    pub access_visible: bool,
    pub access_write_allowed: bool,
    pub access_reason: String,
}

#[derive(Clone)]
pub struct ChannelUserOptionVm {
    pub id: String,
    pub label: String,
}

#[derive(Clone)]
struct ChannelAccessUser {
    id: Uuid,
    label: String,
    role: String,
    is_admin: bool,
    level: i32,
    subscribed: bool,
}

#[derive(Template)]
#[template(path = "channels_content.html")]
pub struct ChannelsPage {
    pub title: String,
    pub subtitle: Option<String>,
    pub notice: Option<ui::AdminNotice>,
    pub host: &'static str,
    pub route: &'static str,
    pub channels: Vec<ChannelVm>,
    pub users: Vec<ChannelUserOptionVm>,
    pub selected_user_id: String,
    pub selected_user_label: String,
    pub access_check_enabled: bool,
}

#[derive(Deserialize, Default)]
pub struct ChannelsNoticeQuery {
    pub ok: Option<String>,
    pub err: Option<String>,
    pub view_user_id: Option<Uuid>,
}

fn channels_notice(q: &ChannelsNoticeQuery) -> Option<ui::AdminNotice> {
    match (q.ok.as_deref(), q.err.as_deref()) {
        (Some("rename"), _) => Some(ui::AdminNotice::success("频道名称已保存")),
        (Some("policy"), _) => Some(ui::AdminNotice::success("频道权限已保存")),
        (Some("clear"), _) => Some(ui::AdminNotice::success("频道消息已清空")),
        (_, Some("title")) => Some(ui::AdminNotice::warning(
            "频道标题不能为空，且最多 64 个字符",
        )),
        (_, Some("policy")) => Some(ui::AdminNotice::warning("频道权限参数无效")),
        _ => None,
    }
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

async fn channels_page(
    State(s): State<Arc<AppState>>,
    headers: HeaderMap,
    Query(q): Query<ChannelsNoticeQuery>,
) -> Response {
    if let Err(resp) =
        crate::admin_customization::require_actor(&headers, &s, "admin.channels.read").await
    {
        return resp;
    }

    let rows = sqlx::query(
        r#"SELECT c.id, c.slug, c.title, c.group_label, c.is_official, c.last_message_at,
                  c.write_role,
                  (SELECT COUNT(*) FROM messages m WHERE m.chat_id = c.id AND m.deleted_at IS NULL) AS msg_count,
                  COALESCE(cs.write_policy, CASE WHEN c.write_role = 'admin_only' THEN 'admin_only' ELSE 'everyone' END) AS write_policy,
                  cs.allowed_role,
                  COALESCE(cs.min_level, 1) AS min_level,
                  COALESCE(cs.slowmode_seconds, 0) AS slowmode_seconds,
                  COALESCE(cs.requires_subscription, FALSE) AS requires_subscription,
                  COALESCE(cs.is_readonly, FALSE) AS is_readonly,
                  COALESCE(cs.is_locked, FALSE) AS is_locked,
                  cs.chat_id IS NOT NULL AS has_settings,
                  COALESCE(cs.is_enabled, FALSE) AS settings_enabled
           FROM chats c
           LEFT JOIN channel_settings cs ON cs.chat_id = c.id
           WHERE c.kind = 'channel'
           ORDER BY c.is_official DESC, c.group_label NULLS LAST, c.title"#)
        .fetch_all(&s.db).await.unwrap_or_default();

    let users = list_access_users(&s).await;
    let selected_user = selected_access_user(&s, q.view_user_id, &users).await;
    let access_check_enabled = selected_user.is_some();
    let selected_user_id = selected_user
        .as_ref()
        .map(|u| u.id.to_string())
        .unwrap_or_default();
    let selected_user_label = selected_user
        .as_ref()
        .map(|u| u.label.clone())
        .unwrap_or_else(|| "未选择用户".into());

    let mut channels = Vec::with_capacity(rows.len());
    for r in rows {
        let group_label = r
            .try_get::<Option<String>, _>("group_label")
            .ok()
            .flatten()
            .unwrap_or_else(|| "—".into());
        let write_policy: String = r
            .try_get("write_policy")
            .unwrap_or_else(|_| "everyone".into());
        let slug = r
            .try_get::<Option<String>, _>("slug")
            .ok()
            .flatten()
            .unwrap_or_default();
        let title = r
            .try_get::<Option<String>, _>("title")
            .ok()
            .flatten()
            .unwrap_or_default();
        let display_title = channel_display_title(&slug, &title);
        let mut channel = ChannelVm {
            id: r
                .try_get::<Uuid, _>("id")
                .map(|v| v.to_string())
                .unwrap_or_default(),
            slug,
            title,
            display_title,
            group_label_zh: group_label_zh(&group_label).into(),
            message_count: r
                .try_get::<Option<i64>, _>("msg_count")
                .ok()
                .flatten()
                .unwrap_or(0),
            last_at: r
                .try_get::<Option<chrono::DateTime<chrono::Utc>>, _>("last_message_at")
                .ok()
                .flatten()
                .map(|t| t.format("%Y-%m-%d %H:%M").to_string())
                .unwrap_or_else(|| "—".into()),
            is_official: r.try_get("is_official").unwrap_or(false),
            write_policy_label: write_policy_label(&write_policy).into(),
            write_policy,
            allowed_role: r
                .try_get::<Option<String>, _>("allowed_role")
                .ok()
                .flatten()
                .unwrap_or_default(),
            min_level: r.try_get("min_level").unwrap_or(1),
            slowmode_seconds: r.try_get("slowmode_seconds").unwrap_or(0),
            requires_subscription: r.try_get("requires_subscription").unwrap_or(false),
            is_readonly: r.try_get("is_readonly").unwrap_or(false),
            is_locked: r.try_get("is_locked").unwrap_or(false),
            has_settings: r.try_get("has_settings").unwrap_or(false),
            settings_enabled: r.try_get("settings_enabled").unwrap_or(false),
            access_visible: false,
            access_write_allowed: false,
            access_reason: String::new(),
        };
        if let Some(user) = selected_user.as_ref() {
            apply_channel_access_check(&s, &mut channel, user).await;
        }
        channels.push(channel);
    }

    ui::render(&ChannelsPage {
        title: "频道管控".into(),
        subtitle: Some("频道管理同时控制客户端频道列表、发言权限和真实用户视角校验".into()),
        notice: channels_notice(&q),
        host: ui::host(),
        route: ui::ROUTE_CHANNELS,
        channels,
        users,
        selected_user_id,
        selected_user_label,
        access_check_enabled,
    })
    .into_response()
}

async fn list_access_users(state: &AppState) -> Vec<ChannelUserOptionVm> {
    let rows = sqlx::query(
        r#"SELECT id, uid, username, nickname, role, role_label
           FROM users
           ORDER BY created_at DESC
           LIMIT 200"#,
    )
    .fetch_all(&state.db)
    .await
    .unwrap_or_default();

    rows.into_iter()
        .map(|r| {
            let id = r
                .try_get::<Uuid, _>("id")
                .map(|v| v.to_string())
                .unwrap_or_default();
            let uid = r
                .try_get::<Option<String>, _>("uid")
                .ok()
                .flatten()
                .unwrap_or_else(|| "—".into());
            let username = r
                .try_get::<Option<String>, _>("username")
                .ok()
                .flatten()
                .unwrap_or_else(|| "—".into());
            let nickname = r
                .try_get::<Option<String>, _>("nickname")
                .ok()
                .flatten()
                .unwrap_or_default();
            let role = r
                .try_get::<String, _>("role")
                .unwrap_or_else(|_| "user".into());
            let role_label = r
                .try_get::<Option<String>, _>("role_label")
                .ok()
                .flatten()
                .unwrap_or_default();
            let display = if nickname.is_empty() || nickname == username {
                username
            } else {
                format!("{nickname} / {username}")
            };
            let suffix = if role_label.is_empty() {
                role
            } else {
                format!("{role_label} ({role})")
            };
            ChannelUserOptionVm {
                id,
                label: format!("{display} · UID {uid} · {suffix}"),
            }
        })
        .collect()
}

async fn selected_access_user(
    state: &AppState,
    requested: Option<Uuid>,
    options: &[ChannelUserOptionVm],
) -> Option<ChannelAccessUser> {
    let selected_id =
        requested.or_else(|| options.first().and_then(|u| Uuid::parse_str(&u.id).ok()))?;
    let row = sqlx::query(
        r#"SELECT u.id, u.uid, u.username, u.nickname, u.role, u.role_label, u.is_admin,
                  COALESCE(up.level, 1) AS level,
                  (
                    EXISTS (
                      SELECT 1 FROM subscriptions s
                      WHERE s.user_id = u.id
                        AND (s.expires_at IS NULL OR s.expires_at > now())
                    )
                    OR (
                      u.subscription_tier IS NOT NULL
                      AND u.subscription_tier <> ''
                      AND (u.subscription_expires_at IS NULL OR u.subscription_expires_at > now())
                    )
                  ) AS subscribed
           FROM users u
           LEFT JOIN user_progress up ON up.user_id = u.id
           WHERE u.id = $1"#,
    )
    .bind(selected_id)
    .fetch_optional(&state.db)
    .await
    .ok()
    .flatten()?;

    let uid = row
        .try_get::<Option<String>, _>("uid")
        .ok()
        .flatten()
        .unwrap_or_else(|| "—".into());
    let username = row
        .try_get::<Option<String>, _>("username")
        .ok()
        .flatten()
        .unwrap_or_else(|| "—".into());
    let nickname = row
        .try_get::<Option<String>, _>("nickname")
        .ok()
        .flatten()
        .unwrap_or_default();
    let role = row
        .try_get::<String, _>("role")
        .unwrap_or_else(|_| "user".into());
    let role_label = row
        .try_get::<Option<String>, _>("role_label")
        .ok()
        .flatten()
        .unwrap_or_default();
    let display = if nickname.is_empty() || nickname == username {
        username
    } else {
        format!("{nickname} / {username}")
    };
    let suffix = if role_label.is_empty() {
        role.clone()
    } else {
        format!("{role_label} ({role})")
    };
    Some(ChannelAccessUser {
        id: selected_id,
        label: format!("{display} · UID {uid} · {suffix}"),
        role,
        is_admin: row.try_get("is_admin").unwrap_or(false),
        level: row.try_get("level").unwrap_or(1),
        subscribed: row.try_get("subscribed").unwrap_or(false),
    })
}

fn is_admin_like(user: &ChannelAccessUser) -> bool {
    user.is_admin || matches!(user.role.as_str(), "admin" | "owner" | "super_admin")
}

async fn apply_channel_access_check(
    state: &AppState,
    channel: &mut ChannelVm,
    user: &ChannelAccessUser,
) {
    if !channel.has_settings {
        channel.access_visible = false;
        channel.access_write_allowed = false;
        channel.access_reason = "客户端 bootstrap 不显示：缺少 channel_settings".into();
        return;
    }
    if !channel.settings_enabled {
        channel.access_visible = false;
        channel.access_write_allowed = false;
        channel.access_reason = "客户端 bootstrap 不显示：频道设置已禁用".into();
        return;
    }

    channel.access_visible = true;
    let admin_like = is_admin_like(user);
    let mut allow = true;
    let mut reason = "可见且可发言".to_string();

    if channel.is_locked || channel.write_policy == "locked" {
        allow = false;
        reason = "频道已锁定".into();
    } else if channel.is_readonly || channel.write_policy == "readonly" {
        allow = false;
        reason = "频道只读".into();
    } else if (channel.requires_subscription || channel.write_policy == "subscriber_only")
        && !user.subscribed
    {
        allow = false;
        reason = "需要有效订阅".into();
    } else if channel.write_policy == "admin_only" && !admin_like {
        allow = false;
        reason = "仅管理员可发言".into();
    } else if channel.write_policy == "role_only" && !admin_like {
        if channel.allowed_role.is_empty() {
            allow = false;
            reason = "指定角色策略未配置允许角色".into();
        } else if channel.allowed_role != user.role {
            allow = false;
            reason = format!(
                "角色不匹配：用户 {}，需要 {}",
                user.role, channel.allowed_role
            );
        }
    } else if channel.write_policy == "min_level" && user.level < channel.min_level {
        allow = false;
        reason = format!(
            "等级不足：用户 lv{}，需要 lv{}+",
            user.level, channel.min_level
        );
    }

    if allow && channel.slowmode_seconds > 0 && !admin_like {
        let chat_id = Uuid::parse_str(&channel.id).ok();
        let recent = if let Some(chat_id) = chat_id {
            sqlx::query_scalar::<_, i64>(
                r#"SELECT COUNT(*) FROM messages
                   WHERE chat_id = $1 AND sender_id = $2 AND deleted_at IS NULL
                     AND created_at > now() - ($3::TEXT || ' seconds')::INTERVAL"#,
            )
            .bind(chat_id)
            .bind(user.id)
            .bind(channel.slowmode_seconds)
            .fetch_one(&state.db)
            .await
            .unwrap_or(0)
        } else {
            0
        };
        if recent > 0 {
            allow = false;
            reason = format!("慢速模式生效：{} 秒内已发过言", channel.slowmode_seconds);
        } else {
            reason = format!("可发言，慢速模式 {} 秒", channel.slowmode_seconds);
        }
    }

    channel.access_write_allowed = allow;
    channel.access_reason = reason;
}

#[derive(Deserialize)]
pub struct RenameForm {
    pub title: String,
}

#[derive(Deserialize)]
pub struct PolicyForm {
    pub write_policy: String,
    pub allowed_role: Option<String>,
    pub min_level: Option<i32>,
    pub slowmode_seconds: Option<i32>,
    pub requires_subscription: Option<String>,
    pub is_readonly: Option<String>,
    pub is_locked: Option<String>,
}

async fn rename_submit(
    State(s): State<Arc<AppState>>,
    headers: HeaderMap,
    Path(id): Path<Uuid>,
    Form(form): Form<RenameForm>,
) -> Response {
    let actor = match crate::admin_customization::require_actor(
        &headers,
        &s,
        "admin.channels.manage",
    )
    .await
    {
        Ok(v) => v,
        Err(resp) => return resp,
    };

    let title = form.title.trim();
    if title.is_empty() || title.len() > 64 {
        return Redirect::to("/admin/channels?err=title").into_response();
    }
    let _ = sqlx::query("UPDATE chats SET title=$1 WHERE id=$2 AND kind='channel'")
        .bind(title)
        .bind(id)
        .execute(&s.db)
        .await;
    let _ = sqlx::query(
        "INSERT INTO audit_log (actor, action, target, metadata) VALUES ($1,'admin.channel_rename',$2,$3)",
    )
    .bind(&actor.name)
    .bind(id.to_string())
    .bind(serde_json::json!({ "title": title }))
    .execute(&s.db)
    .await;
    Redirect::to("/admin/channels?ok=rename").into_response()
}

async fn clear_messages(
    State(s): State<Arc<AppState>>,
    headers: HeaderMap,
    Path(id): Path<Uuid>,
) -> Response {
    let actor = match crate::admin_customization::require_actor(
        &headers,
        &s,
        "admin.channels.manage",
    )
    .await
    {
        Ok(v) => v,
        Err(resp) => return resp,
    };

    let res =
        sqlx::query("UPDATE messages SET deleted_at=now() WHERE chat_id=$1 AND deleted_at IS NULL")
            .bind(id)
            .execute(&s.db)
            .await;
    let n = res.map(|r| r.rows_affected()).unwrap_or(0);
    let _ = sqlx::query(
        "INSERT INTO audit_log (actor, action, target, metadata) VALUES ($1,'admin.channel_clear',$2,$3)",
    )
    .bind(&actor.name)
    .bind(id.to_string())
    .bind(serde_json::json!({ "soft_deleted": n }))
    .execute(&s.db)
    .await;
    Redirect::to("/admin/channels?ok=clear").into_response()
}

async fn policy_submit(
    State(s): State<Arc<AppState>>,
    headers: HeaderMap,
    Path(id): Path<Uuid>,
    Form(form): Form<PolicyForm>,
) -> Response {
    let actor = match crate::admin_customization::require_actor(
        &headers,
        &s,
        "admin.channels.manage",
    )
    .await
    {
        Ok(v) => v,
        Err(resp) => return resp,
    };
    if !matches!(
        form.write_policy.as_str(),
        "everyone"
            | "admin_only"
            | "role_only"
            | "min_level"
            | "subscriber_only"
            | "readonly"
            | "locked"
    ) {
        return Redirect::to("/admin/channels?err=policy").into_response();
    }
    let allowed_role = form
        .allowed_role
        .as_deref()
        .map(str::trim)
        .filter(|s| !s.is_empty())
        .map(|s| s.chars().take(32).collect::<String>());
    let min_level = form.min_level.unwrap_or(1).clamp(1, 100);
    let slowmode_seconds = form.slowmode_seconds.unwrap_or(0).clamp(0, 86_400);
    let requires_subscription = form.requires_subscription.is_some();
    let is_readonly = form.is_readonly.is_some() || form.write_policy == "readonly";
    let is_locked = form.is_locked.is_some() || form.write_policy == "locked";
    let res = sqlx::query(
        r#"INSERT INTO channel_settings
              (chat_id, display_name, write_policy, allowed_role, min_level, slowmode_seconds,
               requires_subscription, is_readonly, is_locked, updated_at)
           VALUES (
              $1,
              (SELECT title FROM chats WHERE id = $1),
              $2, $3, $4, $5, $6, $7, $8, now()
           )
           ON CONFLICT (chat_id) DO UPDATE
             SET write_policy = EXCLUDED.write_policy,
                 allowed_role = EXCLUDED.allowed_role,
                 min_level = EXCLUDED.min_level,
                 slowmode_seconds = EXCLUDED.slowmode_seconds,
                 requires_subscription = EXCLUDED.requires_subscription,
                 is_readonly = EXCLUDED.is_readonly,
                 is_locked = EXCLUDED.is_locked,
                 updated_at = now()"#,
    )
    .bind(id)
    .bind(&form.write_policy)
    .bind(allowed_role)
    .bind(min_level)
    .bind(slowmode_seconds)
    .bind(requires_subscription)
    .bind(is_readonly)
    .bind(is_locked)
    .execute(&s.db)
    .await;
    if res.is_err() {
        return Redirect::to("/admin/channels?err=policy").into_response();
    }
    crate::admin_customization::write_audit(
        &s,
        &actor,
        "admin.channel_policy",
        &id.to_string(),
        serde_json::json!({
            "write_policy": form.write_policy,
            "requires_subscription": requires_subscription,
            "readonly": is_readonly,
            "locked": is_locked
        }),
    )
    .await;
    Redirect::to("/admin/channels?ok=policy").into_response()
}

pub fn routes() -> Router<Arc<AppState>> {
    Router::new()
        .route("/admin/channels", get(channels_page))
        .route("/admin/channels/:id/rename", post(rename_submit))
        .route("/admin/channels/:id/policy", post(policy_submit))
        .route("/admin/channels/:id/clear", post(clear_messages))
}
