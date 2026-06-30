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
use chrono::{DateTime, Utc};
use serde::Deserialize;
use sqlx::Row;
use std::sync::Arc;
use uuid::Uuid;

#[derive(Clone)]
pub struct AdminActor {
    pub name: String,
    pub role: String,
    pub display_name: String,
}

impl AdminActor {
    pub fn bootstrap() -> Self {
        Self {
            name: "bootstrap-owner".into(),
            role: "owner".into(),
            display_name: "Bootstrap Owner".into(),
        }
    }
}

#[derive(Clone)]
pub struct ConfigEntryVm {
    pub key: String,
    pub category: String,
    pub label: String,
    pub description: String,
    pub value_type: String,
    pub value_text: String,
    pub enabled: bool,
    pub expose_to_client: bool,
    pub updated_by: String,
    pub updated_at: String,
}

#[derive(Clone)]
pub struct OperatorVm {
    pub id: String,
    pub user_id: String,
    pub username: String,
    pub display_name: String,
    pub operator_role: String,
    pub role_label: String,
    pub enabled: bool,
    pub note: String,
    pub updated_at: String,
    pub last_login_at: String,
}

#[derive(Clone)]
pub struct RevisionVm {
    pub id: i64,
    pub config_key: String,
    pub old_value: String,
    pub new_value: String,
    pub actor: String,
    pub note: String,
    pub created_at: String,
}

#[derive(Clone)]
pub struct AuditRowVm {
    pub id: i64,
    pub time: String,
    pub action: String,
    pub actor: String,
    pub target: String,
    pub metadata: String,
}

#[derive(Template)]
#[template(path = "config_content.html")]
pub struct ConfigPage {
    pub title: String,
    pub subtitle: Option<String>,
    pub notice: Option<ui::AdminNotice>,
    pub host: &'static str,
    pub route: &'static str,
    pub entries: Vec<ConfigEntryVm>,
    pub revisions: Vec<RevisionVm>,
}

#[derive(Template)]
#[template(path = "operators_content.html")]
pub struct OperatorsPage {
    pub title: String,
    pub subtitle: Option<String>,
    pub notice: Option<ui::AdminNotice>,
    pub host: &'static str,
    pub route: &'static str,
    pub operators: Vec<OperatorVm>,
}

#[derive(Template)]
#[template(path = "audit_content.html")]
pub struct AuditPage {
    pub title: String,
    pub subtitle: Option<String>,
    pub notice: Option<ui::AdminNotice>,
    pub host: &'static str,
    pub route: &'static str,
    pub rows: Vec<AuditRowVm>,
}

#[derive(Deserialize, Default)]
pub struct AdminNoticeQuery {
    pub ok: Option<String>,
    pub err: Option<String>,
}

#[derive(Deserialize)]
pub struct ConfigForm {
    pub key: String,
    pub category: String,
    pub label: String,
    pub description: Option<String>,
    pub value_type: String,
    pub value: String,
    pub enabled: Option<String>,
    pub expose_to_client: Option<String>,
    pub note: Option<String>,
}

#[derive(Deserialize)]
pub struct OperatorForm {
    pub user_id: String,
    pub username: String,
    pub display_name: String,
    pub operator_role: String,
    pub enabled: Option<String>,
    pub note: Option<String>,
}

use crate::error::internal_msg as internal;

fn fmt_ts(ts: Option<DateTime<Utc>>) -> String {
    ts.map(|t| t.format("%Y-%m-%d %H:%M:%S").to_string())
        .unwrap_or_else(|| "-".into())
}

fn role_label(role: &str) -> &'static str {
    match role {
        "owner" => "Owner",
        "admin" => "Admin",
        "operator" => "Operator",
        "auditor" => "Auditor",
        _ => "Unknown",
    }
}

fn value_text(v: serde_json::Value) -> String {
    match v {
        serde_json::Value::String(s) => s,
        other => other.to_string(),
    }
}

