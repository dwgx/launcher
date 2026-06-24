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
use chrono::{DateTime, Utc};
use serde::Deserialize;
use sqlx::Row;
use std::sync::Arc;
use uuid::Uuid;

pub struct AdminTicketVm {
    pub id: String,
    pub chat_id: String,
    pub number: i64,
    pub title: String,
    pub category_name: String,
    pub creator: String,
    pub status: String,
    pub status_label: String,
    pub visibility: String,
    pub visibility_label: String,
    pub protected: bool,
    pub assigned_admin: String,
    pub created_at: String,
    pub updated_at: String,
}

pub struct TicketCategoryVm {
    pub id: String,
    pub slug: String,
    pub name: String,
    pub description: String,
    pub is_builtin: bool,
    pub is_enabled: bool,
    pub sort_order: i32,
}

pub struct TicketMessageVm {
    pub id: i64,
    pub sender: String,
    pub msg_type_label: String,
    pub text: String,
    pub payload_json: String,
    pub edited: bool,
    pub deleted: bool,
    pub created_at: String,
    pub edits: Vec<MessageEditVm>,
}

pub struct MessageEditVm {
    pub editor: String,
    pub edit_kind_label: String,
    pub reason: String,
    pub created_at: String,
}

#[derive(Template)]
#[template(path = "tickets_content.html")]
pub struct TicketsPage {
    pub title: String,
    pub subtitle: Option<String>,
    pub notice: Option<ui::AdminNotice>,
    pub host: &'static str,
    pub route: &'static str,
    pub tickets: Vec<AdminTicketVm>,
    pub categories: Vec<TicketCategoryVm>,
}

#[derive(Template)]
#[template(path = "ticket_detail_content.html")]
pub struct TicketDetailPage {
    pub title: String,
    pub subtitle: Option<String>,
    pub notice: Option<ui::AdminNotice>,
    pub host: &'static str,
    pub route: &'static str,
    pub ticket: AdminTicketVm,
    pub messages: Vec<TicketMessageVm>,
}

#[derive(Deserialize, Default)]
pub struct TicketsNoticeQuery {
    pub ok: Option<String>,
    pub err: Option<String>,
}

#[derive(Deserialize)]
pub struct StatusForm {
    pub status: String,
}

#[derive(Deserialize)]
pub struct VisibilityForm {
    pub visibility: String,
}

#[derive(Deserialize)]
pub struct CategoryForm {
    pub slug: Option<String>,
    pub name: String,
    pub description: Option<String>,
    pub sort_order: Option<i32>,
    pub is_enabled: Option<String>,
}

#[derive(Deserialize)]
pub struct EditMessageForm {
    pub text: String,
    pub reason: Option<String>,
    pub redact: Option<String>,
}

fn notice(q: &TicketsNoticeQuery) -> Option<ui::AdminNotice> {
    match (q.ok.as_deref(), q.err.as_deref()) {
        (Some("status"), _) => Some(ui::AdminNotice::success("工单状态已更新")),
        (Some("visibility"), _) => Some(ui::AdminNotice::success("工单公开状态已更新")),
        (Some("category"), _) => Some(ui::AdminNotice::success("工单分类已保存")),
        (Some("message"), _) => Some(ui::AdminNotice::success("消息已编辑并写入审计")),
        (_, Some("bad_status")) => Some(ui::AdminNotice::warning("不支持的工单状态")),
        (_, Some("bad_visibility")) => Some(ui::AdminNotice::warning("不支持的公开状态")),
        (_, Some("bad_category")) => Some(ui::AdminNotice::warning("分类名称或 slug 不合法")),
        (_, Some("failed")) => Some(ui::AdminNotice::error("操作失败，请检查服务日志")),
        _ => None,
    }
}

fn fmt_ts(ts: Option<DateTime<Utc>>) -> String {
    ts.map(|t| t.format("%Y-%m-%d %H:%M").to_string())
        .unwrap_or_else(|| "-".into())
}

