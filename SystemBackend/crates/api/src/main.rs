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

    // Why: rustls 0.23 起没有默认 crypto provider，要在第一次用 rustls 之前显式安装
    rustls::crypto::ring::default_provider().install_default()
        .expect("rustls crypto provider install");

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

    // 如果配了 TLS cert，用 axum-server-rustls；否则纯 HTTP
    if let (Some(cert_path), Some(key_path)) =
        (&cfg.tls_cert_path, &cfg.tls_key_path)
    {
        let addr: std::net::SocketAddr = cfg.bind_addr.parse()?;
        tracing::info!("HTTPS listening on {} (cert={})", addr, cert_path);
        let tls = axum_server::tls_rustls::RustlsConfig::from_pem_file(cert_path, key_path).await?;
        axum_server::bind_rustls(addr, tls)
            .serve(app.into_make_service())
            .await?;
    } else {
        let listener = tokio::net::TcpListener::bind(&cfg.bind_addr).await?;
        tracing::info!("HTTP listening on {}", cfg.bind_addr);
        axum::serve(listener, app).await?;
    }
    Ok(())
}

fn api_routes(state: Arc<AppState>) -> axum::Router<Arc<AppState>> {
    use axum::routing::{post, get};
    axum::Router::new()
        .route("/auth/register",         post(auth::register))
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
