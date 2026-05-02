use launcher_shared::{config::AppConfig, logging};
use std::sync::Arc;

mod auth;
mod admin;
mod admin_users;
mod heartbeat;
mod profile;
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
        .merge(admin_users::routes())
        .layer(tower_http::trace::TraceLayer::new_for_http())
        // 头像上传走 multipart，限 5MB；生产再单独 raise multipart limit
        .layer(tower_http::limit::RequestBodyLimitLayer::new(6 * 1024 * 1024))
        .with_state(state);

    let listener = tokio::net::TcpListener::bind(&cfg.bind_addr).await?;
    tracing::info!("listening on {}", cfg.bind_addr);
    axum::serve(listener, app).await?;
    Ok(())
}

fn api_routes(state: Arc<AppState>) -> axum::Router<Arc<AppState>> {
    use axum::routing::{post, get};
    axum::Router::new()
        .route("/auth/login",            post(auth::login))
        .route("/auth/logout",           post(auth::logout))
        .route("/heartbeat",             post(heartbeat::heartbeat))
        .route("/subscription",          get(subscription::list_for_user))
        .route("/hwid/rebind/request",   post(rebind::submit))
        .route("/hwid/rebind/list",      get(rebind::list_for_user))

        .route("/profile",               get(profile::get_profile))
        .route("/profile/nickname",      post(profile::change_nickname))
        .route("/profile/password",      post(profile::change_password))
        .route("/profile/avatar",        post(profile::upload_avatar))
        .route("/profile/login-history", get(profile::login_history))
        .route("/avatar/:id",            get(profile::get_avatar))

        .with_state(state)
}
