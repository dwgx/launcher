// Admin SSR：登录、dashboard。
// users / invites / rebind 各管自己的 page struct（在对应文件）。
// 全部继承 templates/base.html 的 layout（Tailwind + DaisyUI luxury 主题）。

use crate::state::AppState;
use crate::ui;
use askama::Template;
use axum::{
    extract::{Form, State},
    http::{header, HeaderMap, HeaderValue},
    response::{Html, IntoResponse, Redirect, Response},
    routing::get,
    Router,
};
use chrono::Utc;
use hmac::{Hmac, Mac};
use launcher_shared::hashing;
use serde::Deserialize;
use sha2::Sha256;
use sqlx::Row;
use std::sync::Arc;
use std::time;
use uuid::Uuid;

const ADMIN_COOKIE: &str = "launcher_admin";
const ADMIN_COOKIE_MAX_AGE: i64 = 60 * 60 * 12;
const ADMIN_LOGIN_WINDOW: time::Duration = time::Duration::from_secs(15 * 60);
const ADMIN_LOGIN_COOLDOWN: time::Duration = time::Duration::from_secs(15 * 60);
const ADMIN_LOGIN_MAX_FAILURES: u32 = 5;
type HmacSha256 = Hmac<Sha256>;

#[derive(Clone)]
pub(crate) struct AdminSession {
    pub username: String,
    pub user_id: Option<Uuid>,
    pub display_name: String,
    pub role: String,
    pub bootstrap: bool,
}

impl AdminSession {
    pub(crate) fn actor_name(&self) -> String {
        if self.bootstrap {
            "bootstrap-owner".into()
        } else {
            format!("operator:{}", self.username)
        }
    }
}

fn admin_login_key(username: &str) -> String {
    let user = username.trim().to_ascii_lowercase();
    if user.is_empty() {
        "admin:bootstrap".into()
    } else {
        format!("admin:{user}")
    }
}

fn check_admin_login_limit(state: &AppState, key: &str) -> Result<(), time::Duration> {
    let mut map = state.login_attempts.lock().unwrap();
    let entry = map
        .entry(key.to_string())
        .or_insert_with(|| crate::state::LoginAttemptEntry {
            count: 0,
            first_at: time::Instant::now(),
        });
    if entry.first_at.elapsed() > ADMIN_LOGIN_WINDOW {
        entry.count = 0;
        entry.first_at = time::Instant::now();
    }
    if entry.count >= ADMIN_LOGIN_MAX_FAILURES {
        return Err(ADMIN_LOGIN_COOLDOWN.saturating_sub(entry.first_at.elapsed()));
    }
    Ok(())
}

fn record_admin_login_failure(state: &AppState, key: &str) {
    if let Ok(mut map) = state.login_attempts.lock() {
        let entry = map
            .entry(key.to_string())
            .or_insert_with(|| crate::state::LoginAttemptEntry {
                count: 0,
                first_at: time::Instant::now(),
            });
        if entry.first_at.elapsed() > ADMIN_LOGIN_WINDOW {
            entry.count = 0;
            entry.first_at = time::Instant::now();
        }
        entry.count += 1;
    }
}

fn clear_admin_login_limit(state: &AppState, key: &str) {
    if let Ok(mut map) = state.login_attempts.lock() {
        map.remove(key);
    }
}

fn admin_cookie_sig(
    state: &AppState,
    issued_at: i64,
    username: &str,
    user_id: &str,
    role: &str,
) -> String {
    let mut mac = HmacSha256::new_from_slice(state.cfg.admin_password.as_bytes())
        .expect("HMAC accepts any key length");
    mac.update(b"launcher.admin.session.v1:");
    mac.update(issued_at.to_string().as_bytes());
    mac.update(b":");
    mac.update(username.as_bytes());
    mac.update(b":");
    mac.update(user_id.as_bytes());
    mac.update(b":");
    mac.update(role.as_bytes());
    hex::encode(mac.finalize().into_bytes())
}