fn pretty_json(v: Option<serde_json::Value>) -> String {
    v.map(|value| value_text(value))
        .unwrap_or_else(|| "-".into())
}

fn notice(q: &AdminNoticeQuery) -> Option<ui::AdminNotice> {
    match (q.ok.as_deref(), q.err.as_deref()) {
        (Some("config"), _) => Some(ui::AdminNotice::success("配置已保存")),
        (Some("rollback"), _) => Some(ui::AdminNotice::success("配置已回滚")),
        (Some("operator"), _) => Some(ui::AdminNotice::success("后台操作者已保存")),
        (_, Some("bad_config")) => Some(ui::AdminNotice::warning("配置参数无效")),
        (_, Some("bad_operator")) => Some(ui::AdminNotice::warning("操作者参数无效")),
        (_, Some("conflict")) => Some(ui::AdminNotice::warning("操作者账号或绑定用户与现有记录冲突")),
        (_, Some("permission")) => Some(ui::AdminNotice::warning("当前操作者没有权限")),
        (_, Some("failed")) => Some(ui::AdminNotice::error("操作失败，请检查服务日志")),
        _ => None,
    }
}

pub(crate) fn can(actor: &AdminActor, permission: &str) -> bool {
    match actor.role.as_str() {
        "owner" => true,
        "admin" => !matches!(permission, "admin.operators.manage"),
        "operator" => matches!(
            permission,
            "admin.config.read"
                | "admin.config.write"
                | "admin.channels.read"
                | "admin.channels.manage"
                | "admin.chat.read"
                | "admin.chat.send"
                | "admin.users.read"
                | "admin.market.read"
                | "admin.invites.read"
                | "admin.invites.manage"
                | "admin.rebind.read"
                | "admin.rebind.manage"
                | "admin.tickets.read"
                | "admin.tickets.manage"
                | "admin.announcements.read"
                | "admin.announcements.manage"
        ),
        "auditor" => matches!(
            permission,
            "admin.audit.read"
                | "admin.config.read"
                | "admin.channels.read"
                | "admin.chat.read"
                | "admin.users.read"
                | "admin.invites.read"
                | "admin.rebind.read"
                | "admin.tickets.read"
                | "admin.announcements.read"
        ),
        _ => false,
    }
}

pub(crate) async fn actor_from_session(
    headers: &HeaderMap,
    state: &AppState,
) -> Option<AdminActor> {
    let session = crate::admin::admin_session(headers, state).await?;
    Some(AdminActor {
        name: session.actor_name(),
        role: session.role,
        display_name: session.display_name,
    })
}

pub(crate) async fn require_actor(
    headers: &HeaderMap,
    state: &AppState,
    permission: &str,
) -> Result<AdminActor, Response> {
    let Some(actor) = actor_from_session(headers, state).await else {
        return Err(crate::admin::admin_login_redirect());
    };
    if can(&actor, permission) {
        Ok(actor)
    } else {
        Err(Redirect::to("/admin?err=permission").into_response())
    }
}

pub(crate) async fn require_actor_or_admin_key(
    headers: &HeaderMap,
    state: &AppState,
    key: Option<&str>,
    permission: &str,
) -> Result<AdminActor, (StatusCode, String)> {
    if let Some(actor) = actor_from_session(headers, state).await {
        if can(&actor, permission) {
            return Ok(actor);
        }
        if key == Some(state.cfg.admin_password.as_str()) {
            return Ok(AdminActor::bootstrap());
        }
        return Err((StatusCode::FORBIDDEN, "permission denied".into()));
    }
    if key == Some(state.cfg.admin_password.as_str()) {
        return Ok(AdminActor::bootstrap());
    }
    Err((StatusCode::UNAUTHORIZED, "admin key invalid".into()))
}

pub(crate) async fn write_audit(
    state: &AppState,
    actor: &AdminActor,
    action: &str,
    target: &str,
    metadata: serde_json::Value,
) {
    let _ = sqlx::query(
        "INSERT INTO audit_log (actor, action, target, metadata) VALUES ($1, $2, $3, $4)",
    )
    .bind(&actor.name)
    .bind(action)
    .bind(target)
    .bind(metadata)
    .execute(&state.db)
    .await;
    crate::audit::event(&actor.name, action, target);
}

