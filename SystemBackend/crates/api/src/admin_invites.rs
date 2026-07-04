// 邀请码 admin SSR + JSON API。

use crate::state::AppState;
use crate::ui;
use askama::Template;
use axum::{
    extract::{Form, Path, Query, State},
    http::{HeaderMap, StatusCode},
    response::{IntoResponse, Redirect, Response},
    routing::{get, post},
    Router,
};
use rand::{rngs::OsRng, Rng};
use serde::Deserialize;
use std::sync::Arc;

const ALPH: &[u8; 32] = b"23456789ABCDEFGHJKLMNPQRSTUVWXYZ";

fn gen_code(len: usize) -> String {
    let mut rng = OsRng;
    (0..len)
        .map(|_| ALPH[rng.gen_range(0..ALPH.len())] as char)
        .collect()
}

pub struct InviteVm {
    pub code: String,
    pub note: String,
    pub uses: String,
    pub status: String,
    pub created: String,
}

#[derive(Template)]
#[template(path = "invites_content.html")]
pub struct InvitesPage {
    pub title: String,
    pub subtitle: Option<String>,
    pub notice: Option<ui::AdminNotice>,
    pub host: &'static str,
    pub route: &'static str,
    pub items: Vec<InviteVm>,
}

#[derive(Deserialize, Default)]
pub struct InvitesNoticeQuery {
    pub ok: Option<String>,
    pub err: Option<String>,
}

fn invites_notice(q: &InvitesNoticeQuery) -> Option<ui::AdminNotice> {
    match (q.ok.as_deref(), q.err.as_deref()) {
        (Some("create"), _) => Some(ui::AdminNotice::success("邀请码已生成")),
        (Some("revoke"), _) => Some(ui::AdminNotice::success("邀请码已撤销")),
        (_, Some("create_failed")) => Some(ui::AdminNotice::error("邀请码生成失败，请重试")),
        (_, Some("revoke_failed")) => Some(ui::AdminNotice::error("邀请码撤销失败，请重试")),
        _ => None,
    }
}

async fn list_page(
    State(s): State<Arc<AppState>>,
    headers: HeaderMap,
    Query(q): Query<InvitesNoticeQuery>,
) -> Response {
    if let Err(resp) =
        crate::admin_customization::require_actor(&headers, &s, "admin.invites.read").await
    {
        return resp;
    }

    let rows = sqlx::query!(
        r#"SELECT code, note, max_uses, use_count, expires_at, revoked_at, created_at
           FROM invite_codes ORDER BY created_at DESC LIMIT 200"#
    )
    .fetch_all(&s.db)
    .await
    .unwrap_or_default();
    let items = rows
        .into_iter()
        .map(|r| {
            let status = if r.revoked_at.is_some() {
                "revoked"
            } else if r
                .expires_at
                .map(|e| e < chrono::Utc::now())
                .unwrap_or(false)
            {
                "expired"
            } else if r.use_count >= r.max_uses {
                "exhausted"
            } else {
                "active"
            };
            InviteVm {
                code: r.code,
                note: r.note.unwrap_or_default(),
                uses: format!("{}/{}", r.use_count, r.max_uses),
                status: status.into(),
                created: r.created_at.format("%Y-%m-%d %H:%M").to_string(),
            }
        })
        .collect();
    ui::render(&InvitesPage {
        title: "邀请码".into(),
        subtitle: Some("注册必须带一个有效邀请码".into()),
        notice: invites_notice(&q),
        host: ui::host(),
        route: ui::ROUTE_INVITES,
        items,
    })
    .into_response()
}

// HTML form 留空字段会发 `field=`（空字符串）；serde 默认 Option<i32>::deserialize 把它
// 当成 Some("") 然后 i32::from_str("") 报错。统一走 empty_str_as_none。
#[derive(Deserialize)]
pub struct CreateForm {
    pub note: Option<String>,
    #[serde(
        default,
        deserialize_with = "launcher_shared::formhelp::empty_str_as_none"
    )]
    pub max_uses: Option<i32>,
    #[serde(
        default,
        deserialize_with = "launcher_shared::formhelp::empty_str_as_none"
    )]
    pub days: Option<i64>,
    #[serde(
        default,
        deserialize_with = "launcher_shared::formhelp::empty_str_as_none"
    )]
    pub count: Option<i32>,
}

