use launcher_shared::{config::AppConfig, logging};
use std::sync::Arc;

mod auth;
mod admin;
mod heartbeat;
mod rebind;
mod state;
mod subscription;

use state::AppState;

#[tokio::main]
async fn main() -> anyhow::Result<()> {
    logging::init();

    let cfg_path = std::env::var("LAUNCHER_CONFIG")
        .unwrap_or_else(|_| "/opt/systembackend/config.toml".to_string());
    let cfg = AppConfig::from_file(&cfg_path)?;
    tracing::info!("loaded config from {}", cfg_path);

    let pool = sqlx::PgPool::connect(&cfg.database_url).await?;
    sqlx::migrate!("../../migrations").run(&pool).await?;

    let state = Arc::new(AppState { cfg: cfg.clone(), db: pool });

    let app = axum::Router::new()
        .nest("/api",   api_routes(state.clone()))
        .nest("/admin", admin::routes(state.clone()))
        .layer(tower_http::trace::TraceLayer::new_for_http())
        .layer(tower_http::limit::RequestBodyLimitLayer::new(1024 * 1024))
        .with_state(state);

    let listener = tokio::net::TcpListener::bind(&cfg.bind_addr).await?;
    tracing::info!("listening on {}", cfg.bind_addr);
    axum::serve(listener, app).await?;
    Ok(())
}

fn api_routes(state: Arc<AppState>) -> axum::Router<Arc<AppState>> {
    use axum::routing::{post, get};
    axum::Router::new()
        .route("/auth/login",          post(auth::login))
        .route("/auth/logout",         post(auth::logout))
        .route("/heartbeat",           post(heartbeat::heartbeat))
        .route("/subscription",        get(subscription::list_for_user))
        .route("/hwid/rebind/request", post(rebind::submit))
        .route("/hwid/rebind/list",    get(rebind::list_for_user))
        .with_state(state)
}