fn clean_slug_like(input: &str, max: usize) -> String {
    input
        .trim()
        .to_ascii_lowercase()
        .chars()
        .filter(|c| c.is_ascii_alphanumeric() || matches!(c, '_' | '-' | '.'))
        .take(max)
        .collect()
}

fn parse_config_value(value_type: &str, raw: &str) -> Result<serde_json::Value, String> {
    let trimmed = raw.trim();
    match value_type {
        "string" => Ok(serde_json::json!(trimmed)),
        "number" => {
            let n = trimmed
                .parse::<i64>()
                .map_err(|_| "number expected".to_string())?;
            Ok(serde_json::json!(n))
        }
        "bool" => match trimmed.to_ascii_lowercase().as_str() {
            "true" | "1" | "yes" | "on" => Ok(serde_json::json!(true)),
            "false" | "0" | "no" | "off" => Ok(serde_json::json!(false)),
            _ => Err("bool expected".into()),
        },
        "json" => serde_json::from_str(trimmed).map_err(internal),
        _ => Err("bad value type".into()),
    }
}

async fn load_configs(state: &AppState) -> Result<Vec<ConfigEntryVm>, String> {
    let rows = sqlx::query(
        r#"SELECT key, category, label, description, value_type, value, enabled,
                  expose_to_client, updated_by, updated_at
           FROM app_config_entries
           ORDER BY category, key"#,
    )
    .fetch_all(&state.db)
    .await
    .map_err(internal)?;

    rows.into_iter()
        .map(|r| {
            Ok(ConfigEntryVm {
                key: r.try_get("key").map_err(internal)?,
                category: r.try_get("category").map_err(internal)?,
                label: r.try_get("label").map_err(internal)?,
                description: r.try_get("description").map_err(internal)?,
                value_type: r.try_get("value_type").map_err(internal)?,
                value_text: value_text(r.try_get("value").map_err(internal)?),
                enabled: r.try_get("enabled").map_err(internal)?,
                expose_to_client: r.try_get("expose_to_client").map_err(internal)?,
                updated_by: r
                    .try_get::<Option<String>, _>("updated_by")
                    .map_err(internal)?
                    .unwrap_or_else(|| "-".into()),
                updated_at: fmt_ts(Some(r.try_get("updated_at").map_err(internal)?)),
            })
        })
        .collect()
}

async fn load_revisions(state: &AppState) -> Result<Vec<RevisionVm>, String> {
    let rows = sqlx::query(
        r#"SELECT id, config_key, old_value, new_value, actor, note, created_at
           FROM app_config_revisions
           ORDER BY created_at DESC, id DESC
           LIMIT 50"#,
    )
    .fetch_all(&state.db)
    .await
    .map_err(internal)?;

    rows.into_iter()
        .map(|r| {
            Ok(RevisionVm {
                id: r.try_get("id").map_err(internal)?,
                config_key: r.try_get("config_key").map_err(internal)?,
                old_value: pretty_json(r.try_get("old_value").map_err(internal)?),
                new_value: value_text(r.try_get("new_value").map_err(internal)?),
                actor: r.try_get("actor").map_err(internal)?,
                note: r
                    .try_get::<Option<String>, _>("note")
                    .map_err(internal)?
                    .unwrap_or_default(),
                created_at: fmt_ts(Some(r.try_get("created_at").map_err(internal)?)),
            })
        })
        .collect()
}

async fn config_page(
    State(s): State<Arc<AppState>>,
    headers: HeaderMap,
    Query(q): Query<AdminNoticeQuery>,
) -> Response {
    if let Err(resp) = require_actor(&headers, &s, "admin.config.read").await {
        return resp;
    }
    let entries = load_configs(&s).await.unwrap_or_default();
    let revisions = load_revisions(&s).await.unwrap_or_default();
    ui::render(&ConfigPage {
        title: "配置中心".into(),
        subtitle: Some("数据库驱动的运营配置，保存后立即对 API 生效".into()),
        notice: notice(&q),
        host: ui::host(),
        route: ui::ROUTE_CONFIG,
        entries,
        revisions,
    })
    .into_response()
}

