// 官方频道 admin SSR — 列出 / 改名 / 清空消息（不允许删，因为客户端写死）。
// DaisyUI dropdown 行级操作 + dialog modal 改名。

use crate::state::AppState;
use crate::ui;
use axum::{
    extract::{State, Path, Form, Query},
    http::HeaderMap,
    response::{IntoResponse, Redirect, Response},
    Router,
    routing::{get, post},
};
use serde::Deserialize;
use std::sync::Arc;
use uuid::Uuid;
use askama::Template;

pub struct ChannelVm {
    pub id:           String,
    pub slug:         String,
    pub title:        String,
    pub group_label:  String,
    pub message_count: i64,
    pub last_at:      String,
    pub is_official:  bool,
}

#[derive(Template)]
#[template(path = "channels_content.html")]
pub struct ChannelsPage {
    pub title:    String,
    pub subtitle: Option<String>,
    pub notice:   Option<ui::AdminNotice>,
    pub host:     &'static str,
    pub route:    &'static str,
    pub channels: Vec<ChannelVm>,
}

#[derive(Deserialize, Default)]
pub struct ChannelsNoticeQuery {
    pub ok: Option<String>,
    pub err: Option<String>,
}

fn channels_notice(q: &ChannelsNoticeQuery) -> Option<ui::AdminNotice> {
    match (q.ok.as_deref(), q.err.as_deref()) {
        (Some("rename"), _) => Some(ui::AdminNotice::success("频道名称已保存")),
        (Some("clear"), _) => Some(ui::AdminNotice::success("频道消息已清空")),
        (_, Some("title")) => Some(ui::AdminNotice::warning("频道标题不能为空，且最多 64 个字符")),
        _ => None,
    }
}

async fn channels_page(
    State(s): State<Arc<AppState>>,
    headers: HeaderMap,
    Query(q): Query<ChannelsNoticeQuery>,
) -> Response {
    if !crate::admin::has_admin_session(&headers, &s) {
        return crate::admin::admin_login_redirect();
    }

    let rows = sqlx::query!(
        r#"SELECT c.id, c.slug, c.title, c.group_label, c.is_official, c.last_message_at,
                  (SELECT COUNT(*) FROM messages m WHERE m.chat_id = c.id AND m.deleted_at IS NULL) AS msg_count
           FROM chats c
           WHERE c.kind = 'channel'
           ORDER BY c.is_official DESC, c.group_label NULLS LAST, c.title"#)
        .fetch_all(&s.db).await.unwrap_or_default();

    let channels = rows.into_iter().map(|r| ChannelVm {
        id:           r.id.to_string(),
        slug:         r.slug.unwrap_or_default(),
        title:        r.title.unwrap_or_default(),
        group_label:  r.group_label.unwrap_or_else(|| "—".into()),
        message_count: r.msg_count.unwrap_or(0),
        last_at: r.last_message_at
            .map(|t| t.format("%Y-%m-%d %H:%M").to_string())
            .unwrap_or_else(|| "—".into()),
        is_official: r.is_official,
    }).collect();

    ui::render(&ChannelsPage {
        title: "频道管控".into(),
        subtitle: Some("官方频道写死，可改名 / 清空消息，但不能删除".into()),
        notice: channels_notice(&q),
        host: ui::host(),
        route: ui::ROUTE_CHANNELS,
        channels,
    }).into_response()
}

#[derive(Deserialize)]
pub struct RenameForm { pub title: String }

async fn rename_submit(
    State(s): State<Arc<AppState>>,
    headers: HeaderMap,
    Path(id): Path<Uuid>,
    Form(form): Form<RenameForm>,
) -> Response {
    if !crate::admin::has_admin_session(&headers, &s) {
        return crate::admin::admin_login_redirect();
    }

    let title = form.title.trim();
    if title.is_empty() || title.len() > 64 {
        return Redirect::to("/admin/channels?err=title").into_response();
    }
    let _ = sqlx::query!(
        "UPDATE chats SET title=$1 WHERE id=$2 AND kind='channel'",
        title, id).execute(&s.db).await;
    let _ = sqlx::query!(
        "INSERT INTO audit_log (actor, action, target, metadata) VALUES ('admin','admin.channel_rename',$1,$2)",
        id.to_string(),
        Some(serde_json::json!({ "title": title }))).execute(&s.db).await;
    Redirect::to("/admin/channels?ok=rename").into_response()
}

async fn clear_messages(
    State(s): State<Arc<AppState>>,
    headers: HeaderMap,
    Path(id): Path<Uuid>,
) -> Response {
    if !crate::admin::has_admin_session(&headers, &s) {
        return crate::admin::admin_login_redirect();
    }

    let res = sqlx::query!(
        "UPDATE messages SET deleted_at=now() WHERE chat_id=$1 AND deleted_at IS NULL",
        id).execute(&s.db).await;
    let n = res.map(|r| r.rows_affected()).unwrap_or(0);
    let _ = sqlx::query!(
        "INSERT INTO audit_log (actor, action, target, metadata) VALUES ('admin','admin.channel_clear',$1,$2)",
        id.to_string(),
        Some(serde_json::json!({ "soft_deleted": n }))).execute(&s.db).await;
    Redirect::to("/admin/channels?ok=clear").into_response()
}

pub fn routes() -> Router<Arc<AppState>> {
    Router::new()
        .route("/admin/channels",                 get(channels_page))
        .route("/admin/channels/:id/rename",      post(rename_submit))
        .route("/admin/channels/:id/clear",       post(clear_messages))
}
