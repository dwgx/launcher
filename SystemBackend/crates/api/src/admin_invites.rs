// 邀请码 admin SSR + JSON API。

use crate::state::AppState;
use crate::ui;
use axum::{
    extract::{State, Form, Path, Query},
    http::StatusCode,
    response::{IntoResponse, Redirect, Html},
    Router,
    routing::{get, post},
};
use serde::Deserialize;
use std::sync::Arc;
use rand::{Rng, rngs::OsRng};
use askama::Template;

const ALPH: &[u8; 32] = b"23456789ABCDEFGHJKLMNPQRSTUVWXYZ";

fn gen_code(len: usize) -> String {
    let mut rng = OsRng;
    (0..len).map(|_| ALPH[rng.gen_range(0..ALPH.len())] as char).collect()
}

pub struct InviteVm {
    pub code: String, pub note: String, pub uses: String,
    pub status: String, pub created: String,
}

#[derive(Template)]
#[template(path = "invites_content.html")]
pub struct InvitesPage {
    pub title:    String,
    pub subtitle: Option<String>,
    pub host:     &'static str,
    pub route:    &'static str,
    pub items:    Vec<InviteVm>,
}

async fn list_page(State(s): State<Arc<AppState>>) -> Html<String> {
    let rows = sqlx::query!(
        r#"SELECT code, note, max_uses, use_count, expires_at, revoked_at, created_at
           FROM invite_codes ORDER BY created_at DESC LIMIT 200"#)
        .fetch_all(&s.db).await.unwrap_or_default();
    let items = rows.into_iter().map(|r| {
        let status = if r.revoked_at.is_some() { "revoked" }
            else if r.expires_at.map(|e| e < chrono::Utc::now()).unwrap_or(false) { "expired" }
            else if r.use_count >= r.max_uses { "exhausted" }
            else { "active" };
        InviteVm {
            code: r.code,
            note: r.note.unwrap_or_default(),
            uses: format!("{}/{}", r.use_count, r.max_uses),
            status: status.into(),
            created: r.created_at.format("%Y-%m-%d %H:%M").to_string(),
        }
    }).collect();
    ui::render(&InvitesPage {
        title: "邀请码".into(),
        subtitle: Some("注册必须带一个有效邀请码".into()),
        host: ui::host(),
        route: ui::ROUTE_INVITES,
        items,
    })
}

// HTML form 留空字段会发 `field=`（空字符串）；serde 默认 Option<i32>::deserialize 把它
// 当成 Some("") 然后 i32::from_str("") 报错。统一走 empty_str_as_none。
#[derive(Deserialize)]
pub struct CreateForm {
    pub note:     Option<String>,
    #[serde(default, deserialize_with = "launcher_shared::formhelp::empty_str_as_none")]
    pub max_uses: Option<i32>,
    #[serde(default, deserialize_with = "launcher_shared::formhelp::empty_str_as_none")]
    pub days:     Option<i64>,
    #[serde(default, deserialize_with = "launcher_shared::formhelp::empty_str_as_none")]
    pub count:    Option<i32>,
}

async fn create_submit(
    State(s): State<Arc<AppState>>,
    Form(form): Form<CreateForm>,
) -> impl IntoResponse {
    let n = form.count.unwrap_or(1).clamp(1, 50);
    let max_uses = form.max_uses.unwrap_or(1).clamp(1, 1000);
    let expires_at = form.days.map(|d| chrono::Utc::now() + chrono::Duration::days(d));
    for _ in 0..n {
        let code = gen_code(8);
        let _ = sqlx::query!(
            r#"INSERT INTO invite_codes (code, note, max_uses, expires_at, created_by)
               VALUES ($1, $2, $3, $4, 'admin')
               ON CONFLICT DO NOTHING"#,
            code, form.note, max_uses, expires_at)
            .execute(&s.db).await;
    }
    Redirect::to("/admin/invites")
}

async fn revoke(
    State(s): State<Arc<AppState>>,
    Path(code): Path<String>,
) -> impl IntoResponse {
    let _ = sqlx::query!(
        "UPDATE invite_codes SET revoked_at = now(), revoked_by = 'admin' WHERE code = $1",
        code).execute(&s.db).await;
    Redirect::to("/admin/invites")
}

#[derive(Deserialize)]
pub struct AdminAuth { pub key: Option<String> }

async fn list_json(
    State(s): State<Arc<AppState>>,
    Query(q): Query<AdminAuth>,
) -> Result<axum::Json<serde_json::Value>, (StatusCode, String)> {
    if q.key.as_deref() != Some(s.cfg.admin_password.as_str()) {
        return Err((StatusCode::UNAUTHORIZED, "admin key invalid".into()));
    }
    let rows = sqlx::query!(
        "SELECT code, note, max_uses, use_count, expires_at, revoked_at, created_at FROM invite_codes")
        .fetch_all(&s.db).await
        .map_err(|e| (StatusCode::INTERNAL_SERVER_ERROR, e.to_string()))?;
    Ok(axum::Json(serde_json::json!(rows.iter().map(|r| serde_json::json!({
        "code": r.code, "note": r.note,
        "max_uses": r.max_uses, "use_count": r.use_count,
        "expires_at": r.expires_at.map(|t| t.timestamp()),
        "revoked": r.revoked_at.is_some(),
        "created_at": r.created_at.timestamp()
    })).collect::<Vec<_>>())))
}

pub fn routes() -> Router<Arc<AppState>> {
    Router::new()
        .route("/admin/invites",              get(list_page).post(create_submit))
        .route("/admin/invites/:code/revoke", post(revoke))
        .route("/api/admin/invites",          get(list_json))
}