fn clean_text(input: &str, max: usize) -> String {
    input.trim().chars().take(max).collect()
}

fn clean_slug(input: &str, max: usize) -> String {
    input
        .trim()
        .to_ascii_lowercase()
        .chars()
        .filter_map(|ch| {
            if ch.is_ascii_alphanumeric() || matches!(ch, '-' | '_') {
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

fn message_text(payload: &serde_json::Value) -> String {
    payload
        .get("text")
        .and_then(|v| v.as_str())
        .map(ToOwned::to_owned)
        .unwrap_or_else(|| payload.to_string())
}

fn ticket_status_label(v: &str) -> &'static str {
    match v {
        "open" => "处理中",
        "pending" => "待处理",
        "resolved" => "已解决",
        "closed" => "已关闭",
        _ => "未知",
    }
}

fn visibility_label(v: &str) -> &'static str {
    match v {
        "private" => "私密",
        "public" => "公开",
        _ => "未知",
    }
}

fn msg_type_label(v: &str) -> &'static str {
    match v {
        "text" => "文本",
        "image" => "图片",
        "video" => "视频",
        "gif" => "GIF",
        "sticker" => "表情",
        "pack_share" => "表情包",
        "system" => "系统",
        _ => "未知",
    }
}

fn edit_kind_label(v: &str) -> &'static str {
    match v {
        "redact" => "打码",
        "edit" => "编辑",
        _ => "编辑",
    }
}

async fn tickets_page(
    State(s): State<Arc<AppState>>,
    headers: HeaderMap,
    Query(q): Query<TicketsNoticeQuery>,
) -> Response {
    if let Err(resp) =
        crate::admin_customization::require_actor(&headers, &s, "admin.tickets.read").await
    {
        return resp;
    }
    let tickets = fetch_tickets(&s, "WHERE t.protected_by_superadmin = FALSE").await;
    let categories = fetch_categories(&s).await;

    ui::render(&TicketsPage {
        title: "工单管控".into(),
        subtitle: Some("普通管理员不可见 superadmin 保护工单；内容编辑会写入审计记录".into()),
        notice: notice(&q),
        host: ui::host(),
        route: ui::ROUTE_TICKETS,
        tickets,
        categories,
    })
    .into_response()
}

async fn ticket_detail_page(
    State(s): State<Arc<AppState>>,
    headers: HeaderMap,
    Path(id): Path<Uuid>,
    Query(q): Query<TicketsNoticeQuery>,
) -> Response {
    if let Err(resp) =
        crate::admin_customization::require_actor(&headers, &s, "admin.tickets.read").await
    {
        return resp;
    }
    let Some(ticket) = fetch_ticket(&s, id).await else {
        return Redirect::to("/admin/tickets?err=failed").into_response();
    };
    if ticket.protected {
        return Redirect::to("/admin/tickets?err=failed").into_response();
    }
    let messages = fetch_ticket_messages(&s, id).await;
    ui::render(&TicketDetailPage {
        title: format!("工单 #{}", ticket.number),
        subtitle: Some("公开/私密工单的聊天内容可在这里做隐私脱敏编辑".into()),
        notice: notice(&q),
        host: ui::host(),
        route: ui::ROUTE_TICKETS,
        ticket,
        messages,
    })
    .into_response()
}

fn tickets_sql(where_clause: &str) -> String {
    format!(
        r#"SELECT t.id, t.chat_id, t.number, t.title, t.status, t.visibility,
                  t.protected_by_superadmin, t.created_at, t.updated_at,
                  tc.name AS category_name,
                  COALESCE(u.nickname, u.username, u.uid) AS creator,
                  COALESCE(a.nickname, a.username, a.uid) AS assigned_admin
           FROM tickets t
           LEFT JOIN ticket_categories tc ON tc.id = t.category_id
           LEFT JOIN users u ON u.id = t.creator_id
           LEFT JOIN users a ON a.id = t.assigned_admin_id
           {where_clause}
           ORDER BY t.updated_at DESC
           LIMIT 200"#
    )
}