async fn save_config(
    State(s): State<Arc<AppState>>,
    headers: HeaderMap,
    Form(form): Form<ConfigForm>,
) -> Response {
    let actor = match require_actor(&headers, &s, "admin.config.write").await {
        Ok(v) => v,
        Err(resp) => return resp,
    };
    let key = clean_slug_like(&form.key, 96);
    let category = clean_slug_like(&form.category, 40);
    let label = form.label.trim().chars().take(80).collect::<String>();
    let description = form
        .description
        .as_deref()
        .unwrap_or_default()
        .trim()
        .chars()
        .take(240)
        .collect::<String>();
    if key.len() < 3
        || category.is_empty()
        || label.is_empty()
        || !matches!(
            form.value_type.as_str(),
            "string" | "number" | "bool" | "json"
        )
    {
        return Redirect::to("/admin/config?err=bad_config").into_response();
    }
    let value = match parse_config_value(&form.value_type, &form.value) {
        Ok(v) => v,
        Err(_) => return Redirect::to("/admin/config?err=bad_config").into_response(),
    };
    let enabled = form.enabled.is_some();
    let expose = form.expose_to_client.is_some();
    let note = form
        .note
        .as_deref()
        .unwrap_or_default()
        .trim()
        .chars()
        .take(240)
        .collect::<String>();

    let existing = sqlx::query(
        "SELECT value, enabled, expose_to_client FROM app_config_entries WHERE key = $1",
    )
    .bind(&key)
    .fetch_optional(&s.db)
    .await;
    let Ok(existing) = existing else {
        return Redirect::to("/admin/config?err=failed").into_response();
    };
    let old_value = existing
        .as_ref()
        .and_then(|r| r.try_get::<serde_json::Value, _>("value").ok());
    let old_enabled = existing
        .as_ref()
        .and_then(|r| r.try_get::<bool, _>("enabled").ok());
    let old_expose = existing
        .as_ref()
        .and_then(|r| r.try_get::<bool, _>("expose_to_client").ok());

    let saved = sqlx::query(
        r#"INSERT INTO app_config_entries
              (key, category, label, description, value_type, value, enabled,
               expose_to_client, updated_by, updated_at)
           VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, now())
           ON CONFLICT (key) DO UPDATE
             SET category = EXCLUDED.category,
                 label = EXCLUDED.label,
                 description = EXCLUDED.description,
                 value_type = EXCLUDED.value_type,
                 value = EXCLUDED.value,
                 enabled = EXCLUDED.enabled,
                 expose_to_client = EXCLUDED.expose_to_client,
                 updated_by = EXCLUDED.updated_by,
                 updated_at = now()"#,
    )
    .bind(&key)
    .bind(&category)
    .bind(&label)
    .bind(&description)
    .bind(&form.value_type)
    .bind(&value)
    .bind(enabled)
    .bind(expose)
    .bind(&actor.name)
    .execute(&s.db)
    .await;
    if saved.is_err() {
        return Redirect::to("/admin/config?err=failed").into_response();
    }

    let _ = sqlx::query(
        r#"INSERT INTO app_config_revisions
              (config_key, old_value, new_value, old_enabled, new_enabled,
               old_expose_to_client, new_expose_to_client, actor, note)
           VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9)"#,
    )
    .bind(&key)
    .bind(old_value)
    .bind(&value)
    .bind(old_enabled)
    .bind(enabled)
    .bind(old_expose)
    .bind(expose)
    .bind(&actor.name)
    .bind(if note.is_empty() {
        None
    } else {
        Some(note.clone())
    })
    .execute(&s.db)
    .await;

    write_audit(
        &s,
        &actor,
        "admin.config_save",
        &key,
        serde_json::json!({ "category": category, "value_type": form.value_type, "note": note }),
    )
    .await;
    Redirect::to("/admin/config?ok=config").into_response()
}

