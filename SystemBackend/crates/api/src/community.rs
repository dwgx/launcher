use crate::media::auth_user;
use crate::state::AppState;
use axum::{
    extract::{Json, Path, Query, State},
    http::StatusCode,
};
use chrono::{DateTime, Utc};
use serde::{Deserialize, Serialize};
use sqlx::{postgres::PgRow, Row};
use std::collections::HashMap;
use std::sync::Arc;
use uuid::Uuid;

use crate::error::internal;

fn clean_text(input: &str, max: usize) -> String {
    input.trim().chars().take(max).collect()
}

fn clean_slug(input: &str, max: usize) -> String {
    input
        .trim()
        .to_ascii_lowercase()
        .chars()
        .filter_map(|ch| {
            if ch.is_ascii_alphanumeric() {
                Some(ch)
            } else if matches!(ch, '-' | '_') {
                Some(ch)
            } else if ch.is_whitespace() {
                Some('_')
            } else {
                None
            }
        })
        .take(max)
        .collect()
}

async fn user_role(
    state: &AppState,
    user_id: Uuid,
) -> Result<(String, Option<String>, bool, bool), (StatusCode, String)> {
    let row = sqlx::query(
        r#"SELECT username, role, role_label, is_admin
           FROM users WHERE id = $1"#,
    )
    .bind(user_id)
    .fetch_optional(&state.db)
    .await
    .map_err(internal)?
    .ok_or((StatusCode::NOT_FOUND, "user not found".into()))?;
    let username: Option<String> = row.try_get("username").map_err(internal)?;
    let role: String = row.try_get("role").map_err(internal)?;
    let role_label: Option<String> = row.try_get("role_label").map_err(internal)?;
    let is_admin: bool = row.try_get("is_admin").map_err(internal)?;
    let admin = is_admin || matches!(role.as_str(), "admin" | "owner" | "super_admin");
    let super_admin = is_admin
        && (role == "owner" || role == "super_admin" || username.as_deref() == Some("admin"));
    Ok((role, role_label, admin, super_admin))
}

async fn user_level(state: &AppState, user_id: Uuid) -> Result<(i32, i64), (StatusCode, String)> {
    let row = sqlx::query(
        r#"INSERT INTO user_progress (user_id)
           VALUES ($1)
           ON CONFLICT (user_id) DO UPDATE SET user_id = EXCLUDED.user_id
           RETURNING level, xp"#,
    )
    .bind(user_id)
    .fetch_one(&state.db)
    .await
    .map_err(internal)?;
    Ok((
        row.try_get::<i32, _>("level").map_err(internal)?,
        row.try_get::<i64, _>("xp").map_err(internal)?,
    ))
}

fn level_from_xp(xp: i64) -> i32 {
    let mut level = (xp / 100 + 1) as i32;
    level = level.clamp(1, 100);
    level
}