async fn fetch_tickets(s: &AppState, where_clause: &str) -> Vec<AdminTicketVm> {
    let sql = tickets_sql(where_clause);
    let rows = sqlx::query(&sql).fetch_all(&s.db).await.unwrap_or_default();
    rows.into_iter().map(ticket_vm_from_row).collect()
}

async fn fetch_ticket(s: &AppState, id: Uuid) -> Option<AdminTicketVm> {
    let sql = tickets_sql("WHERE t.id = $1 AND t.protected_by_superadmin = FALSE");
    let row = sqlx::query(&sql)
        .bind(id)
        .fetch_optional(&s.db)
        .await
        .ok()??;
    Some(ticket_vm_from_row(row))
}

fn ticket_vm_from_row(r: sqlx::postgres::PgRow) -> AdminTicketVm {
    let status: String = r.try_get("status").unwrap_or_default();
    let visibility: String = r.try_get("visibility").unwrap_or_default();
    AdminTicketVm {
        id: r
            .try_get::<Uuid, _>("id")
            .map(|v| v.to_string())
            .unwrap_or_default(),
        chat_id: r
            .try_get::<Uuid, _>("chat_id")
            .map(|v| v.to_string())
            .unwrap_or_default(),
        number: r.try_get("number").unwrap_or(0),
        title: r.try_get("title").unwrap_or_default(),
        category_name: r
            .try_get::<Option<String>, _>("category_name")
            .ok()
            .flatten()
            .unwrap_or_else(|| "-".into()),
        creator: r
            .try_get::<Option<String>, _>("creator")
            .ok()
            .flatten()
            .unwrap_or_else(|| "-".into()),
        status_label: ticket_status_label(&status).into(),
        status,
        visibility_label: visibility_label(&visibility).into(),
        visibility,
        protected: r.try_get("protected_by_superadmin").unwrap_or(false),
        assigned_admin: r
            .try_get::<Option<String>, _>("assigned_admin")
            .ok()
            .flatten()
            .unwrap_or_else(|| "-".into()),
        created_at: fmt_ts(r.try_get("created_at").ok()),
        updated_at: fmt_ts(r.try_get("updated_at").ok()),
    }
}

async fn fetch_categories(s: &AppState) -> Vec<TicketCategoryVm> {
    let rows = sqlx::query(
        r#"SELECT id, slug, name, description, is_builtin, is_enabled, sort_order
           FROM ticket_categories ORDER BY sort_order, name"#,
    )
    .fetch_all(&s.db)
    .await
    .unwrap_or_default();
    rows.into_iter()
        .map(|r| TicketCategoryVm {
            id: r
                .try_get::<Uuid, _>("id")
                .map(|v| v.to_string())
                .unwrap_or_default(),
            slug: r.try_get("slug").unwrap_or_default(),
            name: r.try_get("name").unwrap_or_default(),
            description: r.try_get("description").unwrap_or_default(),
            is_builtin: r.try_get("is_builtin").unwrap_or(false),
            is_enabled: r.try_get("is_enabled").unwrap_or(false),
            sort_order: r.try_get("sort_order").unwrap_or(100),
        })
        .collect()
}