fn verify_admin_cookie_sig(
    state: &AppState,
    issued_at: i64,
    username: &str,
    user_id: &str,
    role: &str,
    sig_hex: &str,
) -> bool {
    let Ok(sig) = hex::decode(sig_hex) else {
        return false;
    };
    let mut mac = HmacSha256::new_from_slice(state.cfg.admin_password.as_bytes())
        .expect("HMAC accepts any key length");
    mac.update(b"launcher.admin.session.v1:");
    mac.update(issued_at.to_string().as_bytes());
    mac.update(b":");
    mac.update(username.as_bytes());
    mac.update(b":");
    mac.update(user_id.as_bytes());
    mac.update(b":");
    mac.update(role.as_bytes());
    mac.verify_slice(&sig).is_ok()
}

fn admin_cookie_value(
    state: &AppState,
    issued_at: i64,
    username: &str,
    user_id: Option<Uuid>,
    role: &str,
) -> String {
    let user_id = user_id
        .map(|v| v.to_string())
        .unwrap_or_else(|| "bootstrap".into());
    format!(
        "{}:{}:{}:{}:{}",
        issued_at,
        username,
        user_id,
        role,
        admin_cookie_sig(state, issued_at, username, &user_id, role)
    )
}

fn parse_admin_cookie_value(state: &AppState, value: &str) -> Option<AdminSession> {
    let mut parts = value.splitn(5, ':');
    let issued_at = parts.next()?.parse::<i64>().ok()?;
    let username = parts.next()?.to_string();
    let user_id_raw = parts.next()?.to_string();
    let role = parts.next()?.to_string();
    let sig = parts.next()?;
    if username.is_empty() || role.is_empty() {
        return None;
    }
    let bootstrap = username == "bootstrap" && user_id_raw == "bootstrap";
    let user_id = if bootstrap {
        None
    } else {
        Some(Uuid::parse_str(&user_id_raw).ok()?)
    };
    if !matches!(role.as_str(), "owner" | "admin" | "operator" | "auditor") {
        return None;
    }
    let now = Utc::now().timestamp();
    if issued_at > now || now.saturating_sub(issued_at) > ADMIN_COOKIE_MAX_AGE {
        return None;
    }
    if !verify_admin_cookie_sig(state, issued_at, &username, &user_id_raw, &role, sig) {
        return None;
    }
    Some(AdminSession {
        display_name: if username == "bootstrap" {
            "Bootstrap Owner".into()
        } else {
            username.clone()
        },
        bootstrap,
        username,
        user_id,
        role,
    })
}

pub(crate) async fn admin_session(headers: &HeaderMap, state: &AppState) -> Option<AdminSession> {
    let raw = headers.get(header::COOKIE).and_then(|v| v.to_str().ok())?;
    let session = raw.split(';').find_map(|part| {
        let part = part.trim();
        let (name, value) = part.split_once('=')?;
        if name != ADMIN_COOKIE {
            return None;
        }
        parse_admin_cookie_value(state, value)
    })?;
    if session.bootstrap {
        return Some(session);
    }
    let row = sqlx::query(
        r#"SELECT user_id, display_name, operator_role, enabled
           FROM admin_operators
           WHERE username = $1"#,
    )
    .bind(&session.username)
    .fetch_optional(&state.db)
    .await
    .ok()??;
    let enabled: bool = row.try_get("enabled").ok()?;
    if !enabled {
        return None;
    }
    let user_id: Uuid = row.try_get("user_id").ok()?;
    if session.user_id != Some(user_id) {
        return None;
    }
    let role: String = row.try_get("operator_role").ok()?;
    if !matches!(role.as_str(), "owner" | "admin" | "operator" | "auditor") {
        return None;
    }
    Some(AdminSession {
        display_name: row
            .try_get("display_name")
            .ok()
            .filter(|v: &String| !v.trim().is_empty())
            .unwrap_or_else(|| session.username.clone()),
        role,
        ..session
    })
}