async fn create_submit(
    State(s): State<Arc<AppState>>,
    headers: HeaderMap,
    Form(form): Form<CreateForm>,
) -> Response {
    let actor =
        match crate::admin_customization::require_actor(&headers, &s, "admin.invites.manage").await
        {
            Ok(v) => v,
            Err(resp) => return resp,
        };

    let n = form.count.unwrap_or(1).clamp(1, 50);
    let max_uses = form.max_uses.unwrap_or(1).clamp(1, 1000);
    let expires_at = form
        .days
        .map(|d| chrono::Utc::now() + chrono::Duration::days(d));
    let actor_name = actor.name.clone();
    let note = form.note.as_deref().and_then(|raw| {
        let trimmed = raw.trim();
        if trimmed.is_empty() {
            None
        } else {
            Some(trimmed.chars().take(240).collect::<String>())
        }
    });
    for _ in 0..n {
        let code = gen_code(8);
        let _ = sqlx::query(
            r#"INSERT INTO invite_codes (code, note, max_uses, expires_at, created_by)
               VALUES ($1, $2, $3, $4, $5)
               ON CONFLICT DO NOTHING"#,
        )
        .bind(&code)
        .bind(note.as_deref())
        .bind(max_uses)
        .bind(expires_at.clone())
        .bind(&actor_name)
        .execute(&s.db)
        .await;
    }
    crate::admin_customization::write_audit(
        &s,
        &actor,
        "admin.invite_create",
        "invite_codes",
        serde_json::json!({ "count": n, "max_uses": max_uses }),
    )
    .await;
    Redirect::to("/admin/invites?ok=create").into_response()
}

async fn revoke(
    State(s): State<Arc<AppState>>,
    headers: HeaderMap,
    Path(code): Path<String>,
) -> Response {
    let actor =
        match crate::admin_customization::require_actor(&headers, &s, "admin.invites.manage").await
        {
            Ok(v) => v,
            Err(resp) => return resp,
        };

    let _ = sqlx::query!(
        "UPDATE invite_codes SET revoked_at = now(), revoked_by = $2 WHERE code = $1",
        &code,
        &actor.name
    )
    .execute(&s.db)
    .await;
    crate::admin_customization::write_audit(
        &s,
        &actor,
        "admin.invite_revoke",
        &code,
        serde_json::json!({}),
    )
    .await;
    Redirect::to("/admin/invites?ok=revoke").into_response()
}

#[derive(Deserialize)]
pub struct AdminAuth {
    pub key: Option<String>,
}

async fn list_json(
    State(s): State<Arc<AppState>>,
    headers: HeaderMap,
    Query(q): Query<AdminAuth>,
) -> Result<axum::Json<serde_json::Value>, (StatusCode, String)> {
    let _actor = crate::admin_customization::require_actor_or_admin_key(
        &headers,
        &s,
        q.key.as_deref(),
        "admin.invites.read",
    )
    .await?;
    let rows = sqlx::query!(
        "SELECT code, note, max_uses, use_count, expires_at, revoked_at, created_at FROM invite_codes")
        .fetch_all(&s.db).await
        .map_err(crate::error::internal)?;
    Ok(axum::Json(serde_json::json!(rows
        .iter()
        .map(|r| serde_json::json!({
            "code": r.code, "note": r.note,
            "max_uses": r.max_uses, "use_count": r.use_count,
            "expires_at": r.expires_at.map(|t| t.timestamp()),
            "revoked": r.revoked_at.is_some(),
            "created_at": r.created_at.timestamp()
        }))
        .collect::<Vec<_>>())))
}

pub fn routes() -> Router<Arc<AppState>> {
    Router::new()
        .route("/admin/invites", get(list_page).post(create_submit))
        .route("/admin/invites/:code/revoke", post(revoke))
        .route("/api/admin/invites", get(list_json))
}