async fn fetch_ticket_messages(s: &AppState, ticket_id: Uuid) -> Vec<TicketMessageVm> {
    let Some(chat_id) = sqlx::query_scalar::<_, Uuid>(
        "SELECT chat_id FROM tickets WHERE id = $1 AND protected_by_superadmin = FALSE",
    )
    .bind(ticket_id)
    .fetch_optional(&s.db)
    .await
    .unwrap_or(None) else {
        return Vec::new();
    };
    let rows = sqlx::query(
        r#"SELECT m.id, COALESCE(u.nickname, u.username, u.uid, 'system') AS sender,
                  m.msg_type, m.payload, m.edited_at, m.deleted_at, m.created_at
           FROM messages m
           LEFT JOIN users u ON u.id = m.sender_id
           WHERE m.chat_id = $1
           ORDER BY m.id ASC
           LIMIT 500"#,
    )
    .bind(chat_id)
    .fetch_all(&s.db)
    .await
    .unwrap_or_default();

    let mut messages = Vec::with_capacity(rows.len());
    for r in rows {
        let id: i64 = r.try_get("id").unwrap_or_default();
        let payload: serde_json::Value = r
            .try_get("payload")
            .unwrap_or_else(|_| serde_json::json!({}));
        let edits = fetch_message_edits(s, id).await;
        let msg_type: String = r.try_get("msg_type").unwrap_or_else(|_| "text".into());
        messages.push(TicketMessageVm {
            id,
            sender: r.try_get("sender").unwrap_or_else(|_| "system".into()),
            msg_type_label: msg_type_label(&msg_type).into(),
            text: message_text(&payload),
            payload_json: payload.to_string(),
            edited: r
                .try_get::<Option<DateTime<Utc>>, _>("edited_at")
                .ok()
                .flatten()
                .is_some(),
            deleted: r
                .try_get::<Option<DateTime<Utc>>, _>("deleted_at")
                .ok()
                .flatten()
                .is_some(),
            created_at: fmt_ts(r.try_get("created_at").ok()),
            edits,
        });
    }
    messages
}

async fn fetch_message_edits(s: &AppState, message_id: i64) -> Vec<MessageEditVm> {
    let rows = sqlx::query(
        r#"SELECT COALESCE(u.nickname, u.username, u.uid, 'admin') AS editor,
                  e.edit_kind, e.reason, e.created_at
           FROM message_admin_edits e
           LEFT JOIN users u ON u.id = e.editor_id
           WHERE e.message_id = $1
           ORDER BY e.created_at DESC"#,
    )
    .bind(message_id)
    .fetch_all(&s.db)
    .await
    .unwrap_or_default();
    rows.into_iter()
        .map(|r| {
            let edit_kind: String = r.try_get("edit_kind").unwrap_or_else(|_| "edit".into());
            MessageEditVm {
                editor: r.try_get("editor").unwrap_or_else(|_| "admin".into()),
                edit_kind_label: edit_kind_label(&edit_kind).into(),
                reason: r.try_get("reason").unwrap_or_default(),
                created_at: fmt_ts(r.try_get("created_at").ok()),
            }
        })
        .collect()
}

async fn update_status(
    State(s): State<Arc<AppState>>,
    headers: HeaderMap,
    Path(id): Path<Uuid>,
    Form(form): Form<StatusForm>,
) -> Response {
    let actor =
        match crate::admin_customization::require_actor(&headers, &s, "admin.tickets.manage").await
        {
            Ok(v) => v,
            Err(resp) => return resp,
        };
    if !matches!(
        form.status.as_str(),
        "open" | "pending" | "resolved" | "closed"
    ) {
        return Redirect::to("/admin/tickets?err=bad_status").into_response();
    }
    let resolved = form.status == "resolved";
    let res = sqlx::query(
        r#"UPDATE tickets
           SET status = $2,
               resolved_at = CASE WHEN $3 THEN now() ELSE resolved_at END,
               updated_at = now()
           WHERE id = $1 AND protected_by_superadmin = FALSE"#,
    )
    .bind(id)
    .bind(&form.status)
    .bind(resolved)
    .execute(&s.db)
    .await;
    if res.is_err() {
        return Redirect::to("/admin/tickets?err=failed").into_response();
    }
    crate::admin_customization::write_audit(
        &s,
        &actor,
        "admin.ticket_status",
        &id.to_string(),
        serde_json::json!({ "status": form.status }),
    )
    .await;
    Redirect::to("/admin/tickets?ok=status").into_response()
}

