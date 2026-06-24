use launcher_shared::config::AppConfig;
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
}

impl AppState {
    pub fn new(cfg: AppConfig, db: PgPool) -> Self {
        Self {
            cfg,
            db,
            login_attempts: std::sync::Arc::new(Mutex::new(HashMap::new())),
        }
    }
}
