use launcher_shared::config::AppConfig;
use sqlx::PgPool;

#[derive(Clone)]
pub struct AppState {
    pub cfg: AppConfig,
    pub db:  PgPool,
}