async fn update_visibility(
    State(s): State<Arc<AppState>>,
    headers: HeaderMap,
    Path(id): Path<Uuid>,
    Form(form): Form<VisibilityForm>,
) -> Response {
    let actor =
        match crate::admin_customization::require_actor(&headers, &s, "admin.tickets.manage").await
        {
            Ok(v) => v,
            Err(resp) => return resp,
        };
    if !matches!(form.visibility.as_str(), "private" | "public") {
        return Redirect::to("/admin/tickets?err=bad_visibility").into_response();
    }
    let res = sqlx::query(
        r#"WITH upd AS (
             UPDATE tickets SET visibility = $2, updated_at = now()
             WHERE id = $1 AND protected_by_superadmin = FALSE RETURNING chat_id
           )
           UPDATE chats SET is_public = ($2 = 'public')
           WHERE id IN (SELECT chat_id FROM upd)"#,
    )
    .bind(id)
    .bind(&form.visibility)
    .execute(&s.db)
    .await;
    if res.is_err() {
        return Redirect::to("/admin/tickets?err=failed").into_response();
    }
    crate::admin_customization::write_audit(
        &s,
        &actor,
        "admin.ticket_visibility",
        &id.to_string(),
        serde_json::json!({ "visibility": form.visibility }),
    )
    .await;
    Redirect::to("/admin/tickets?ok=visibility").into_response()
}

async fn create_category(
    State(s): State<Arc<AppState>>,
    headers: HeaderMap,
    Form(form): Form<CategoryForm>,
) -> Response {
    let actor =
        match crate::admin_customization::require_actor(&headers, &s, "admin.tickets.manage").await
        {
            Ok(v) => v,
            Err(resp) => return resp,
        };
    let name = clean_text(&form.name, 64);
    let slug = form
        .slug
        .as_deref()
        .map(|v| clean_slug(v, 48))
        .filter(|v| !v.is_empty())
        .unwrap_or_else(|| clean_slug(&name, 48));
    if name.is_empty() || slug.is_empty() {
        return Redirect::to("/admin/tickets?err=bad_category").into_response();
    }
    let res = sqlx::query(
        r#"INSERT INTO ticket_categories
              (slug, name, description, is_builtin, is_enabled, sort_order, updated_at)
           VALUES ($1, $2, $3, FALSE, TRUE, $4, now())"#,
    )
    .bind(&slug)
    .bind(&name)
    .bind(
        form.description
            .as_deref()
            .map(|v| clean_text(v, 240))
            .unwrap_or_default(),
    )
    .bind(form.sort_order.unwrap_or(100).clamp(0, 10_000))
    .execute(&s.db)
    .await;
    if res.is_err() {
        return Redirect::to("/admin/tickets?err=failed").into_response();
    }
    crate::admin_customization::write_audit(
        &s,
        &actor,
        "admin.ticket_category_create",
        &slug,
        serde_json::json!({ "name": name }),
    )
    .await;
    Redirect::to("/admin/tickets?ok=category").into_response()
}

async fn update_category(
    State(s): State<Arc<AppState>>,
    headers: HeaderMap,
    Path(id): Path<Uuid>,
    Form(form): Form<CategoryForm>,
) -> Response {
    let actor =
        match crate::admin_customization::require_actor(&headers, &s, "admin.tickets.manage").await
        {
            Ok(v) => v,
            Err(resp) => return resp,
        };
    let name = clean_text(&form.name, 64);
    if name.is_empty() {
        return Redirect::to("/admin/tickets?err=bad_category").into_response();
    }
    let res = sqlx::query(
        r#"UPDATE ticket_categories
           SET name = $2,
               description = $3,
               is_enabled = $4,
               sort_order = $5,
               updated_at = now()
           WHERE id = $1"#,
    )
    .bind(id)
    .bind(&name)
    .bind(
        form.description
            .as_deref()
            .map(|v| clean_text(v, 240))
            .unwrap_or_default(),
    )
    .bind(form.is_enabled.is_some())
    .bind(form.sort_order.unwrap_or(100).clamp(0, 10_000))
    .execute(&s.db)
    .await;
    if res.is_err() {
        return Redirect::to("/admin/tickets?err=failed").into_response();
    }
    crate::admin_customization::write_audit(
        &s,
        &actor,
        "admin.ticket_category_update",
        &id.to_string(),
        serde_json::json!({ "name": name, "enabled": form.is_enabled.is_some() }),
    )
    .await;
    Redirect::to("/admin/tickets?ok=category").into_response()
}

