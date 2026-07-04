use launcher_shared::{config::AppConfig, logging};
use std::sync::Arc;

mod admin;
mod admin_announcements;
mod admin_channels;
mod admin_chat;
mod admin_customization;
mod admin_invites;
mod admin_tickets;
mod admin_users;
mod audit;
mod auth;
mod chat;
mod client_settings;
mod community;
mod error;
mod heartbeat;
mod market;
mod media;
mod media_policy;
mod media_thumb;
mod profile;
mod rebind;
mod state;
mod sticker;
mod subscription;
mod ui;
mod ws;

use state::AppState;

#[tokio::main]
async fn main() -> anyhow::Result<()> {
    logging::init();

    // Why: rustls 0.23 起没有默认 crypto provider，要在第一次用 rustls 之前显式安装
    rustls::crypto::ring::default_provider()
        .install_default()
        .expect("rustls crypto provider install");

    let cfg_path = std::env::var("LAUNCHER_CONFIG")
        .unwrap_or_else(|_| "/opt/systembackend/config.toml".to_string());
    let cfg = AppConfig::from_file(&cfg_path)?;
    tracing::info!("loaded config from {}", cfg_path);

    let pool = sqlx::PgPool::connect(&cfg.database_url).await?;
    sqlx::migrate!("../../migrations").run(&pool).await?;

    let state = Arc::new(AppState::new(cfg.clone(), pool));

    let app = axum::Router::new()
        .route(
            "/",
            axum::routing::get(|| async { axum::response::Redirect::to("/admin") }),
        )
        .route(
            "/admin/",
            axum::routing::get(|| async { axum::response::Redirect::to("/admin") }),
        )
        .route("/ws/chat", axum::routing::get(ws::ws_handler))
        .nest("/api", api_routes(state.clone()))
        .nest("/admin", admin::routes(state.clone()))
        .merge(admin_users::routes())
        .merge(admin_invites::routes())
        .merge(admin_channels::routes())
        .merge(admin_chat::routes())
        .merge(admin_announcements::routes())
        .merge(admin_tickets::routes())
        .merge(admin_customization::routes())
        .merge(rebind::admin_routes())
        .layer(tower_http::trace::TraceLayer::new_for_http())
        // multipart 上限 100MB（媒体上传）
        .layer(tower_http::limit::RequestBodyLimitLayer::new(
            100 * 1024 * 1024,
        ))
        .with_state(state);

    // 如果配了 TLS cert，用 axum-server-rustls；否则纯 HTTP
    if let (Some(cert_path), Some(key_path)) = (&cfg.tls_cert_path, &cfg.tls_key_path) {
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
    use axum::routing::{get, post};
    axum::Router::new()
        // auth
        .route("/auth/register", post(auth::register))
        .route("/auth/login", post(auth::login))
        .route("/auth/logout", post(auth::logout))
        .route("/heartbeat", post(heartbeat::heartbeat))
        // subscription
        .route("/subscription", get(subscription::list_for_user))
        // hwid rebind
        .route("/hwid/rebind/request", post(rebind::submit))
        .route("/hwid/rebind/list", get(rebind::list_for_user))
        // profile
        .route("/profile", get(profile::get_profile))
        .route("/profile/nickname", post(profile::change_nickname))
        .route("/profile/password", post(profile::change_password))
        .route("/profile/avatar", post(profile::upload_avatar))
        .route("/profile/login-history", get(profile::login_history))
        .route("/profile/status", post(profile::set_status))
        .route("/profile/update", post(profile::update_profile))
        .route("/profile/peer/:key", get(profile::get_peer_profile))
        .route("/avatar/:id", get(profile::get_avatar))
        // user_tags chips
        .route("/profile/tags", get(profile::list_tags))
        .route("/profile/tags/add", post(profile::add_tag))
        .route("/profile/tags/remove", post(profile::remove_tag))
        // media
        .route("/media/upload", post(media::upload))
        .route("/media/:sha/:name", get(media::download))
        // chat
        .route("/chat/dm", post(chat::open_dm))
        .route("/chat/group", post(chat::create_group))
        .route("/chat/list", get(chat::list_chats))
        .route("/chat/official", get(chat::list_official))
        .route("/chat/send", post(chat::send))
        .route("/chat/history", get(chat::history))
        .route("/chat/sync", get(chat::sync_events))
        .route("/chat/search", get(chat::search))
        .route("/chat/read", post(chat::mark_read))
        .route("/chat/react", post(chat::react))
        .route("/chat/delete", post(chat::delete_msg))
        .route("/chat/moderation/member", get(chat::moderation_member))
        .route("/chat/moderation/mute", post(chat::mute_user))
        .route("/chat/moderation/unmute", post(chat::unmute_user))
        // community / forum / tickets
        .route("/community/bootstrap", get(community::bootstrap))
        .route("/community/checkin", post(community::checkin))
        .route(
            "/forum/topics",
            get(community::list_topics).post(community::create_topic),
        )
        .route(
            "/ticket/categories",
            get(community::list_ticket_categories).post(community::create_ticket_category),
        )
        .route(
            "/ticket/categories/:id",
            post(community::update_ticket_category),
        )
        .route("/tickets", post(community::create_ticket))
        .route("/tickets/mine", get(community::list_my_tickets))
        .route("/tickets/public", get(community::list_public_tickets))
        .route("/tickets/:id", get(community::get_ticket_detail))
        .route("/admin/tickets", get(community::list_admin_tickets))
        .route(
            "/admin/tickets/:id/status",
            post(community::update_ticket_status),
        )
        .route(
            "/admin/tickets/:id/visibility",
            post(community::update_ticket_visibility),
        )
        .route(
            "/admin/tickets/:id/protect",
            post(community::protect_ticket),
        )
        .route(
            "/admin/messages/:id/edit",
            post(community::admin_edit_message),
        )
        .route("/announcements/active", get(community::list_announcements))
        .route(
            "/announcements/:id/read",
            post(community::mark_announcement_read),
        )
        .route(
            "/announcements/:id/ack",
            post(community::acknowledge_announcement),
        )
        .route("/admin/announcements", post(community::create_announcement))
        // desktop settings sync
        .route(
            "/client/settings",
            get(client_settings::get_settings).post(client_settings::upsert_settings),
        )
        // sticker
        .route("/sticker", post(sticker::create_sticker))
        .route("/sticker/delete", post(sticker::delete_sticker))
        .route("/sticker/pack", post(sticker::create_pack))
        .route("/sticker/pack/:id", get(sticker::get_pack))
        .route("/sticker/pack/add", post(sticker::add_to_pack))
        .route("/sticker/pack/remove", post(sticker::remove_from_pack))
        .route("/sticker/pack/install", post(sticker::install))
        .route("/sticker/pack/uninstall", post(sticker::uninstall))
        .route("/sticker/pack/rename", post(sticker::rename_pack))
        .route("/sticker/pack/delete", post(sticker::delete_pack))
        .route("/sticker/pack/share", post(sticker::share_pack))
        .route("/sticker/pack/cover", post(sticker::set_pack_cover))
        .route("/sticker/pack/reorder", post(sticker::reorder_packs))
        .route(
            "/sticker/pack/by-short/:short",
            get(sticker::get_pack_by_short),
        )
        .route("/sticker/packs/public", get(sticker::list_public_packs))
        .route("/sticker/packs/mine", get(sticker::my_packs))
        .route("/sticker/mine", get(sticker::my_stickers))
        // market
        .route("/market/categories", get(market::list_categories))
        .route("/market/listings", get(market::list_listings))
        .route("/market/listings/:id", get(market::get_listing))
        .route("/market/listing/create", post(market::create_listing))
        .route("/market/purchase", post(market::purchase))
        .route("/market/orders/mine", get(market::my_orders))
        .route("/market/review", post(market::review))
        .route(
            "/market/admin/grant_credit",
            post(market::admin_grant_credit),
        )
        .with_state(state)
}