async fn rollback_config(
    State(s): State<Arc<AppState>>,
    headers: HeaderMap,
    Path(id): Path<i64>,
) -> Response {
    let actor = match require_actor(&headers, &s, "admin.config.write").await {
        Ok(v) => v,
        Err(resp) => return resp,
    };
    let row = sqlx::query(
        r#"SELECT config_key, old_value, old_enabled, old_expose_to_client
           FROM app_config_revisions WHERE id = $1"#,
    )
    .bind(id)
    .fetch_optional(&s.db)
    .await;
    let Ok(Some(row)) = row else {
        return Redirect::to("/admin/config?err=failed").into_response();
    };
    let key: String = row.try_get("config_key").unwrap_or_default();
    let Some(old_value) = row
        .try_get::<Option<serde_json::Value>, _>("old_value")
        .ok()
        .flatten()
    else {
        return Redirect::to("/admin/config?err=failed").into_response();
    };
    let old_enabled = row
        .try_get::<Option<bool>, _>("old_enabled")
        .ok()
        .flatten()
        .unwrap_or(true);
    let old_expose = row
        .try_get::<Option<bool>, _>("old_expose_to_client")
        .ok()
        .flatten()
        .unwrap_or(false);
    let current = sqlx::query(
        "SELECT value, enabled, expose_to_client FROM app_config_entries WHERE key = $1",
    )
    .bind(&key)
    .fetch_optional(&s.db)
    .await
    .ok()
    .flatten();
    let current_value = current
        .as_ref()
        .and_then(|r| r.try_get::<serde_json::Value, _>("value").ok());
    let current_enabled = current
        .as_ref()
        .and_then(|r| r.try_get::<bool, _>("enabled").ok());
    let current_expose = current
        .as_ref()
        .and_then(|r| r.try_get::<bool, _>("expose_to_client").ok());

    let updated = sqlx::query(
        r#"UPDATE app_config_entries
           SET value = $2, enabled = $3, expose_to_client = $4, updated_by = $5, updated_at = now()
           WHERE key = $1"#,
    )
    .bind(&key)
    .bind(&old_value)
    .bind(old_enabled)
    .bind(old_expose)
    .bind(&actor.name)
    .execute(&s.db)
    .await;
    if updated.is_err() {
        return Redirect::to("/admin/config?err=failed").into_response();
    }
    let _ = sqlx::query(
        r#"INSERT INTO app_config_revisions
              (config_key, old_value, new_value, old_enabled, new_enabled,
               old_expose_to_client, new_expose_to_client, actor, note)
           VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9)"#,
    )
    .bind(&key)
    .bind(current_value)
    .bind(&old_value)
    .bind(current_enabled)
    .bind(old_enabled)
    .bind(current_expose)
    .bind(old_expose)
    .bind(&actor.name)
    .bind(format!("rollback revision {id}"))
    .execute(&s.db)
    .await;
    write_audit(
        &s,
        &actor,
        "admin.config_rollback",
        &key,
        serde_json::json!({ "revision_id": id }),
    )
    .await;
    Redirect::to("/admin/config?ok=rollback").into_response()
}