pub(crate) fn admin_login_redirect() -> Response {
    Redirect::to("/admin/login").into_response()
}

fn set_admin_cookie(
    resp: &mut Response,
    state: &AppState,
    username: &str,
    user_id: Option<Uuid>,
    role: &str,
) {
    let secure = if state.cfg.tls_cert_path.is_some() {
        "; Secure"
    } else {
        ""
    };
    let cookie = format!(
        "{}={}; Path=/admin; Max-Age={}; HttpOnly; SameSite=Strict{}",
        ADMIN_COOKIE,
        admin_cookie_value(state, Utc::now().timestamp(), username, user_id, role),
        ADMIN_COOKIE_MAX_AGE,
        secure
    );
    if let Ok(value) = HeaderValue::from_str(&cookie) {
        resp.headers_mut().insert(header::SET_COOKIE, value);
    }
}

fn clear_admin_cookie(resp: &mut Response, state: &AppState) {
    let secure = if state.cfg.tls_cert_path.is_some() {
        "; Secure"
    } else {
        ""
    };
    let cookie = format!(
        "{}=; Path=/admin; Max-Age=0; HttpOnly; SameSite=Strict{}",
        ADMIN_COOKIE, secure
    );
    if let Ok(value) = HeaderValue::from_str(&cookie) {
        resp.headers_mut().insert(header::SET_COOKIE, value);
    }
}

// ---------------- login ----------------
#[derive(Template, Default)]
#[template(path = "login.html")]
pub struct LoginPage {
    pub error: bool,
    pub error_msg: String,
}

#[derive(Deserialize)]
pub struct AdminLogin {
    pub username: Option<String>,
    pub password: String,
}

async fn login_page() -> Html<String> {
    ui::render(&LoginPage::default())
}

