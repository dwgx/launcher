use serde::Deserialize;
use std::path::Path;

#[derive(Debug, Clone, Deserialize)]
pub struct AppConfig {
    pub bind_addr: String,
    pub database_url: String,
    pub session_ttl_seconds: i64,
    pub heartbeat_grace_seconds: i64,

    /// Argon2id 内存成本 (KiB)
    #[serde(default = "default_argon_mem")]
    pub argon_memory_kib: u32,
    #[serde(default = "default_argon_iters")]
    pub argon_iterations: u32,

    /// 当前活跃签名公钥（hex），客户端会硬编码同样的值
    pub signing_public_key_hex: String,

    /// CDN base，用于客户端拉取 .helix
    pub cdn_base: String,

    /// 管理后台登录密码（明文留 toml，部署后立刻 chmod 600）
    pub admin_password: String,

    /// TLS 证书路径（fullchain pem）；为空则跑纯 HTTP
    #[serde(default)]
    pub tls_cert_path: Option<String>,
    /// TLS 私钥路径（pem）
    #[serde(default)]
    pub tls_key_path: Option<String>,

    /// 是否强制邀请码注册。生产默认 true。
    #[serde(default = "default_require_invite")]
    pub require_invite_code: bool,

    /// chat 上传 / media — 控制能发多大文件。
    /// 默认 image=8MB image=8MB image=8MB / video=32MB / generic=100MB（旧值）。
    #[serde(default = "default_image_max")]
    pub media_image_max_bytes: u64,
    #[serde(default = "default_video_max")]
    pub media_video_max_bytes: u64,
    #[serde(default = "default_generic_max")]
    pub media_generic_max_bytes: u64,
}

fn default_argon_mem() -> u32 { 64 * 1024 }
fn default_argon_iters() -> u32 { 3 }
fn default_require_invite() -> bool { true }
fn default_image_max() -> u64 { 8 * 1024 * 1024 }
fn default_video_max() -> u64 { 32 * 1024 * 1024 }
fn default_generic_max() -> u64 { 100 * 1024 * 1024 }

impl AppConfig {
    pub fn from_file(path: impl AsRef<Path>) -> anyhow::Result<Self> {
        let raw = std::fs::read_to_string(path)?;
        let cfg: AppConfig = toml::from_str(&raw)?;
        Ok(cfg)
    }
}