async fn load_operators(state: &AppState) -> Result<Vec<OperatorVm>, String> {
    let rows = sqlx::query(
        r#"SELECT id, user_id, username, display_name, operator_role, enabled, note,
                  updated_at, last_login_at
           FROM admin_operators
           ORDER BY
             CASE operator_role
               WHEN 'owner' THEN 1 WHEN 'admin' THEN 2 WHEN 'operator' THEN 3 ELSE 4
             END,
             username"#,
    )
    .fetch_all(&state.db)
    .await
    .map_err(internal)?;
    rows.into_iter()
        .map(|r| {
            let operator_role: String = r.try_get("operator_role").map_err(internal)?;
            Ok(OperatorVm {
                id: r.try_get::<Uuid, _>("id").map_err(internal)?.to_string(),
                user_id: r
                    .try_get::<Option<Uuid>, _>("user_id")
                    .map_err(internal)?
                    .map(|v| v.to_string())
                    .unwrap_or_else(|| "-".into()),
                username: r.try_get("username").map_err(internal)?,
                display_name: r.try_get("display_name").map_err(internal)?,
                role_label: role_label(&operator_role).into(),
                operator_role,
                enabled: r.try_get("enabled").map_err(internal)?,
                note: r
                    .try_get::<Option<String>, _>("note")
                    .map_err(internal)?
                    .unwrap_or_default(),
                updated_at: fmt_ts(Some(r.try_get("updated_at").map_err(internal)?)),
                last_login_at: fmt_ts(r.try_get("last_login_at").map_err(internal)?),
            })
        })
        .collect()
}

async fn operators_page(
    State(s): State<Arc<AppState>>,
    headers: HeaderMap,
    Query(q): Query<AdminNoticeQuery>,
) -> Response {
    if let Err(resp) = require_actor(&headers, &s, "admin.operators.manage").await {
        return resp;
    }
    let operators = load_operators(&s).await.unwrap_or_default();
    ui::render(&OperatorsPage {
        title: "后台权限".into(),
        subtitle: Some("后台操作者分级管理；当前 admin 密码登录视为 bootstrap owner".into()),
        notice: notice(&q),
        host: ui::host(),
        route: ui::ROUTE_OPERATORS,
        operators,
    })
    .into_response()
}

async fn save_operator(
    State(s): State<Arc<AppState>>,
    headers: HeaderMap,
    Form(form): Form<OperatorForm>,
) -> Response {
    let actor = match require_actor(&headers, &s, "admin.operators.manage").await {
        Ok(v) => v,
        Err(resp) => return resp,
    };
    let user_key = form.user_id.trim();
    if user_key.is_empty() {
        return Redirect::to("/admin/operators?err=bad_operator").into_response();
    }
    let username = clean_slug_like(&form.username, 48);
    let display_name = form
        .display_name
        .trim()
        .chars()
        .take(80)
        .collect::<String>();
    if !matches!(
        form.operator_role.as_str(),
        "owner" | "admin" | "operator" | "auditor"
    ) {
        return Redirect::to("/admin/operators?err=bad_operator").into_response();
    }
    let user = if let Ok(user_id) = Uuid::parse_str(user_key) {
        sqlx::query("SELECT id, username FROM users WHERE id = $1")
            .bind(user_id)
            .fetch_optional(&s.db)
            .await
    } else {
        sqlx::query("SELECT id, username FROM users WHERE username = $1 OR uid = $1")
            .bind(user_key)
            .fetch_optional(&s.db)
            .await
    };
    let Ok(Some(user)) = user else {
        return Redirect::to("/admin/operators?err=bad_operator").into_response();
    };
    let user_id = match user.try_get::<Uuid, _>("id") {
        Ok(v) => v,
        Err(_) => return Redirect::to("/admin/operators?err=failed").into_response(),
    };
    let linked_username = user
        .try_get::<Option<String>, _>("username")
        .ok()
        .flatten()
        .unwrap_or_default();
    let canonical_username = if username.is_empty() {
        clean_slug_like(&linked_username, 48)
    } else {
        username
    };
    if canonical_username.len() < 3 {
        return Redirect::to("/admin/operators?err=bad_operator").into_response();
    }
    let note = form
        .note
        .as_deref()
        .unwrap_or_default()
        .trim()
        .chars()
        .take(240)
        .collect::<String>();
    let enabled = form.enabled.is_some();
    let existing = sqlx::query(
        r#"SELECT id
           FROM admin_operators
           WHERE username = $1 OR user_id = $2
           ORDER BY created_at ASC"#,
    )
    .bind(&canonical_username)
    .bind(user_id)
    .fetch_all(&s.db)
    .await;
    let Ok(existing) = existing else {
        return Redirect::to("/admin/operators?err=failed").into_response();
    };
    let mut existing_id = None;
    for row in existing {
        let Ok(id) = row.try_get::<Uuid, _>("id") else {
            return Redirect::to("/admin/operators?err=failed").into_response();
        };
        if let Some(prev) = existing_id {
            if prev != id {
                return Redirect::to("/admin/operators?err=conflict").into_response();
            }
        } else {
            existing_id = Some(id);
        }
    }
    let note_value = if note.is_empty() {
        None
    } else {
        Some(note.clone())
    };
    let res = if let Some(existing_id) = existing_id {
        sqlx::query(
            r#"UPDATE admin_operators
               SET user_id = $2,
                   username = $3,
                   display_name = $4,
                   operator_role = $5,
                   enabled = $6,
                   note = $7,
                   updated_at = now()
               WHERE id = $1"#,
        )
        .bind(existing_id)
        .bind(user_id)
        .bind(&canonical_username)
        .bind(&display_name)
        .bind(&form.operator_role)
        .bind(enabled)
        .bind(note_value)
        .execute(&s.db)
        .await
    } else {
        sqlx::query(
            r#"INSERT INTO admin_operators
                  (user_id, username, display_name, operator_role, enabled, note, updated_at)
               VALUES ($1, $2, $3, $4, $5, $6, now())"#,
        )
        .bind(user_id)
        .bind(&canonical_username)
        .bind(&display_name)
        .bind(&form.operator_role)
        .bind(enabled)
        .bind(note_value)
        .execute(&s.db)
        .await
    };
    if res.is_err() {
        return Redirect::to("/admin/operators?err=failed").into_response();
    }
    write_audit(
        &s,
        &actor,
        "admin.operator_save",
        &canonical_username,
        serde_json::json!({ "role": form.operator_role, "enabled": enabled, "user_id": user_id }),
    )
    .await;
    Redirect::to("/admin/operators?ok=operator").into_response()
}

