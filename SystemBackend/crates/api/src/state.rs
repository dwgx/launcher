use launcher_shared::config::AppConfig;
use launcher_shared::hashing;
use rand::{rngs::OsRng, RngCore};
use sqlx::PgPool;
use std::collections::HashMap;
use std::sync::Mutex;
use std::time::Instant;

#[derive(Clone)]
pub struct LoginAttemptEntry {
    pub count: u32,
    pub first_at: Instant,
}

#[derive(Clone)]
pub struct AppState {
    pub cfg: AppConfig,
    pub db: PgPool,
    pub login_attempts: std::sync::Arc<Mutex<HashMap<String, LoginAttemptEntry>>>,
    /// admin 会话 cookie HMAC 密钥；与 admin_password 解耦，改密码不失效已登录会话。
    pub admin_cookie_secret: std::sync::Arc<[u8; 32]>,
    /// M-2: 未知用户登录时跑 dummy argon2 verify，用与真实密码相同的成本参数消除枚举时序旁路。
    pub dummy_password_hash: String,
}

impl AppState {
    pub fn new(cfg: AppConfig, db: PgPool) -> Self {
        let admin_cookie_secret = derive_admin_cookie_secret(&cfg);
        let dummy_password_hash = hashing::hash_password(
            "launcher.dummy.timing.guard",
            cfg.argon_memory_kib,
            cfg.argon_iterations,
        )
        .expect("precompute dummy argon2 hash from configured params");
        Self {
            cfg,
            db,
            login_attempts: std::sync::Arc::new(Mutex::new(HashMap::new())),
            admin_cookie_secret,
            dummy_password_hash,
        }
    }
}

fn derive_admin_cookie_secret(cfg: &AppConfig) -> std::sync::Arc<[u8; 32]> {
    if let Some(raw) = cfg.admin_cookie_secret.as_deref() {
        let trimmed = raw.trim();
        if !trimmed.is_empty() {
            match hex::decode(trimmed) {
                Ok(bytes) if bytes.len() >= 32 => {
                    let mut key = [0u8; 32];
                    key.copy_from_slice(&bytes[..32]);
                    return std::sync::Arc::new(key);
                }
                _ => tracing::warn!("admin_cookie_secret 无效（需 ≥32 字节 hex），改用随机启动密钥"),
            }
        }
    }
    let mut key = [0u8; 32];
    OsRng.fill_bytes(&mut key);
    tracing::warn!("admin_cookie_secret 未配置，本次启动随机生成——重启将使已登录 admin 会话失效");
    std::sync::Arc::new(key)
}