async fn edit_message(
    State(s): State<Arc<AppState>>,
    headers: HeaderMap,
    Path((ticket_id, message_id)): Path<(Uuid, i64)>,
    Form(form): Form<EditMessageForm>,
) -> Response {
    let actor =
        match crate::admin_customization::require_actor(&headers, &s, "admin.tickets.manage").await
        {
            Ok(v) => v,
            Err(resp) => return resp,
        };
    let text = if form.redact.is_some() {
        "[redacted by admin]".to_string()
    } else {
        clean_text(&form.text, 4000)
    };
    if text.is_empty() {
        return Redirect::to(&format!("/admin/tickets/{ticket_id}?err=failed")).into_response();
    }
    let payload = serde_json::json!({ "text": text });
    let reason = form
        .reason
        .as_deref()
        .map(|v| clean_text(v, 240))
        .unwrap_or_default();
    let msg = sqlx::query(
        r#"SELECT m.payload
           FROM messages m
           JOIN tickets t ON t.chat_id = m.chat_id
           WHERE t.id = $1 AND t.protected_by_superadmin = FALSE
             AND m.id = $2 AND m.deleted_at IS NULL"#,
    )
    .bind(ticket_id)
    .bind(message_id)
    .fetch_optional(&s.db)
    .await;
    let Ok(Some(msg)) = msg else {
        return Redirect::to(&format!("/admin/tickets/{ticket_id}?err=failed")).into_response();
    };
    let old_payload: serde_json::Value = msg
        .try_get("payload")
        .unwrap_or_else(|_| serde_json::json!({}));
    let res = sqlx::query(
        r#"INSERT INTO message_admin_edits
              (message_id, old_payload, new_payload, reason, edit_kind)
           VALUES ($1, $2, $3, $4, $5)"#,
    )
    .bind(message_id)
    .bind(old_payload)
    .bind(&payload)
    .bind(reason)
    .bind(if form.redact.is_some() {
        "redact"
    } else {
        "edit"
    })
    .execute(&s.db)
    .await;
    if res.is_err() {
        return Redirect::to(&format!("/admin/tickets/{ticket_id}?err=failed")).into_response();
    }
    let res = sqlx::query("UPDATE messages SET payload = $2, edited_at = now() WHERE id = $1")
        .bind(message_id)
        .bind(payload)
        .execute(&s.db)
        .await;
    if res.is_err() {
        return Redirect::to(&format!("/admin/tickets/{ticket_id}?err=failed")).into_response();
    }
    crate::admin_customization::write_audit(
        &s,
        &actor,
        "admin.ticket_message_edit",
        &message_id.to_string(),
        serde_json::json!({
            "ticket_id": ticket_id,
            "kind": if form.redact.is_some() { "redact" } else { "edit" }
        }),
    )
    .await;
    Redirect::to(&format!("/admin/tickets/{ticket_id}?ok=message")).into_response()
}

pub fn routes() -> Router<Arc<AppState>> {
    Router::new()
        .route("/admin/tickets", get(tickets_page))
        .route("/admin/tickets/:id", get(ticket_detail_page))
        .route("/admin/tickets/:id/status", post(update_status))
        .route("/admin/tickets/:id/visibility", post(update_visibility))
        .route(
            "/admin/tickets/:ticket_id/messages/:message_id/edit",
            post(edit_message),
        )
        .route("/admin/ticket-categories", post(create_category))
        .route("/admin/ticket-categories/:id", post(update_category))
}