async fn audit_page(State(s): State<Arc<AppState>>, headers: HeaderMap) -> Response {
    if let Err(resp) = require_actor(&headers, &s, "admin.audit.read").await {
        return resp;
    }
    let rows = sqlx::query(
        r#"SELECT id, occurred_at, action, actor, target, metadata
           FROM audit_log
           ORDER BY occurred_at DESC, id DESC
           LIMIT 200"#,
    )
    .fetch_all(&s.db)
    .await
    .unwrap_or_default()
    .into_iter()
    .map(|r| AuditRowVm {
        id: r.try_get("id").unwrap_or_default(),
        time: fmt_ts(Some(
            r.try_get("occurred_at").unwrap_or_else(|_| Utc::now()),
        )),
        action: r.try_get("action").unwrap_or_default(),
        actor: r
            .try_get::<Option<String>, _>("actor")
            .ok()
            .flatten()
            .unwrap_or_else(|| "-".into()),
        target: r
            .try_get::<Option<String>, _>("target")
            .ok()
            .flatten()
            .unwrap_or_else(|| "-".into()),
        metadata: r
            .try_get::<Option<serde_json::Value>, _>("metadata")
            .ok()
            .flatten()
            .map(value_text)
            .unwrap_or_else(|| "-".into()),
    })
    .collect();
    ui::render(&AuditPage {
        title: "审计日志".into(),
        subtitle: Some("后台操作、系统动作和配置变更记录".into()),
        notice: None,
        host: ui::host(),
        route: ui::ROUTE_AUDIT,
        rows,
    })
    .into_response()
}

pub fn routes() -> Router<Arc<AppState>> {
    Router::new()
        .route("/admin/config", get(config_page).post(save_config))
        .route(
            "/admin/config/revisions/:id/rollback",
            post(rollback_config),
        )
        .route("/admin/operators", get(operators_page).post(save_operator))
        .route("/admin/audit", get(audit_page))
}
