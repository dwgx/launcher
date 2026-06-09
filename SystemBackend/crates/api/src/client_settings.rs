use crate::media::auth_user;
use crate::state::AppState;
use axum::{
    extract::{Json, Query, State},
    http::StatusCode,
};
use chrono::{DateTime, Utc};
use serde::{Deserialize, Serialize};
use sqlx::Row;
use std::sync::Arc;

fn internal<E: std::fmt::Display>(e: E) -> (StatusCode, String) {
    (StatusCode::INTERNAL_SERVER_ERROR, e.to_string())
}

fn valid_key(key: &str) -> bool {
    let len = key.chars().count();
    (1..=64).contains(&len)
        && key.chars().all(|c| c.is_ascii_alphanumeric() || c == '_' || c == '-' || c == '.')
}

#[derive(Deserialize)]
pub struct SettingsQ {
    pub session_token: String,
    pub scope: Option<String>,
}

#[derive(Serialize)]
pub struct ClientSettingOut {
    pub scope: String,
    pub key: String,
    pub value: serde_json::Value,
    pub updated_at: i64,
}

#[derive(Deserialize)]
pub struct UpsertSettingsReq {
    pub session_token: String,
    pub scope: Option<String>,
    pub items: Vec<SettingIn>,
}

#[derive(Deserialize)]
pub struct SettingIn {
    pub key: String,
    pub value: serde_json::Value,
}

fn normalize_scope(scope: Option<String>) -> String {
    let s = scope.unwrap_or_else(|| "desktop".into());
    let s = s.trim();
    if s.is_empty() || s.chars().count() > 32 {
        "desktop".into()
    } else {
        s.to_string()
    }
}

pub async fn get_settings(
    State(s): State<Arc<AppState>>,
    Query(q): Query<SettingsQ>,
) -> Result<Json<Vec<ClientSettingOut>>, (StatusCode, String)> {
    let me = auth_user(&s, &q.session_token).await?;
    let scope = normalize_scope(q.scope);
    let rows = sqlx::query(
        r#"SELECT scope, key, value, updated_at
           FROM user_client_settings
           WHERE user_id = $1 AND scope = $2
           ORDER BY key"#)
        .bind(me)
        .bind(scope)
        .fetch_all(&s.db).await.map_err(internal)?;
    let mut out = Vec::with_capacity(rows.len());
    for r in rows {
        out.push(ClientSettingOut {
            scope: r.try_get("scope").map_err(internal)?,
            key: r.try_get("key").map_err(internal)?,
            value: r.try_get("value").map_err(internal)?,
            updated_at: r.try_get::<DateTime<Utc>, _>("updated_at").map_err(internal)?.timestamp(),
        });
    }
    Ok(Json(out))
}

pub async fn upsert_settings(
    State(s): State<Arc<AppState>>,
    Json(req): Json<UpsertSettingsReq>,
) -> Result<Json<Vec<ClientSettingOut>>, (StatusCode, String)> {
    let me = auth_user(&s, &req.session_token).await?;
    let scope = normalize_scope(req.scope);
    if req.items.len() > 64 {
        return Err((StatusCode::BAD_REQUEST, "too many settings".into()));
    }

    for item in req.items {
        if !valid_key(&item.key) {
            return Err((StatusCode::BAD_REQUEST, "bad setting key".into()));
        }
        let encoded = serde_json::to_vec(&item.value).map_err(internal)?;
        if encoded.len() > 16 * 1024 {
            return Err((StatusCode::BAD_REQUEST, "setting too large".into()));
        }
        sqlx::query(
            r#"INSERT INTO user_client_settings (user_id, scope, key, value, updated_at)
               VALUES ($1, $2, $3, $4, now())
               ON CONFLICT (user_id, scope, key)
               DO UPDATE SET value = EXCLUDED.value, updated_at = now()"#)
            .bind(me)
            .bind(&scope)
            .bind(item.key)
            .bind(item.value)
            .execute(&s.db).await.map_err(internal)?;
    }

    get_settings(State(s), Query(SettingsQ {
        session_token: req.session_token,
        scope: Some(scope),
    })).await
}
