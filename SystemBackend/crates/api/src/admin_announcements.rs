use crate::state::AppState;
use crate::ui;
use askama::Template;
use axum::{
    extract::{Form, Query, State},
    http::HeaderMap,
    response::{IntoResponse, Redirect, Response},
    routing::get,
    Router,
};
use chrono::{DateTime, Utc};
use serde::Deserialize;
use sqlx::Row;
use std::sync::Arc;

pub struct AnnouncementVm {
    pub title: String,
    pub body: String,
    pub severity_label: String,
    pub audience_role: String,
    pub min_level: i32,
    pub force_popup: bool,
    pub red_dot: bool,
    pub read_count: i64,
    pub ack_count: i64,
    pub starts_at: String,
    pub expires_at: String,
}

#[derive(Template)]
#[template(path = "announcements_content.html")]
pub struct AnnouncementsPage {
    pub title: String,
    pub subtitle: Option<String>,
    pub notice: Option<ui::AdminNotice>,
    pub host: &'static str,
    pub route: &'static str,
    pub items: Vec<AnnouncementVm>,
}

#[derive(Deserialize, Default)]
pub struct NoticeQuery {
    pub ok: Option<String>,
    pub err: Option<String>,
}

#[derive(Deserialize)]
pub struct AnnouncementForm {
    pub title: String,
    pub body: String,
    pub severity: String,
    pub audience_role: Option<String>,
    pub min_level: Option<i32>,
    pub force_popup: Option<String>,
    pub red_dot: Option<String>,
    pub expires_days: Option<i64>,
}

fn notice(q: &NoticeQuery) -> Option<ui::AdminNotice> {
    match (q.ok.as_deref(), q.err.as_deref()) {
        (Some("create"), _) => Some(ui::AdminNotice::success("公告已发布")),
        (_, Some("bad_input")) => Some(ui::AdminNotice::warning("标题、正文或级别无效")),
        (_, Some("failed")) => Some(ui::AdminNotice::error("公告操作失败")),
        _ => None,
    }
}

fn fmt_ts(ts: Option<DateTime<Utc>>) -> String {
    ts.map(|t| t.format("%Y-%m-%d %H:%M").to_string())
        .unwrap_or_else(|| "-".into())
}

fn severity_label(v: &str) -> &'static str {
    match v {
        "normal" => "普通",
        "important" => "重要",
        "critical" => "关键",
        _ => "未知",
    }
}

async fn list_page(
    State(s): State<Arc<AppState>>,
    headers: HeaderMap,
    Query(q): Query<NoticeQuery>,
) -> Response {
    if let Err(resp) =
        crate::admin_customization::require_actor(&headers, &s, "admin.announcements.read").await
    {
        return resp;
    }

    let rows = sqlx::query(
        r#"SELECT a.title, a.body, a.severity, a.audience_role, a.min_level,
                  a.force_popup, a.red_dot, a.starts_at, a.expires_at,
                  COUNT(ar.user_id)::BIGINT AS read_count,
                  COUNT(ar.acknowledged_at)::BIGINT AS ack_count
           FROM announcements a
           LEFT JOIN announcement_reads ar ON ar.announcement_id = a.id
           GROUP BY a.id
           ORDER BY a.starts_at DESC
           LIMIT 100"#,
    )
    .fetch_all(&s.db)
    .await
    .unwrap_or_default();

    let items = rows
        .into_iter()
        .map(|r| {
            let severity: String = r.try_get("severity").unwrap_or_else(|_| "normal".into());
            AnnouncementVm {
                title: r.try_get("title").unwrap_or_default(),
                body: r.try_get("body").unwrap_or_default(),
                severity_label: severity_label(&severity).into(),
                audience_role: r
                    .try_get::<Option<String>, _>("audience_role")
                    .ok()
                    .flatten()
                    .unwrap_or_else(|| "all".into()),
                min_level: r.try_get("min_level").unwrap_or(1),
                force_popup: r.try_get("force_popup").unwrap_or(false),
                red_dot: r.try_get("red_dot").unwrap_or(false),
                read_count: r.try_get("read_count").unwrap_or(0),
                ack_count: r.try_get("ack_count").unwrap_or(0),
                starts_at: fmt_ts(r.try_get("starts_at").ok()),
                expires_at: fmt_ts(r.try_get("expires_at").ok()),
            }
        })
        .collect();

    ui::render(&AnnouncementsPage {
        title: "公告".into(),
        subtitle: Some("普通公告显示红点；关键公告可在客户端启动时强制弹窗。".into()),
        notice: notice(&q),
        host: ui::host(),
        route: ui::ROUTE_ANNOUNCEMENTS,
        items,
    })
    .into_response()
}

async fn create_submit(
    State(s): State<Arc<AppState>>,
    headers: HeaderMap,
    Form(form): Form<AnnouncementForm>,
) -> Response {
    let actor =
        match crate::admin_customization::require_actor(&headers, &s, "admin.announcements.manage")
            .await
        {
            Ok(v) => v,
            Err(resp) => return resp,
        };
    let title = form.title.trim().chars().take(96).collect::<String>();
    let body = form.body.trim().chars().take(4000).collect::<String>();
    if title.is_empty()
        || body.is_empty()
        || !matches!(form.severity.as_str(), "normal" | "important" | "critical")
    {
        return Redirect::to("/admin/announcements?err=bad_input").into_response();
    }
    let audience_role = form
        .audience_role
        .as_deref()
        .map(str::trim)
        .filter(|s| !s.is_empty())
        .map(|s| s.chars().take(32).collect::<String>());
    let expires_at = form
        .expires_days
        .filter(|d| *d > 0)
        .map(|d| Utc::now() + chrono::Duration::days(d.min(365)));
    let res = sqlx::query(
        r#"INSERT INTO announcements
              (title, body, severity, audience_role, min_level, force_popup, red_dot, expires_at, created_by)
           VALUES ($1, $2, $3, $4, $5, $6, $7, $8, NULL)"#,
    )
    .bind(title)
    .bind(body)
    .bind(&form.severity)
    .bind(audience_role)
    .bind(form.min_level.unwrap_or(1).clamp(1, 100))
    .bind(form.force_popup.is_some())
    .bind(form.red_dot.is_some())
    .bind(expires_at)
    .execute(&s.db)
    .await;
    if res.is_err() {
        return Redirect::to("/admin/announcements?err=failed").into_response();
    }
    crate::admin_customization::write_audit(
        &s,
        &actor,
        "admin.announcement_create",
        "announcements",
        serde_json::json!({ "severity": form.severity }),
    )
    .await;
    Redirect::to("/admin/announcements?ok=create").into_response()
}

pub fn routes() -> Router<Arc<AppState>> {
    Router::new().route("/admin/announcements", get(list_page).post(create_submit))
}
