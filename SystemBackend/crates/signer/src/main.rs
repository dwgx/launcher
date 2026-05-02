// Launcher 离线签名 CLI。
// 子命令：
//   keygen --out <dir>            生成 Ed25519 密钥对到 <dir>/private.key + public.key
//   sign --key <pri> --in <toml> --out <out.helix>
//   verify --key <pub> --in <helix>
//
// Why: 私钥永远不上服务器，签发离线进行；服务端只持公钥用于展示和审计。

use clap::{Parser, Subcommand};
use ed25519_dalek::{Signer, SigningKey, VerifyingKey, Signature, SECRET_KEY_LENGTH};
use prost::Message;
use rand::rngs::OsRng;
use serde::Deserialize;
use std::path::PathBuf;
use std::fs;

#[derive(Parser)]
#[command(name = "signer", version)]
struct Cli {
    #[command(subcommand)]
    cmd: Cmd,
}

#[derive(Subcommand)]
enum Cmd {
    Keygen { #[arg(long)] out: PathBuf },
    Sign {
        #[arg(long)] key: PathBuf,
        #[arg(long, value_name = "TOML")] r#in: PathBuf,
        #[arg(long)] out: PathBuf,
    },
    Verify {
        #[arg(long)] key: PathBuf,
        #[arg(long, value_name = "HELIX")] r#in: PathBuf,
    },
}

#[derive(Debug, Deserialize)]
struct GameTomlEntry {
    game_id: String,
    display_name: String,
    version: String,
    download_url: String,
    file_path: Option<String>,     // 用于本地算 BLAKE3
    cover_url: Option<String>,
    executable: String,
    args: Option<Vec<String>>,
    working_directory: Option<String>,
}

#[derive(Debug, Deserialize)]
struct SubscriptionToml {
    id: String,
    name: String,
    valid_seconds: u64,
    issuer: String,
    games: Vec<GameTomlEntry>,
}

fn main() -> anyhow::Result<()> {
    tracing_subscriber::fmt::init();
    match Cli::parse().cmd {
        Cmd::Keygen { out } => keygen(&out),
        Cmd::Sign { key, r#in, out } => sign(&key, &r#in, &out),
        Cmd::Verify { key, r#in } => verify(&key, &r#in),
    }
}

fn keygen(dir: &PathBuf) -> anyhow::Result<()> {
    fs::create_dir_all(dir)?;
    let sk = SigningKey::generate(&mut OsRng);
    let pk = sk.verifying_key();
    fs::write(dir.join("private.key"), sk.to_bytes())?;
    fs::write(dir.join("public.key"),  pk.to_bytes())?;
    fs::write(dir.join("public.hex"),  hex::encode(pk.to_bytes()))?;
    tracing::info!("wrote {:?}", dir);
    println!("public_hex = {}", hex::encode(pk.to_bytes()));
    Ok(())
}

fn load_signing_key(path: &PathBuf) -> anyhow::Result<SigningKey> {
    let raw = fs::read(path)?;
    if raw.len() != SECRET_KEY_LENGTH {
        anyhow::bail!("private key length must be {SECRET_KEY_LENGTH}");
    }
    let mut buf = [0u8; SECRET_KEY_LENGTH];
    buf.copy_from_slice(&raw);
    Ok(SigningKey::from_bytes(&buf))
}

fn sign(key: &PathBuf, in_toml: &PathBuf, out_helix: &PathBuf) -> anyhow::Result<()> {
    use launcher_proto::launcher_proto as p;

    let raw = fs::read_to_string(in_toml)?;
    let cfg: SubscriptionToml = toml::from_str(&raw)?;
    let now = chrono::Utc::now().timestamp() as u64;

    let mut sub = p::Subscription {
        id: cfg.id.clone(),
        name: cfg.name.clone(),
        updated_at: now,
        valid_until: now + cfg.valid_seconds,
        games: vec![],
        issuer: cfg.issuer.clone(),
        schema_version: 1,
        signature: vec![],
    };

    for g in cfg.games {
        let hash = if let Some(fp) = &g.file_path {
            blake3::hash(&fs::read(fp)?).as_bytes().to_vec()
        } else { vec![] };
        sub.games.push(p::GameEntry {
            game_id: g.game_id,
            display_name: g.display_name,
            version: g.version,
            download_url: g.download_url,
            file_hash: hash,
            size_bytes: g.file_path.as_ref()
                .and_then(|p| fs::metadata(p).ok())
                .map(|m| m.len()).unwrap_or(0),
            launch: Some(p::LaunchConfig {
                executable: g.executable,
                args: g.args.unwrap_or_default(),
                env: Default::default(),
                hooks: vec![],
                working_directory: g.working_directory.unwrap_or_default(),
            }),
            cover_url: g.cover_url.unwrap_or_default(),
        });
    }

    let unsigned = sub.encode_to_vec();
    let sk = load_signing_key(key)?;
    let sig: Signature = sk.sign(&unsigned);
    sub.signature = sig.to_bytes().to_vec();

    let final_bytes = sub.encode_to_vec();
    fs::write(out_helix, final_bytes)?;
    tracing::info!("signed {} games -> {:?}", sub.games.len(), out_helix);
    Ok(())
}

fn verify(key: &PathBuf, in_helix: &PathBuf) -> anyhow::Result<()> {
    use launcher_proto::launcher_proto as p;
    let bytes = fs::read(in_helix)?;
    let mut sub = p::Subscription::decode(bytes.as_slice())?;
    let sig_bytes = std::mem::take(&mut sub.signature);
    if sig_bytes.len() != 64 {
        anyhow::bail!("signature length != 64");
    }
    let unsigned = sub.encode_to_vec();

    let pk_raw = fs::read(key)?;
    if pk_raw.len() != 32 { anyhow::bail!("public key length != 32"); }
    let mut pk_buf = [0u8; 32];
    pk_buf.copy_from_slice(&pk_raw);
    let pk = VerifyingKey::from_bytes(&pk_buf)?;
    let mut sig_arr = [0u8; 64];
    sig_arr.copy_from_slice(&sig_bytes);
    let sig = Signature::from_bytes(&sig_arr);
    pk.verify_strict(&unsigned, &sig)?;
    println!("ok: {} games, expires {}", sub.games.len(), sub.valid_until);
    Ok(())
}