async fn login_submit(
    State(s): State<Arc<AppState>>,
    Form(form): Form<AdminLogin>,
) -> axum::response::Response {
    let username = form
        .username
        .as_deref()
        .unwrap_or_default()
        .trim()
        .to_ascii_lowercase();
    let limit_key = admin_login_key(&username);
    if let Err(remaining) = check_admin_login_limit(&s, &limit_key) {
        let _ = sqlx::query(
            "INSERT INTO audit_log (actor, action, target, metadata) VALUES ($1, 'admin.login_locked', $2, $3)",
        )
        .bind(if username.is_empty() { "bootstrap" } else { username.as_str() })
        .bind(if username.is_empty() { "bootstrap" } else { username.as_str() })
        .bind(serde_json::json!({ "remaining_seconds": remaining.as_secs() }))
        .execute(&s.db)
        .await;
        return ui::render(&LoginPage {
            error: true,
            error_msg: format!("登录已临时锁定，请 {} 秒后再试", remaining.as_secs()),
        })
        .into_response();
    }

    if username.is_empty() && form.password == s.cfg.admin_password {
        clear_admin_login_limit(&s, &limit_key);
        let _ = sqlx::query(
            "INSERT INTO audit_log (actor, action, target, metadata) VALUES ('bootstrap-owner', 'admin.login', 'bootstrap', $1)",
        )
        .bind(serde_json::json!({ "mode": "bootstrap" }))
        .execute(&s.db)
        .await;
        let mut resp = Redirect::to("/admin").into_response();
        set_admin_cookie(&mut resp, &s, "bootstrap", None, "owner");
        return resp;
    }

    if !username.is_empty() {
        let row = sqlx::query(
            r#"SELECT ao.username, ao.display_name, ao.operator_role, ao.enabled,
                      u.id AS user_id, u.password_hash
               FROM admin_operators ao
               JOIN users u ON u.id = ao.user_id
               WHERE ao.username = $1"#,
        )
        .bind(&username)
        .fetch_optional(&s.db)
        .await;
        if let Ok(Some(row)) = row {
            let enabled = row.try_get::<bool, _>("enabled").unwrap_or(false);
            let password_hash = row
                .try_get::<String, _>("password_hash")
                .unwrap_or_default();
            let role = row
                .try_get::<String, _>("operator_role")
                .unwrap_or_else(|_| "operator".into());
            let user_id = row.try_get::<Uuid, _>("user_id").ok();
            if enabled
                && user_id.is_some()
                && matches!(role.as_str(), "owner" | "admin" | "operator" | "auditor")
                && hashing::verify_password(&form.password, &password_hash).unwrap_or(false)
            {
                clear_admin_login_limit(&s, &limit_key);
                let display_name = row
                    .try_get::<String, _>("display_name")
                    .unwrap_or_else(|_| username.clone());
                let _ = sqlx::query(
                    "UPDATE admin_operators SET last_login_at = now() WHERE username = $1",
                )
                .bind(&username)
                .execute(&s.db)
                .await;
                let _ = sqlx::query(
                    "INSERT INTO audit_log (actor, action, target, metadata) VALUES ($1, 'admin.login', $2, $3)",
                )
                .bind(format!("operator:{username}"))
                .bind(&username)
                .bind(serde_json::json!({ "mode": "operator", "role": &role, "display_name": &display_name }))
                .execute(&s.db)
                .await;
                let mut resp = Redirect::to("/admin").into_response();
                set_admin_cookie(&mut resp, &s, &username, user_id, &role);
                return resp;
            }
        }
    }
    record_admin_login_failure(&s, &limit_key);
    let _ = sqlx::query(
        "INSERT INTO audit_log (actor, action, target, metadata) VALUES ($1, 'admin.login_failed', $2, $3)",
    )
    .bind(if username.is_empty() { "bootstrap" } else { username.as_str() })
    .bind(if username.is_empty() { "bootstrap" } else { username.as_str() })
    .bind(serde_json::json!({ "mode": if username.is_empty() { "bootstrap" } else { "operator" } }))
    .execute(&s.db)
    .await;
    ui::render(&LoginPage {
        error: true,
        error_msg: "账号或密码错误".into(),
    })
    .into_response()
}

async fn logout(State(s): State<Arc<AppState>>) -> Response {
    let mut resp = Redirect::to("/admin/login").into_response();
    clear_admin_cookie(&mut resp, &s);
    resp
}

// ---------------- dashboard ----------------
pub struct AuditRow {
    pub time: String,
    pub action: String,
    pub actor: String,
    pub target: String,
}

pub struct EcosystemMetric {
    pub label: &'static str,
    pub value: i64,
    pub detail: String,
    pub href: &'static str,
    pub tone: &'static str,
}

pub struct OpsLink {
    pub title: &'static str,
    pub detail: &'static str,
    pub href: &'static str,
    pub badge: &'static str,
}

#[derive(Template)]
#[template(path = "dashboard_content.html")]
pub struct DashboardPage {
    pub title: String,
    pub subtitle: Option<String>,
    pub notice: Option<ui::AdminNotice>,
    pub host: &'static str,
    pub route: &'static str,

    pub user_count: i64,
    pub user_today: i64,
    pub active_sessions: i64,
    pub pending_rebinds: i64,
    pub active_invites: i64,

    pub bind_addr: String,
    pub tls_on: bool,
    pub require_invite: bool,
    pub cdn_base: String,
    pub sub_count: i64,

    pub recent_audit: Vec<AuditRow>,
    pub ecosystem: Vec<EcosystemMetric>,
    pub ops_links: Vec<OpsLink>,
}

async fn count_or_zero(state: &AppState, sql: &str) -> i64 {
    sqlx::query_scalar::<_, i64>(sql)
        .fetch_one(&state.db)
        .await
        .unwrap_or(0)
}