async fn grant_xp(
    state: &AppState,
    user_id: Uuid,
    event_type: &str,
    amount: i32,
    source_type: Option<&str>,
    source_id: Option<String>,
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

    let xp_row = sqlx::query(
        r#"SELECT COALESCE(SUM(amount), 0)::BIGINT AS xp
           FROM user_xp_events WHERE user_id = $1"#,
    )
    .bind(user_id)
    .fetch_one(&state.db)
    .await
    .map_err(internal)?;
    let xp: i64 = xp_row.try_get("xp").map_err(internal)?;
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

async fn ensure_ticket_access(
    state: &AppState,
    user_id: Uuid,
    ticket_id: Uuid,
) -> Result<TicketAccess, (StatusCode, String)> {
    let (_, _, is_admin, is_super_admin) = user_role(state, user_id).await?;
    let row = sqlx::query(
        r#"SELECT id, chat_id, creator_id, visibility, protected_by_superadmin
           FROM tickets WHERE id = $1"#,
    )
    .bind(ticket_id)
    .fetch_optional(&state.db)
    .await
    .map_err(internal)?
    .ok_or((StatusCode::NOT_FOUND, "ticket not found".into()))?;
    let creator_id: Option<Uuid> = row.try_get("creator_id").map_err(internal)?;
    let visibility: String = row.try_get("visibility").map_err(internal)?;
    let protected: bool = row.try_get("protected_by_superadmin").map_err(internal)?;
    let allowed = creator_id == Some(user_id)
        || (visibility == "public" && !protected)
        || (is_admin && (!protected || is_super_admin));
    if !allowed {
        return Err((
            StatusCode::FORBIDDEN,
            "ticket is protected or private".into(),
        ));
    }
    Ok(TicketAccess {
        chat_id: row.try_get("chat_id").map_err(internal)?,
        is_admin,
        is_super_admin,
        protected,
    })
}

async fn ensure_admin(state: &AppState, user_id: Uuid) -> Result<bool, (StatusCode, String)> {
    let (_, _, is_admin, is_super_admin) = user_role(state, user_id).await?;
    if !is_admin {
        return Err((StatusCode::FORBIDDEN, "admin required".into()));
    }
    Ok(is_super_admin)
}

struct TicketAccess {
    chat_id: Uuid,
    is_admin: bool,
    is_super_admin: bool,
    protected: bool,
}

#[derive(Deserialize)]
pub struct SessionQ {
    pub session_token: String,
}

/// 分页查询参数：在 SessionQ 基础上加可选 limit/offset。
/// 不传参时退化为原有行为（limit = 该端点默认上限，offset = 0）。
#[derive(Deserialize)]
pub struct PagedQ {
    pub session_token: String,
    pub limit: Option<i64>,
    pub offset: Option<i64>,
}

impl PagedQ {
    /// 把 limit clamp 到 [1, max]（默认 max），offset clamp 到 >= 0。
    /// max 用各端点原本的固定 LIMIT，既保留安全上限，又支持翻页。
    fn bounds(&self, max: i64) -> (i64, i64) {
        let limit = self.limit.unwrap_or(max).clamp(1, max);
        let offset = self.offset.unwrap_or(0).max(0);
        (limit, offset)
    }
}


#[derive(Serialize)]
pub struct AreaOut {
    pub id: String,
    pub slug: String,
    pub name: String,
    pub description: String,
    pub sort_order: i32,
}

#[derive(Serialize)]
pub struct ChannelOut {
    pub id: String,
    pub slug: String,
    pub title: String,
    pub area_slug: Option<String>,
    pub area_name: Option<String>,
    pub write_policy: String,
    pub allowed_role: Option<String>,
    pub min_level: i32,
    pub slowmode_seconds: i32,
    pub requires_subscription: bool,
    pub is_readonly: bool,
    pub is_locked: bool,
    pub sort_order: i32,
}

#[derive(Serialize)]
pub struct TicketCategoryOut {
    pub id: String,
    pub slug: String,
    pub name: String,
    pub description: String,
    pub is_builtin: bool,
    pub is_enabled: bool,
    pub sort_order: i32,
}

#[derive(Serialize)]
pub struct AnnouncementOut {
    pub id: String,
    pub title: String,
    pub body: String,
    pub severity: String,
    pub force_popup: bool,
    pub red_dot: bool,
    pub unread: bool,
    pub read_at: Option<i64>,
    pub acknowledged_at: Option<i64>,
    pub starts_at: i64,
    pub expires_at: Option<i64>,
}

#[derive(Serialize)]
pub struct MeOut {
    pub role: String,
    pub role_label: Option<String>,
    pub is_admin: bool,
    pub is_super_admin: bool,
    pub level: i32,
    pub xp: i64,
}

#[derive(Serialize)]
pub struct BootstrapOut {
    pub me: MeOut,
    pub areas: Vec<AreaOut>,
    pub channels: Vec<ChannelOut>,
    pub ticket_categories: Vec<TicketCategoryOut>,
    pub announcements: Vec<AnnouncementOut>,
    pub config: HashMap<String, serde_json::Value>,
}

pub async fn bootstrap(
    State(s): State<Arc<AppState>>,
    Query(q): Query<SessionQ>,
) -> Result<Json<BootstrapOut>, (StatusCode, String)> {
    let me = auth_user(&s, &q.session_token).await?;
    let (role, role_label, is_admin, is_super_admin) = user_role(&s, me).await?;
    let (level, xp) = user_level(&s, me).await?;

    let areas = sqlx::query(
        r#"SELECT id, slug, name, description, sort_order
           FROM community_areas WHERE is_enabled = TRUE
           ORDER BY sort_order, name"#,
    )
    .fetch_all(&s.db)
    .await
    .map_err(internal)?
    .into_iter()
    .map(|r| {
        Ok(AreaOut {
            id: r.try_get::<Uuid, _>("id").map_err(internal)?.to_string(),
            slug: r.try_get("slug").map_err(internal)?,
            name: r.try_get("name").map_err(internal)?,
            description: r.try_get("description").map_err(internal)?,
            sort_order: r.try_get("sort_order").map_err(internal)?,
        })
    })
    .collect::<Result<Vec<_>, (StatusCode, String)>>()?;

    let channels = sqlx::query(
        r#"SELECT c.id, COALESCE(c.slug, '') AS slug, COALESCE(c.title, '') AS title,
                  a.slug AS area_slug, a.name AS area_name,
                  cs.write_policy, cs.allowed_role, cs.min_level, cs.slowmode_seconds,
                  cs.requires_subscription, cs.is_readonly, cs.is_locked, cs.sort_order
           FROM chats c
           JOIN channel_settings cs ON cs.chat_id = c.id
           LEFT JOIN community_areas a ON a.id = cs.area_id
           WHERE c.kind = 'channel' AND cs.is_enabled = TRUE
           ORDER BY COALESCE(a.sort_order, 999), cs.sort_order, c.title"#,
    )
    .fetch_all(&s.db)
    .await
    .map_err(internal)?
    .into_iter()
    .map(channel_from_row)
    .collect::<Result<Vec<_>, (StatusCode, String)>>()?;

    let ticket_categories = sqlx::query(
        r#"SELECT id, slug, name, description, is_builtin, is_enabled, sort_order
           FROM ticket_categories WHERE is_enabled = TRUE
           ORDER BY sort_order, name"#,
    )
    .fetch_all(&s.db)
    .await
    .map_err(internal)?
    .into_iter()
    .map(category_from_row)
    .collect::<Result<Vec<_>, (StatusCode, String)>>()?;

    let announcements = active_announcements(&s, me, Some(&role), level).await?;
    let config = client_config(&s).await?;

    Ok(Json(BootstrapOut {
        me: MeOut {
            role,
            role_label,
            is_admin,
            is_super_admin,
            level,
            xp,
        },
        areas,
        channels,
        ticket_categories,
        announcements,
        config,
    }))
}

async fn client_config(
    state: &AppState,
) -> Result<HashMap<String, serde_json::Value>, (StatusCode, String)> {
    let rows = sqlx::query(
        r#"SELECT key, value
           FROM app_config_entries
           WHERE enabled = TRUE AND expose_to_client = TRUE
           ORDER BY key"#,
    )
    .fetch_all(&state.db)
    .await
    .map_err(internal)?;
    let mut out = HashMap::with_capacity(rows.len());
    for r in rows {
        out.insert(
            r.try_get("key").map_err(internal)?,
            r.try_get("value").map_err(internal)?,
        );
    }
    Ok(out)
}

fn channel_from_row(r: PgRow) -> Result<ChannelOut, (StatusCode, String)> {
    Ok(ChannelOut {
        id: r.try_get::<Uuid, _>("id").map_err(internal)?.to_string(),
        slug: r.try_get("slug").map_err(internal)?,
        title: r.try_get("title").map_err(internal)?,
        area_slug: r.try_get("area_slug").map_err(internal)?,
        area_name: r.try_get("area_name").map_err(internal)?,
        write_policy: r.try_get("write_policy").map_err(internal)?,
        allowed_role: r.try_get("allowed_role").map_err(internal)?,
        min_level: r.try_get("min_level").map_err(internal)?,
        slowmode_seconds: r.try_get("slowmode_seconds").map_err(internal)?,
        requires_subscription: r.try_get("requires_subscription").map_err(internal)?,
        is_readonly: r.try_get("is_readonly").map_err(internal)?,
        is_locked: r.try_get("is_locked").map_err(internal)?,
        sort_order: r.try_get("sort_order").map_err(internal)?,
    })
}

fn category_from_row(r: PgRow) -> Result<TicketCategoryOut, (StatusCode, String)> {
    Ok(TicketCategoryOut {
        id: r.try_get::<Uuid, _>("id").map_err(internal)?.to_string(),
        slug: r.try_get("slug").map_err(internal)?,
        name: r.try_get("name").map_err(internal)?,
        description: r.try_get("description").map_err(internal)?,
        is_builtin: r.try_get("is_builtin").map_err(internal)?,
        is_enabled: r.try_get("is_enabled").map_err(internal)?,
        sort_order: r.try_get("sort_order").map_err(internal)?,
    })
}

async fn active_announcements(
    state: &AppState,
    user_id: Uuid,
    role: Option<&str>,
    level: i32,
) -> Result<Vec<AnnouncementOut>, (StatusCode, String)> {
    let rows = sqlx::query(
        r#"SELECT a.id, a.title, a.body, a.severity, a.force_popup, a.red_dot,
                  ar.read_at, ar.acknowledged_at,
                  a.starts_at, a.expires_at
           FROM announcements a
           LEFT JOIN announcement_reads ar
             ON ar.announcement_id = a.id AND ar.user_id = $3
           WHERE a.starts_at <= now()
             AND (a.expires_at IS NULL OR a.expires_at > now())
             AND a.min_level <= $1
             AND (a.audience_role IS NULL OR a.audience_role = $2)
           ORDER BY CASE severity WHEN 'critical' THEN 0 WHEN 'important' THEN 1 ELSE 2 END,
                    starts_at DESC
           LIMIT 20"#,
    )
    .bind(level)
    .bind(role)
    .bind(user_id)
    .fetch_all(&state.db)
    .await
    .map_err(internal)?;
    rows.into_iter()
        .map(|r| {
            let read_at = r
                .try_get::<Option<DateTime<Utc>>, _>("read_at")
                .map_err(internal)?
                .map(|t| t.timestamp());
            let acknowledged_at = r
                .try_get::<Option<DateTime<Utc>>, _>("acknowledged_at")
                .map_err(internal)?
                .map(|t| t.timestamp());
            Ok(AnnouncementOut {
                id: r.try_get::<Uuid, _>("id").map_err(internal)?.to_string(),
                title: r.try_get("title").map_err(internal)?,
                body: r.try_get("body").map_err(internal)?,
                severity: r.try_get("severity").map_err(internal)?,
                force_popup: r.try_get("force_popup").map_err(internal)?,
                red_dot: r.try_get("red_dot").map_err(internal)?,
                unread: read_at.is_none(),
                read_at,
                acknowledged_at,
                starts_at: r
                    .try_get::<DateTime<Utc>, _>("starts_at")
                    .map_err(internal)?
                    .timestamp(),
                expires_at: r
                    .try_get::<Option<DateTime<Utc>>, _>("expires_at")
                    .map_err(internal)?
                    .map(|t| t.timestamp()),
            })
        })
        .collect()
}

#[derive(Deserialize)]
pub struct CreateTopicReq {
    pub session_token: String,
    pub area_id: Option<Uuid>,
    pub title: String,
    pub body: String,
}

#[derive(Serialize)]
pub struct TopicOut {
    pub id: String,
    pub chat_id: String,
    pub area_id: Option<String>,
    pub author_id: Option<String>,
    pub title: String,
    pub status: String,
    pub pinned: bool,
    pub locked: bool,
    pub view_count: i64,
    pub last_reply_at: Option<i64>,
    pub created_at: i64,
}

pub async fn create_topic(
    State(s): State<Arc<AppState>>,
    Json(req): Json<CreateTopicReq>,
) -> Result<Json<TopicOut>, (StatusCode, String)> {
    let me = auth_user(&s, &req.session_token).await?;
    let title = clean_text(&req.title, 96);
    let body = clean_text(&req.body, 4000);
    if title.is_empty() || body.is_empty() {
        return Err((StatusCode::BAD_REQUEST, "title and body required".into()));
    }

    let mut tx = s.db.begin().await.map_err(internal)?;
    let chat = sqlx::query(
        r#"INSERT INTO chats (kind, title, created_by, is_public, last_message_at)
           VALUES ('group', $1, $2, TRUE, now())
           RETURNING id"#,
    )
    .bind(&title)
    .bind(me)
    .fetch_one(&mut *tx)
    .await
    .map_err(internal)?;
    let chat_id: Uuid = chat.try_get("id").map_err(internal)?;
    sqlx::query(
        r#"INSERT INTO chat_members (chat_id, user_id, role)
           VALUES ($1, $2, 'owner')
           ON CONFLICT DO NOTHING"#,
    )
    .bind(chat_id)
    .bind(me)
    .execute(&mut *tx)
    .await
    .map_err(internal)?;
    let topic = sqlx::query(
        r#"INSERT INTO forum_topics (chat_id, area_id, author_id, title, last_reply_at)
           VALUES ($1, $2, $3, $4, now())
           RETURNING id, chat_id, area_id, author_id, title, status, pinned, locked,
                     view_count, last_reply_at, created_at"#,
    )
    .bind(chat_id)
    .bind(req.area_id)
    .bind(me)
    .bind(&title)
    .fetch_one(&mut *tx)
    .await
    .map_err(internal)?;
    let msg = sqlx::query(
        r#"INSERT INTO messages (chat_id, sender_id, msg_type, payload)
           VALUES ($1, $2, 'text', $3)
           RETURNING id"#,
    )
    .bind(chat_id)
    .bind(me)
    .bind(serde_json::json!({ "text": body }))
    .fetch_one(&mut *tx)
    .await
    .map_err(internal)?;
    let message_id: i64 = msg.try_get("id").map_err(internal)?;
    sqlx::query(
        r#"INSERT INTO chat_events (chat_id, event_type, message_id, actor_id, payload)
           VALUES ($1, 'message', $2, $3, $4)"#,
    )
    .bind(chat_id)
    .bind(message_id)
    .bind(me)
    .bind(serde_json::json!({ "message_id": message_id }))
    .execute(&mut *tx)
    .await
    .map_err(internal)?;
    tx.commit().await.map_err(internal)?;
    grant_xp(
        &s,
        me,
        "forum_topic",
        10,
        Some("topic"),
        Some(
            topic
                .try_get::<Uuid, _>("id")
                .map_err(internal)?
                .to_string(),
        ),
    )
    .await?;
    Ok(Json(topic_from_row(topic)?))
}

pub async fn list_topics(
    State(s): State<Arc<AppState>>,
    Query(q): Query<PagedQ>,
) -> Result<Json<Vec<TopicOut>>, (StatusCode, String)> {
    let _me = auth_user(&s, &q.session_token).await?;
    let (limit, offset) = q.bounds(100);
    let rows = sqlx::query(
        r#"SELECT id, chat_id, area_id, author_id, title, status, pinned, locked,
                  view_count, last_reply_at, created_at
           FROM forum_topics
           WHERE status <> 'hidden'
           ORDER BY pinned DESC, COALESCE(last_reply_at, created_at) DESC
           LIMIT $1 OFFSET $2"#,
    )
    .bind(limit)
    .bind(offset)
    .fetch_all(&s.db)
    .await
    .map_err(internal)?;
    rows.into_iter()
        .map(topic_from_row)
        .collect::<Result<Vec<_>, (StatusCode, String)>>()
        .map(Json)
}

fn topic_from_row(r: PgRow) -> Result<TopicOut, (StatusCode, String)> {
    Ok(TopicOut {
        id: r.try_get::<Uuid, _>("id").map_err(internal)?.to_string(),
        chat_id: r
            .try_get::<Uuid, _>("chat_id")
            .map_err(internal)?
            .to_string(),
        area_id: r
            .try_get::<Option<Uuid>, _>("area_id")
            .map_err(internal)?
            .map(|v| v.to_string()),
        author_id: r
            .try_get::<Option<Uuid>, _>("author_id")
            .map_err(internal)?
            .map(|v| v.to_string()),
        title: r.try_get("title").map_err(internal)?,
        status: r.try_get("status").map_err(internal)?,
        pinned: r.try_get("pinned").map_err(internal)?,
        locked: r.try_get("locked").map_err(internal)?,
        view_count: r.try_get("view_count").map_err(internal)?,
        last_reply_at: r
            .try_get::<Option<DateTime<Utc>>, _>("last_reply_at")
            .map_err(internal)?
            .map(|t| t.timestamp()),
        created_at: r
            .try_get::<DateTime<Utc>, _>("created_at")
            .map_err(internal)?
            .timestamp(),
    })
}

#[derive(Deserialize)]
pub struct UpsertTicketCategoryReq {
    pub session_token: String,
    pub slug: Option<String>,
    pub name: String,
    pub description: Option<String>,
    pub is_enabled: Option<bool>,
    pub sort_order: Option<i32>,
}

pub async fn list_ticket_categories(
    State(s): State<Arc<AppState>>,
    Query(q): Query<SessionQ>,
) -> Result<Json<Vec<TicketCategoryOut>>, (StatusCode, String)> {
    let me = auth_user(&s, &q.session_token).await?;
    let (_, _, is_admin, _) = user_role(&s, me).await?;
    let where_sql = if is_admin {
        ""
    } else {
        "WHERE is_enabled = TRUE"
    };
    let sql = format!(
        r#"SELECT id, slug, name, description, is_builtin, is_enabled, sort_order
           FROM ticket_categories
           {where_sql}
           ORDER BY sort_order, name"#
    );
    let rows = sqlx::query(&sql).fetch_all(&s.db).await.map_err(internal)?;
    rows.into_iter()
        .map(category_from_row)
        .collect::<Result<Vec<_>, (StatusCode, String)>>()
        .map(Json)
}

pub async fn create_ticket_category(
    State(s): State<Arc<AppState>>,
    Json(req): Json<UpsertTicketCategoryReq>,
) -> Result<Json<TicketCategoryOut>, (StatusCode, String)> {
    let me = auth_user(&s, &req.session_token).await?;
    let _ = ensure_admin(&s, me).await?;
    let name = clean_text(&req.name, 64);
    if name.is_empty() {
        return Err((StatusCode::BAD_REQUEST, "name required".into()));
    }
    let slug = req
        .slug
        .as_deref()
        .map(|v| clean_slug(v, 48))
        .filter(|v| !v.is_empty())
        .unwrap_or_else(|| clean_slug(&name, 48));
    if slug.is_empty() {
        return Err((StatusCode::BAD_REQUEST, "slug required".into()));
    }
    let description = req
        .description
        .as_deref()
        .map(|s| clean_text(s, 240))
        .unwrap_or_default();
    let row = sqlx::query(
        r#"INSERT INTO ticket_categories
              (slug, name, description, is_builtin, is_enabled, sort_order, updated_at)
           VALUES ($1, $2, $3, FALSE, $4, $5, now())
           RETURNING id, slug, name, description, is_builtin, is_enabled, sort_order"#,
    )
    .bind(slug)
    .bind(name)
    .bind(description)
    .bind(req.is_enabled.unwrap_or(true))
    .bind(req.sort_order.unwrap_or(100).clamp(0, 10_000))
    .fetch_one(&s.db)
    .await
    .map_err(internal)?;
    Ok(Json(category_from_row(row)?))
}

pub async fn update_ticket_category(
    State(s): State<Arc<AppState>>,
    Path(category_id): Path<Uuid>,
    Json(req): Json<UpsertTicketCategoryReq>,
) -> Result<Json<TicketCategoryOut>, (StatusCode, String)> {
    let me = auth_user(&s, &req.session_token).await?;
    let _ = ensure_admin(&s, me).await?;
    let name = clean_text(&req.name, 64);
    if name.is_empty() {
        return Err((StatusCode::BAD_REQUEST, "name required".into()));
    }
    let description = req
        .description
        .as_deref()
        .map(|s| clean_text(s, 240))
        .unwrap_or_default();
    let row = sqlx::query(
        r#"UPDATE ticket_categories
           SET name = $2,
               description = $3,
               is_enabled = $4,
               sort_order = $5,
               updated_at = now()
           WHERE id = $1
           RETURNING id, slug, name, description, is_builtin, is_enabled, sort_order"#,
    )
    .bind(category_id)
    .bind(name)
    .bind(description)
    .bind(req.is_enabled.unwrap_or(true))
    .bind(req.sort_order.unwrap_or(100).clamp(0, 10_000))
    .fetch_optional(&s.db)
    .await
    .map_err(internal)?
    .ok_or((StatusCode::NOT_FOUND, "category not found".into()))?;
    Ok(Json(category_from_row(row)?))
}

#[derive(Deserialize)]
pub struct CreateTicketReq {
    pub session_token: String,
    pub category_id: Uuid,
    pub title: String,
    pub body: String,
    pub visibility: Option<String>,
}

#[derive(Serialize)]
pub struct TicketOut {
    pub id: String,
    pub chat_id: String,
    pub number: i64,
    pub creator_id: Option<String>,
    pub category_id: Option<String>,
    pub category_name: Option<String>,
    pub title: String,
    pub status: String,
    pub visibility: String,
    pub protected_by_superadmin: bool,
    pub assigned_admin_id: Option<String>,
    pub resolved_by: Option<String>,
    pub resolved_at: Option<i64>,
    pub created_at: i64,
    pub updated_at: i64,
}

#[derive(Serialize)]
pub struct TicketMessageOut {
    pub id: i64,
    pub sender_id: Option<String>,
    pub sender_name: String,
    pub msg_type: String,
    pub payload: serde_json::Value,
    pub edited_at: Option<i64>,
    pub deleted: bool,
    pub created_at: i64,
    pub edits: Vec<MessageAdminEditOut>,
}

#[derive(Serialize)]
pub struct MessageAdminEditOut {
    pub id: i64,
    pub editor_id: Option<String>,
    pub editor_name: String,
    pub old_payload: serde_json::Value,
    pub new_payload: serde_json::Value,
    pub reason: String,
    pub edit_kind: String,
    pub created_at: i64,
}

#[derive(Serialize)]
pub struct TicketDetailOut {
    pub ticket: TicketOut,
    pub messages: Vec<TicketMessageOut>,
}

pub async fn create_ticket(
    State(s): State<Arc<AppState>>,
    Json(req): Json<CreateTicketReq>,
) -> Result<Json<TicketOut>, (StatusCode, String)> {
    let me = auth_user(&s, &req.session_token).await?;
    let title = clean_text(&req.title, 96);
    let body = clean_text(&req.body, 4000);
    if title.is_empty() || body.is_empty() {
        return Err((StatusCode::BAD_REQUEST, "title and body required".into()));
    }
    let visibility = match req.visibility.as_deref().unwrap_or("private") {
        "public" => "public",
        _ => "private",
    };
    let category_enabled =
        sqlx::query_scalar::<_, bool>("SELECT is_enabled FROM ticket_categories WHERE id = $1")
            .bind(req.category_id)
            .fetch_optional(&s.db)
            .await
            .map_err(internal)?
            .ok_or((StatusCode::BAD_REQUEST, "category not found".into()))?;
    if !category_enabled {
        return Err((StatusCode::BAD_REQUEST, "category disabled".into()));
    }

    let mut tx = s.db.begin().await.map_err(internal)?;
    let chat = sqlx::query(
        r#"INSERT INTO chats (kind, title, created_by, is_public, last_message_at)
           VALUES ('group', $1, $2, $3, now())
           RETURNING id"#,
    )
    .bind(&title)
    .bind(me)
    .bind(visibility == "public")
    .fetch_one(&mut *tx)
    .await
    .map_err(internal)?;
    let chat_id: Uuid = chat.try_get("id").map_err(internal)?;
    sqlx::query(
        r#"INSERT INTO chat_members (chat_id, user_id, role)
           VALUES ($1, $2, 'owner')
           ON CONFLICT DO NOTHING"#,
    )
    .bind(chat_id)
    .bind(me)
    .execute(&mut *tx)
    .await
    .map_err(internal)?;
    let ticket = sqlx::query(
        r#"INSERT INTO tickets (chat_id, creator_id, category_id, title, visibility)
           VALUES ($1, $2, $3, $4, $5)
           RETURNING id, chat_id, number, creator_id, category_id, title, status, visibility,
                     protected_by_superadmin, assigned_admin_id, resolved_by, resolved_at,
                     created_at, updated_at"#,
    )
    .bind(chat_id)
    .bind(me)
    .bind(req.category_id)
    .bind(&title)
    .bind(visibility)
    .fetch_one(&mut *tx)
    .await
    .map_err(internal)?;
    let msg = sqlx::query(
        r#"INSERT INTO messages (chat_id, sender_id, msg_type, payload)
           VALUES ($1, $2, 'text', $3)
           RETURNING id"#,
    )
    .bind(chat_id)
    .bind(me)
    .bind(serde_json::json!({ "text": body }))
    .fetch_one(&mut *tx)
    .await
    .map_err(internal)?;
    let message_id: i64 = msg.try_get("id").map_err(internal)?;
    sqlx::query(
        r#"INSERT INTO chat_events (chat_id, event_type, message_id, actor_id, payload)
           VALUES ($1, 'message', $2, $3, $4)"#,
    )
    .bind(chat_id)
    .bind(message_id)
    .bind(me)
    .bind(serde_json::json!({ "message_id": message_id }))
    .execute(&mut *tx)
    .await
    .map_err(internal)?;
    tx.commit().await.map_err(internal)?;
    grant_xp(
        &s,
        me,
        "ticket_created",
        5,
        Some("ticket"),
        Some(
            ticket
                .try_get::<Uuid, _>("id")
                .map_err(internal)?
                .to_string(),
        ),
    )
    .await?;
    Ok(Json(ticket_from_row(&s, ticket).await?))
}

pub async fn list_my_tickets(
    State(s): State<Arc<AppState>>,
    Query(q): Query<PagedQ>,
) -> Result<Json<Vec<TicketOut>>, (StatusCode, String)> {
    let me = auth_user(&s, &q.session_token).await?;
    let (limit, offset) = q.bounds(100);
    list_tickets_with_where(&s, "WHERE t.creator_id = $1", me, false, limit, offset)
        .await
        .map(Json)
}

pub async fn list_public_tickets(
    State(s): State<Arc<AppState>>,
    Query(q): Query<PagedQ>,
) -> Result<Json<Vec<TicketOut>>, (StatusCode, String)> {
    let _me = auth_user(&s, &q.session_token).await?;
    let (limit, offset) = q.bounds(100);
    let rows = sqlx::query(
        r#"SELECT t.id, t.chat_id, t.number, t.creator_id, t.category_id, tc.name AS category_name,
                  t.title, t.status, t.visibility, t.protected_by_superadmin,
                  t.assigned_admin_id, t.resolved_by, t.resolved_at, t.created_at, t.updated_at
           FROM tickets t
           LEFT JOIN ticket_categories tc ON tc.id = t.category_id
           WHERE t.visibility = 'public' AND t.protected_by_superadmin = FALSE
           ORDER BY t.updated_at DESC LIMIT $1 OFFSET $2"#,
    )
    .bind(limit)
    .bind(offset)
    .fetch_all(&s.db)
    .await
    .map_err(internal)?;
    rows.into_iter()
        .map(ticket_from_join_row)
        .collect::<Result<Vec<_>, (StatusCode, String)>>()
        .map(Json)
}

pub async fn list_admin_tickets(
    State(s): State<Arc<AppState>>,
    Query(q): Query<PagedQ>,
) -> Result<Json<Vec<TicketOut>>, (StatusCode, String)> {
    let me = auth_user(&s, &q.session_token).await?;
    let is_super_admin = ensure_admin(&s, me).await?;
    let (limit, offset) = q.bounds(200);
    let sql = if is_super_admin {
        r#"SELECT t.id, t.chat_id, t.number, t.creator_id, t.category_id, tc.name AS category_name,
                  t.title, t.status, t.visibility, t.protected_by_superadmin,
                  t.assigned_admin_id, t.resolved_by, t.resolved_at, t.created_at, t.updated_at
           FROM tickets t
           LEFT JOIN ticket_categories tc ON tc.id = t.category_id
           ORDER BY t.updated_at DESC LIMIT $1 OFFSET $2"#
    } else {
        r#"SELECT t.id, t.chat_id, t.number, t.creator_id, t.category_id, tc.name AS category_name,
                  t.title, t.status, t.visibility, t.protected_by_superadmin,
                  t.assigned_admin_id, t.resolved_by, t.resolved_at, t.created_at, t.updated_at
           FROM tickets t
           LEFT JOIN ticket_categories tc ON tc.id = t.category_id
           WHERE t.protected_by_superadmin = FALSE
           ORDER BY t.updated_at DESC LIMIT $1 OFFSET $2"#
    };
    let rows = sqlx::query(sql)
        .bind(limit)
        .bind(offset)
        .fetch_all(&s.db)
        .await
        .map_err(internal)?;
    rows.into_iter()
        .map(ticket_from_join_row)
        .collect::<Result<Vec<_>, (StatusCode, String)>>()
        .map(Json)
}

pub async fn get_ticket_detail(
    State(s): State<Arc<AppState>>,
    Path(ticket_id): Path<Uuid>,
    Query(q): Query<SessionQ>,
) -> Result<Json<TicketDetailOut>, (StatusCode, String)> {
    let me = auth_user(&s, &q.session_token).await?;
    let access = ensure_ticket_access(&s, me, ticket_id).await?;
    let ticket_row = sqlx::query(
        r#"SELECT t.id, t.chat_id, t.number, t.creator_id, t.category_id, tc.name AS category_name,
                  t.title, t.status, t.visibility, t.protected_by_superadmin,
                  t.assigned_admin_id, t.resolved_by, t.resolved_at, t.created_at, t.updated_at
           FROM tickets t
           LEFT JOIN ticket_categories tc ON tc.id = t.category_id
           WHERE t.id = $1"#,
    )
    .bind(ticket_id)
    .fetch_optional(&s.db)
    .await
    .map_err(internal)?
    .ok_or((StatusCode::NOT_FOUND, "ticket not found".into()))?;
    let ticket = ticket_from_join_row(ticket_row)?;

    let rows = sqlx::query(
        r#"SELECT m.id, m.sender_id,
                  COALESCE(u.nickname, u.username, u.uid, 'system') AS sender_name,
                  m.msg_type, m.payload, m.edited_at, m.deleted_at, m.created_at
           FROM messages m
           LEFT JOIN users u ON u.id = m.sender_id
           WHERE m.chat_id = $1
           ORDER BY m.id ASC
           LIMIT 500"#,
    )
    .bind(access.chat_id)
    .fetch_all(&s.db)
    .await
    .map_err(internal)?;

    let mut messages = Vec::with_capacity(rows.len());
    for r in rows {
        let message_id: i64 = r.try_get("id").map_err(internal)?;
        let edit_rows = if access.is_admin {
            sqlx::query(
                r#"SELECT e.id, e.editor_id,
                          COALESCE(u.nickname, u.username, u.uid, 'admin') AS editor_name,
                          e.old_payload, e.new_payload, e.reason, e.edit_kind, e.created_at
                   FROM message_admin_edits e
                   LEFT JOIN users u ON u.id = e.editor_id
                   WHERE e.message_id = $1
                   ORDER BY e.created_at DESC"#,
            )
            .bind(message_id)
            .fetch_all(&s.db)
            .await
            .map_err(internal)?
        } else {
            Vec::new()
        };
        let edits = edit_rows
            .into_iter()
            .map(|e| {
                Ok(MessageAdminEditOut {
                    id: e.try_get("id").map_err(internal)?,
                    editor_id: e
                        .try_get::<Option<Uuid>, _>("editor_id")
                        .map_err(internal)?
                        .map(|v| v.to_string()),
                    editor_name: e.try_get("editor_name").map_err(internal)?,
                    old_payload: e.try_get("old_payload").map_err(internal)?,
                    new_payload: e.try_get("new_payload").map_err(internal)?,
                    reason: e.try_get("reason").map_err(internal)?,
                    edit_kind: e.try_get("edit_kind").map_err(internal)?,
                    created_at: e
                        .try_get::<DateTime<Utc>, _>("created_at")
                        .map_err(internal)?
                        .timestamp(),
                })
            })
            .collect::<Result<Vec<_>, (StatusCode, String)>>()?;
        messages.push(TicketMessageOut {
            id: message_id,
            sender_id: r
                .try_get::<Option<Uuid>, _>("sender_id")
                .map_err(internal)?
                .map(|v| v.to_string()),
            sender_name: r.try_get("sender_name").map_err(internal)?,
            msg_type: r.try_get("msg_type").map_err(internal)?,
            payload: r.try_get("payload").map_err(internal)?,
            edited_at: r
                .try_get::<Option<DateTime<Utc>>, _>("edited_at")
                .map_err(internal)?
                .map(|t| t.timestamp()),
            deleted: r
                .try_get::<Option<DateTime<Utc>>, _>("deleted_at")
                .map_err(internal)?
                .is_some(),
            created_at: r
                .try_get::<DateTime<Utc>, _>("created_at")
                .map_err(internal)?
                .timestamp(),
            edits,
        });
    }

    Ok(Json(TicketDetailOut { ticket, messages }))
}

async fn list_tickets_with_where(
    state: &AppState,
    where_sql: &str,
    user_id: Uuid,
    include_category: bool,
    limit: i64,
    offset: i64,
) -> Result<Vec<TicketOut>, (StatusCode, String)> {
    let category_select = if include_category {
        "tc.name"
    } else {
        "NULL::TEXT"
    };
    let sql = format!(
        r#"SELECT t.id, t.chat_id, t.number, t.creator_id, t.category_id, {category_select} AS category_name,
                  t.title, t.status, t.visibility, t.protected_by_superadmin,
                  t.assigned_admin_id, t.resolved_by, t.resolved_at, t.created_at, t.updated_at
           FROM tickets t
           LEFT JOIN ticket_categories tc ON tc.id = t.category_id
           {where_sql}
           ORDER BY t.updated_at DESC LIMIT $2 OFFSET $3"#
    );
    let rows = sqlx::query(&sql)
        .bind(user_id)
        .bind(limit)
        .bind(offset)
        .fetch_all(&state.db)
        .await
        .map_err(internal)?;
    rows.into_iter()
        .map(ticket_from_join_row)
        .collect::<Result<Vec<_>, (StatusCode, String)>>()
}

async fn ticket_from_row(state: &AppState, r: PgRow) -> Result<TicketOut, (StatusCode, String)> {
    let category_id: Option<Uuid> = r.try_get("category_id").map_err(internal)?;
    let category_name = if let Some(cid) = category_id {
        sqlx::query_scalar::<_, String>("SELECT name FROM ticket_categories WHERE id = $1")
            .bind(cid)
            .fetch_optional(&state.db)
            .await
            .map_err(internal)?
    } else {
        None
    };
    ticket_from_values(r, category_name)
}

fn ticket_from_join_row(r: PgRow) -> Result<TicketOut, (StatusCode, String)> {
    let category_name: Option<String> = r.try_get("category_name").map_err(internal)?;
    ticket_from_values(r, category_name)
}

fn ticket_from_values(
    r: PgRow,
    category_name: Option<String>,
) -> Result<TicketOut, (StatusCode, String)> {
    Ok(TicketOut {
        id: r.try_get::<Uuid, _>("id").map_err(internal)?.to_string(),
        chat_id: r
            .try_get::<Uuid, _>("chat_id")
            .map_err(internal)?
            .to_string(),
        number: r.try_get("number").map_err(internal)?,
        creator_id: r
            .try_get::<Option<Uuid>, _>("creator_id")
            .map_err(internal)?
            .map(|v| v.to_string()),
        category_id: r
            .try_get::<Option<Uuid>, _>("category_id")
            .map_err(internal)?
            .map(|v| v.to_string()),
        category_name,
        title: r.try_get("title").map_err(internal)?,
        status: r.try_get("status").map_err(internal)?,
        visibility: r.try_get("visibility").map_err(internal)?,
        protected_by_superadmin: r.try_get("protected_by_superadmin").map_err(internal)?,
        assigned_admin_id: r
            .try_get::<Option<Uuid>, _>("assigned_admin_id")
            .map_err(internal)?
            .map(|v| v.to_string()),
        resolved_by: r
            .try_get::<Option<Uuid>, _>("resolved_by")
            .map_err(internal)?
            .map(|v| v.to_string()),
        resolved_at: r
            .try_get::<Option<DateTime<Utc>>, _>("resolved_at")
            .map_err(internal)?
            .map(|t| t.timestamp()),
        created_at: r
            .try_get::<DateTime<Utc>, _>("created_at")
            .map_err(internal)?
            .timestamp(),
        updated_at: r
            .try_get::<DateTime<Utc>, _>("updated_at")
            .map_err(internal)?
            .timestamp(),
    })
}

#[derive(Deserialize)]
pub struct TicketStatusReq {
    pub session_token: String,
    pub status: String,
}

pub async fn update_ticket_status(
    State(s): State<Arc<AppState>>,
    Path(ticket_id): Path<Uuid>,
    Json(req): Json<TicketStatusReq>,
) -> Result<Json<TicketOut>, (StatusCode, String)> {
    let me = auth_user(&s, &req.session_token).await?;
    let access = ensure_ticket_access(&s, me, ticket_id).await?;
    if !access.is_admin {
        return Err((StatusCode::FORBIDDEN, "admin required".into()));
    }
    let status = match req.status.as_str() {
        "open" | "pending" | "resolved" | "closed" => req.status,
        _ => return Err((StatusCode::BAD_REQUEST, "bad status".into())),
    };
    let previous_status =
        sqlx::query_scalar::<_, String>("SELECT status FROM tickets WHERE id = $1")
            .bind(ticket_id)
            .fetch_optional(&s.db)
            .await
            .map_err(internal)?
            .ok_or((StatusCode::NOT_FOUND, "ticket not found".into()))?;
    let row = sqlx::query(
        r#"UPDATE tickets
           SET status = $2,
               resolved_by = CASE WHEN $2 = 'resolved' THEN $3 ELSE resolved_by END,
               resolved_at = CASE WHEN $2 = 'resolved' THEN now() ELSE resolved_at END,
               updated_at = now()
           WHERE id = $1
           RETURNING id, chat_id, number, creator_id, category_id, title, status, visibility,
                     protected_by_superadmin, assigned_admin_id, resolved_by, resolved_at,
                     created_at, updated_at"#,
    )
    .bind(ticket_id)
    .bind(&status)
    .bind(me)
    .fetch_one(&s.db)
    .await
    .map_err(internal)?;
    if status == "resolved" && previous_status != "resolved" {
        grant_xp(
            &s,
            me,
            "ticket_resolver",
            20,
            Some("ticket"),
            Some(ticket_id.to_string()),
        )
        .await?;
        if let Some(creator_id) = row
            .try_get::<Option<Uuid>, _>("creator_id")
            .map_err(internal)?
        {
            if creator_id != me {
                grant_xp(
                    &s,
                    creator_id,
                    "ticket_resolved",
                    20,
                    Some("ticket"),
                    Some(ticket_id.to_string()),
                )
                .await?;
            }
        }
    }
    Ok(Json(ticket_from_row(&s, row).await?))
}

#[derive(Deserialize)]
pub struct TicketVisibilityReq {
    pub session_token: String,
    pub visibility: String,
}

pub async fn update_ticket_visibility(
    State(s): State<Arc<AppState>>,
    Path(ticket_id): Path<Uuid>,
    Json(req): Json<TicketVisibilityReq>,
) -> Result<Json<TicketOut>, (StatusCode, String)> {
    let me = auth_user(&s, &req.session_token).await?;
    let access = ensure_ticket_access(&s, me, ticket_id).await?;
    if !access.is_admin {
        return Err((StatusCode::FORBIDDEN, "admin required".into()));
    }
    let visibility = match req.visibility.as_str() {
        "public" | "private" => req.visibility,
        _ => return Err((StatusCode::BAD_REQUEST, "bad visibility".into())),
    };
    let row = sqlx::query(
        r#"UPDATE tickets
           SET visibility = $2, updated_at = now()
           WHERE id = $1
           RETURNING id, chat_id, number, creator_id, category_id, title, status, visibility,
                     protected_by_superadmin, assigned_admin_id, resolved_by, resolved_at,
                     created_at, updated_at"#,
    )
    .bind(ticket_id)
    .bind(&visibility)
    .fetch_one(&s.db)
    .await
    .map_err(internal)?;
    sqlx::query("UPDATE chats SET is_public = $2 WHERE id = $1")
        .bind(access.chat_id)
        .bind(visibility == "public")
        .execute(&s.db)
        .await
        .map_err(internal)?;
    Ok(Json(ticket_from_row(&s, row).await?))
}

#[derive(Deserialize)]
pub struct TicketProtectReq {
    pub session_token: String,
    pub protect: bool,
    pub reason: Option<String>,
}

pub async fn protect_ticket(
    State(s): State<Arc<AppState>>,
    Path(ticket_id): Path<Uuid>,
    Json(req): Json<TicketProtectReq>,
) -> Result<Json<TicketOut>, (StatusCode, String)> {
    let me = auth_user(&s, &req.session_token).await?;
    let is_super_admin = ensure_admin(&s, me).await?;
    if !is_super_admin {
        return Err((StatusCode::FORBIDDEN, "super admin required".into()));
    }
    let reason = req.reason.as_deref().map(|s| clean_text(s, 240));
    let row = sqlx::query(
        r#"UPDATE tickets
           SET protected_by_superadmin = $2,
               protected_reason = CASE WHEN $2 THEN $3 ELSE NULL END,
               protected_at = CASE WHEN $2 THEN now() ELSE NULL END,
               protected_by = CASE WHEN $2 THEN $4 ELSE NULL END,
               updated_at = now()
           WHERE id = $1
           RETURNING id, chat_id, number, creator_id, category_id, title, status, visibility,
                     protected_by_superadmin, assigned_admin_id, resolved_by, resolved_at,
                     created_at, updated_at"#,
    )
    .bind(ticket_id)
    .bind(req.protect)
    .bind(reason)
    .bind(me)
    .fetch_one(&s.db)
    .await
    .map_err(internal)?;
    Ok(Json(ticket_from_row(&s, row).await?))
}

#[derive(Deserialize)]
pub struct AdminEditMessageReq {
    pub session_token: String,
    pub payload: serde_json::Value,
    pub reason: Option<String>,
    pub redact: Option<bool>,
}

pub async fn admin_edit_message(
    State(s): State<Arc<AppState>>,
    Path(message_id): Path<i64>,
    Json(req): Json<AdminEditMessageReq>,
) -> Result<StatusCode, (StatusCode, String)> {
    let me = auth_user(&s, &req.session_token).await?;
    let _ = ensure_admin(&s, me).await?;
    let msg =
        sqlx::query("SELECT chat_id, payload FROM messages WHERE id = $1 AND deleted_at IS NULL")
            .bind(message_id)
            .fetch_optional(&s.db)
            .await
            .map_err(internal)?
            .ok_or((StatusCode::NOT_FOUND, "message not found".into()))?;
    let chat_id: Uuid = msg.try_get("chat_id").map_err(internal)?;
    let ticket_row = sqlx::query("SELECT id FROM tickets WHERE chat_id = $1")
        .bind(chat_id)
        .fetch_optional(&s.db)
        .await
        .map_err(internal)?
        .ok_or((StatusCode::BAD_REQUEST, "message is not in a ticket".into()))?;
    let ticket_id: Uuid = ticket_row.try_get("id").map_err(internal)?;
    let access = ensure_ticket_access(&s, me, ticket_id).await?;
    if access.protected && !access.is_super_admin {
        return Err((StatusCode::FORBIDDEN, "ticket is protected".into()));
    }
    let old_payload: serde_json::Value = msg.try_get("payload").map_err(internal)?;
    let reason = req
        .reason
        .as_deref()
        .map(|s| clean_text(s, 240))
        .unwrap_or_default();
    let edit_kind = if req.redact.unwrap_or(false) {
        "redact"
    } else {
        "edit"
    };
    sqlx::query(
        r#"INSERT INTO message_admin_edits (message_id, editor_id, old_payload, new_payload, reason, edit_kind)
           VALUES ($1, $2, $3, $4, $5, $6)"#,
    )
    .bind(message_id)
    .bind(me)
    .bind(&old_payload)
    .bind(&req.payload)
    .bind(reason)
    .bind(edit_kind)
    .execute(&s.db)
    .await
    .map_err(internal)?;
    sqlx::query("UPDATE messages SET payload = $2, edited_at = now() WHERE id = $1")
        .bind(message_id)
        .bind(req.payload)
        .execute(&s.db)
        .await
        .map_err(internal)?;
    Ok(StatusCode::NO_CONTENT)
}

#[derive(Deserialize)]
pub struct CreateAnnouncementReq {
    pub session_token: String,
    pub title: String,
    pub body: String,
    pub severity: Option<String>,
    pub audience_role: Option<String>,
    pub min_level: Option<i32>,
    pub force_popup: Option<bool>,
    pub red_dot: Option<bool>,
}

pub async fn create_announcement(
    State(s): State<Arc<AppState>>,
    Json(req): Json<CreateAnnouncementReq>,
) -> Result<Json<AnnouncementOut>, (StatusCode, String)> {
    let me = auth_user(&s, &req.session_token).await?;
    let _ = ensure_admin(&s, me).await?;
    let title = clean_text(&req.title, 96);
    let body = clean_text(&req.body, 4000);
    if title.is_empty() || body.is_empty() {
        return Err((StatusCode::BAD_REQUEST, "title and body required".into()));
    }
    let severity = match req.severity.as_deref().unwrap_or("normal") {
        "normal" | "important" | "critical" => req.severity.unwrap_or_else(|| "normal".into()),
        _ => return Err((StatusCode::BAD_REQUEST, "bad severity".into())),
    };
    let row = sqlx::query(
        r#"INSERT INTO announcements
              (title, body, severity, audience_role, min_level, force_popup, red_dot, created_by)
           VALUES ($1, $2, $3, $4, $5, $6, $7, $8)
           RETURNING id, title, body, severity, force_popup, red_dot, starts_at, expires_at"#,
    )
    .bind(title)
    .bind(body)
    .bind(severity)
    .bind(req.audience_role)
    .bind(req.min_level.unwrap_or(1).clamp(1, 100))
    .bind(req.force_popup.unwrap_or(false))
    .bind(req.red_dot.unwrap_or(true))
    .bind(me)
    .fetch_one(&s.db)
    .await
    .map_err(internal)?;
    Ok(Json(AnnouncementOut {
        id: row.try_get::<Uuid, _>("id").map_err(internal)?.to_string(),
        title: row.try_get("title").map_err(internal)?,
        body: row.try_get("body").map_err(internal)?,
        severity: row.try_get("severity").map_err(internal)?,
        force_popup: row.try_get("force_popup").map_err(internal)?,
        red_dot: row.try_get("red_dot").map_err(internal)?,
        unread: true,
        read_at: None,
        acknowledged_at: None,
        starts_at: row
            .try_get::<DateTime<Utc>, _>("starts_at")
            .map_err(internal)?
            .timestamp(),
        expires_at: row
            .try_get::<Option<DateTime<Utc>>, _>("expires_at")
            .map_err(internal)?
            .map(|t| t.timestamp()),
    }))
}

pub async fn list_announcements(
    State(s): State<Arc<AppState>>,
    Query(q): Query<SessionQ>,
) -> Result<Json<Vec<AnnouncementOut>>, (StatusCode, String)> {
    let me = auth_user(&s, &q.session_token).await?;
    let (role, _, _, _) = user_role(&s, me).await?;
    let (level, _) = user_level(&s, me).await?;
    Ok(Json(
        active_announcements(&s, me, Some(&role), level).await?,
    ))
}

#[derive(Deserialize)]
pub struct AnnouncementStateReq {
    pub session_token: String,
}

pub async fn mark_announcement_read(
    State(s): State<Arc<AppState>>,
    Path(announcement_id): Path<Uuid>,
    Json(req): Json<AnnouncementStateReq>,
) -> Result<StatusCode, (StatusCode, String)> {
    let me = auth_user(&s, &req.session_token).await?;
    sqlx::query(
        r#"INSERT INTO announcement_reads (announcement_id, user_id, read_at)
           VALUES ($1, $2, now())
           ON CONFLICT (announcement_id, user_id) DO UPDATE
             SET read_at = COALESCE(announcement_reads.read_at, EXCLUDED.read_at)"#,
    )
    .bind(announcement_id)
    .bind(me)
    .execute(&s.db)
    .await
    .map_err(internal)?;
    Ok(StatusCode::NO_CONTENT)
}

pub async fn acknowledge_announcement(
    State(s): State<Arc<AppState>>,
    Path(announcement_id): Path<Uuid>,
    Json(req): Json<AnnouncementStateReq>,
) -> Result<StatusCode, (StatusCode, String)> {
    let me = auth_user(&s, &req.session_token).await?;
    sqlx::query(
        r#"INSERT INTO announcement_reads (announcement_id, user_id, read_at, acknowledged_at)
           VALUES ($1, $2, now(), now())
           ON CONFLICT (announcement_id, user_id) DO UPDATE
             SET read_at = COALESCE(announcement_reads.read_at, now()),
                 acknowledged_at = COALESCE(announcement_reads.acknowledged_at, now())"#,
    )
    .bind(announcement_id)
    .bind(me)
    .execute(&s.db)
    .await
    .map_err(internal)?;
    Ok(StatusCode::NO_CONTENT)
}

#[derive(Serialize)]
pub struct CheckinOut {
    pub level: i32,
    pub xp: i64,
    pub granted: bool,
}

pub async fn checkin(
    State(s): State<Arc<AppState>>,
    Json(req): Json<SessionQ>,
) -> Result<Json<CheckinOut>, (StatusCode, String)> {
    let me = auth_user(&s, &req.session_token).await?;
    let res = sqlx::query(
        r#"INSERT INTO user_progress (user_id, level, xp, last_checkin_at, updated_at)
           VALUES ($1, 1, 0, now(), now())
           ON CONFLICT (user_id) DO UPDATE
             SET last_checkin_at = now(), updated_at = now()
           WHERE user_progress.last_checkin_at IS NULL
              OR user_progress.last_checkin_at::date < CURRENT_DATE"#,
    )
    .bind(me)
    .execute(&s.db)
    .await
    .map_err(internal)?;
    let granted = res.rows_affected() > 0;
    if granted {
        grant_xp(&s, me, "daily_checkin", 5, Some("checkin"), None).await?;
    }
    let (level, xp) = user_level(&s, me).await?;
    Ok(Json(CheckinOut { level, xp, granted }))
}
