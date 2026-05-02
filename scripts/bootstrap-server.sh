#!/usr/bin/env bash
# 服务器一次性初始化脚本：装 Rust、protoc、PostgreSQL（若未装）、systemd unit、专用用户
# 由 deploy.ps1 -BootstrapServer 触发或在服务器上手动运行
#
# 假设：debian 系（Ubuntu/Debian），已装 1Panel
# 用法：bash bootstrap-server.sh

set -euo pipefail

REMOTE_DIR=${REMOTE_DIR:-/opt/systembackend}
SVC=${SVC:-systembackend}
PG_USER=${PG_USER:-helix}
PG_DB=${PG_DB:-helix}
PG_PASS=${PG_PASS:-$(openssl rand -hex 16)}

log() { echo -e "\033[1;34m[bootstrap]\033[0m $*"; }

log "apt deps"
apt-get update
apt-get install -y --no-install-recommends \
    build-essential pkg-config libssl-dev curl git \
    protobuf-compiler postgresql postgresql-contrib rsync ca-certificates

log "rustup"
if ! command -v cargo >/dev/null; then
    curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y --default-toolchain 1.75
    source "$HOME/.cargo/env"
fi

log "postgres user/db"
sudo -u postgres psql <<SQL
DO \$\$
BEGIN
   IF NOT EXISTS (SELECT FROM pg_roles WHERE rolname = '${PG_USER}') THEN
      CREATE ROLE ${PG_USER} LOGIN PASSWORD '${PG_PASS}';
   END IF;
END\$\$;
SELECT 'CREATE DATABASE ${PG_DB} OWNER ${PG_USER}'
WHERE NOT EXISTS (SELECT FROM pg_database WHERE datname = '${PG_DB}')\gexec
SQL

log "system user"
id systembackend &>/dev/null || useradd --system --home-dir "$REMOTE_DIR" --shell /usr/sbin/nologin systembackend

log "directory"
mkdir -p "$REMOTE_DIR/migrations"
chown -R systembackend:systembackend "$REMOTE_DIR"

log "systemd unit"
cp -f "$REMOTE_DIR/SystemBackend/docker/systembackend.service" /etc/systemd/system/${SVC}.service 2>/dev/null \
  || cat > /etc/systemd/system/${SVC}.service <<UNIT
[Unit]
Description=Launcher SystemBackend
After=network.target postgresql.service

[Service]
Type=simple
User=systembackend
WorkingDirectory=${REMOTE_DIR}
ExecStart=${REMOTE_DIR}/systembackend
Environment=LAUNCHER_CONFIG=${REMOTE_DIR}/config.toml
Environment=RUST_LOG=info
Restart=on-failure

[Install]
WantedBy=multi-user.target
UNIT

systemctl daemon-reload

log "firewall (1337)"
if command -v ufw >/dev/null; then
    ufw allow 1337/tcp || true
fi

log "done. credentials:"
echo "  PG_USER=${PG_USER}"
echo "  PG_DB=${PG_DB}"
echo "  PG_PASS=${PG_PASS}"
echo
echo "next:"
echo "  1) edit ${REMOTE_DIR}/config.toml — set database_url and signing_public_key_hex"
echo "  2) systemctl enable --now ${SVC}"
echo "  3) journalctl -u ${SVC} -f"