async fn dashboard(State(s): State<Arc<AppState>>, headers: HeaderMap) -> Response {
    if admin_session(&headers, &s).await.is_none() {
        return admin_login_redirect();
    }

    let user_count = sqlx::query_scalar!("SELECT COUNT(*) FROM users")
        .fetch_one(&s.db)
        .await
        .unwrap_or(Some(0))
        .unwrap_or(0);
    let user_today = sqlx::query_scalar!(
        "SELECT COUNT(*) FROM users WHERE created_at > now() - interval '1 day'"
    )
    .fetch_one(&s.db)
    .await
    .unwrap_or(Some(0))
    .unwrap_or(0);
    let active_sessions =
        sqlx::query_scalar!("SELECT COUNT(*) FROM sessions WHERE expires_at > now()")
            .fetch_one(&s.db)
            .await
            .unwrap_or(Some(0))
            .unwrap_or(0);
    let pending_rebinds =
        sqlx::query_scalar!("SELECT COUNT(*) FROM hwid_rebind_requests WHERE status='pending'")
            .fetch_one(&s.db)
            .await
            .unwrap_or(Some(0))
            .unwrap_or(0);
    let active_invites = sqlx::query_scalar!(
        "SELECT COUNT(*) FROM invite_codes \
         WHERE revoked_at IS NULL \
           AND (expires_at IS NULL OR expires_at > now()) \
           AND use_count < max_uses"
    )
    .fetch_one(&s.db)
    .await
    .unwrap_or(Some(0))
    .unwrap_or(0);
    let sub_count = sqlx::query_scalar!("SELECT COUNT(*) FROM subscriptions")
        .fetch_one(&s.db)
        .await
        .unwrap_or(Some(0))
        .unwrap_or(0);

    let channel_count = count_or_zero(&s, "SELECT COUNT(*) FROM chats WHERE kind='channel'").await;
    let channel_policy_gaps = count_or_zero(
        &s,
        "SELECT COUNT(*) \
         FROM chats c \
         LEFT JOIN channel_settings cs ON cs.chat_id = c.id \
         WHERE c.kind='channel' AND (cs.chat_id IS NULL OR cs.is_enabled = FALSE)",
    )
    .await;
    let message_count =
        count_or_zero(&s, "SELECT COUNT(*) FROM messages WHERE deleted_at IS NULL").await;
    let message_today = count_or_zero(
        &s,
        "SELECT COUNT(*) FROM messages \
         WHERE deleted_at IS NULL AND created_at > now() - interval '1 day'",
    )
    .await;
    let ticket_open_count = count_or_zero(
        &s,
        "SELECT COUNT(*) FROM tickets \
         WHERE status IN ('open','pending') AND protected_by_superadmin = FALSE",
    )
    .await;
    let announcement_active_count = count_or_zero(
        &s,
        "SELECT COUNT(*) FROM announcements \
         WHERE starts_at <= now() AND (expires_at IS NULL OR expires_at > now())",
    )
    .await;
    let market_active_count = count_or_zero(
        &s,
        "SELECT COUNT(*) FROM market_listings WHERE status='active'",
    )
    .await;
    let sticker_pack_count = count_or_zero(&s, "SELECT COUNT(*) FROM sticker_packs").await;
    let client_config_count = count_or_zero(
        &s,
        "SELECT COUNT(*) FROM app_config_entries WHERE enabled = TRUE AND expose_to_client = TRUE",
    )
    .await;
    let audit_today_count = count_or_zero(
        &s,
        "SELECT COUNT(*) FROM audit_log WHERE occurred_at > now() - interval '1 day'",
    )
    .await;

    let ecosystem = vec![
        EcosystemMetric {
            label: "频道",
            value: channel_count,
            detail: format!("{channel_policy_gaps} 个需要补齐或启用策略"),
            href: "/admin/channels",
            tone: "border-orange-400",
        },
        EcosystemMetric {
            label: "消息",
            value: message_count,
            detail: format!("{message_today} 条 24h 内新增"),
            href: "/admin/chat",
            tone: "border-emerald-400",
        },
        EcosystemMetric {
            label: "待处理工单",
            value: ticket_open_count,
            detail: "open / pending，不含 SuperAdmin 保护工单".into(),
            href: "/admin/tickets",
            tone: "border-amber-400",
        },
        EcosystemMetric {
            label: "有效公告",
            value: announcement_active_count,
            detail: "当前客户端可收到的公告".into(),
            href: "/admin/announcements",
            tone: "border-sky-400",
        },
        EcosystemMetric {
            label: "市场商品",
            value: market_active_count,
            detail: "active 状态的上架内容".into(),
            href: "/admin/config",
            tone: "border-violet-400",
        },
        EcosystemMetric {
            label: "表情包",
            value: sticker_pack_count,
            detail: "用户创建和公开安装生态".into(),
            href: "/admin/chat",
            tone: "border-pink-400",
        },
        EcosystemMetric {
            label: "客户端配置",
            value: client_config_count,
            detail: "已启用并暴露给 bootstrap".into(),
            href: "/admin/config",
            tone: "border-cyan-400",
        },
        EcosystemMetric {
            label: "今日审计",
            value: audit_today_count,
            detail: "后台和系统操作记录".into(),
            href: "/admin/audit",
            tone: "border-lime-400",
        },
    ];

    let ops_links = vec![
        OpsLink {
            title: "用户与权限",
            detail: "资料、订阅、角色、强制重置密码和删除账号",
            href: "/admin/users",
            badge: "账号",
        },
        OpsLink {
            title: "频道治理",
            detail: "真实用户视角校验、频道策略、慢速模式和软清空",
            href: "/admin/channels",
            badge: "社区",
        },
        OpsLink {
            title: "聊天控制台",
            detail: "以后台身份向官方频道发消息并查看历史",
            href: "/admin/chat",
            badge: "消息",
        },
        OpsLink {
            title: "工单与公告",
            detail: "分类、状态、公开/私密、消息打码和客户端公告",
            href: "/admin/tickets",
            badge: "支持",
        },
        OpsLink {
            title: "配置中心",
            detail: "客户端功能开关、品牌、限制和版本回滚",
            href: "/admin/config",
            badge: "配置",
        },
        OpsLink {
            title: "后台权限与审计",
            detail: "操作者分级、变更记录和最近 200 条审计",
            href: "/admin/operators",
            badge: "安全",
        },
    ];

    let audit = sqlx::query!(
        r#"SELECT occurred_at, action, actor, target
           FROM audit_log ORDER BY occurred_at DESC LIMIT 12"#
    )
    .fetch_all(&s.db)
    .await
    .unwrap_or_default();
    let recent_audit = audit
        .into_iter()
        .map(|r| AuditRow {
            time: r.occurred_at.format("%m-%d %H:%M:%S").to_string(),
            action: r.action,
            actor: r.actor.unwrap_or_else(|| "—".into()),
            target: r.target.unwrap_or_else(|| "—".into()),
        })
        .collect();

    ui::render(&DashboardPage {
        title: "仪表盘".into(),
        subtitle: Some(format!(
            "{} · 当前 {} 个活跃 session",
            ui::host(),
            active_sessions
        )),
        notice: None,
        host: ui::host(),
        route: ui::ROUTE_DASHBOARD,
        user_count,
        user_today,
        active_sessions,
        pending_rebinds,
        active_invites,
        bind_addr: s.cfg.bind_addr.clone(),
        tls_on: s.cfg.tls_cert_path.is_some(),
        require_invite: s.cfg.require_invite_code,
        cdn_base: s.cfg.cdn_base.clone(),
        sub_count,
        recent_audit,
        ecosystem,
        ops_links,
    })
    .into_response()
}

pub fn routes(state: Arc<AppState>) -> Router<Arc<AppState>> {
    Router::new()
        .route("/", get(dashboard))
        .route("/login", get(login_page).post(login_submit))
        .route("/logout", get(logout))
        .with_state(state)
}
