#!/usr/bin/env python3
"""服务器端基础环境一次性配置：apt 依赖 + Postgres + Rust + systembackend 用户/目录 + systemd unit。
不做 cargo build，那一步在下次部署时单独触发。

幂等：所有动作都先 check 再 do。重复运行无副作用。
"""
from __future__ import annotations
import os, sys, re, io
from pathlib import Path
import paramiko

sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8", errors="replace")
sys.stderr = io.TextIOWrapper(sys.stderr.buffer, encoding="utf-8", errors="replace")

ROOT = Path(__file__).resolve().parent.parent
CFG  = ROOT / ".deploy.local"

def parse(path: Path) -> dict:
    out = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        m = re.match(r'^\s*([\w_]+)\s*=\s*"?([^"]*)"?\s*$', line)
        if m: out[m.group(1)] = m.group(2)
    return out

def expand(p: str) -> Path:
    p = p.replace("$HOME", os.environ.get("USERPROFILE", str(Path.home())))
    return Path(p.replace("\\\\", "\\")).expanduser()

def main() -> int:
    c = parse(CFG)
    cli = paramiko.SSHClient()
    cli.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    cli.connect(c["host"], port=int(c["port"]), username=c["user"],
                key_filename=str(expand(c["key_path"])),
                look_for_keys=False, allow_agent=False, timeout=15)

    def sh(cmd: str, *, label: str | None = None, check: bool = True) -> tuple[int, str]:
        print(f"$ {label or cmd}")
        ch = cli.get_transport().open_session()
        ch.get_pty()
        ch.exec_command(f"set -e; {cmd}")
        buf = []
        while True:
            if ch.recv_ready():
                data = ch.recv(4096).decode("utf-8", "replace")
                buf.append(data); print(data, end="", flush=True)
            if ch.exit_status_ready() and not ch.recv_ready():
                break
        rc = ch.recv_exit_status()
        text = "".join(buf)
        if check and rc != 0:
            print(f"\n[!] command failed rc={rc}", file=sys.stderr); sys.exit(rc)
        return rc, text

    print("=== apt deps ===")
    sh("DEBIAN_FRONTEND=noninteractive apt-get update -qq")
    sh("DEBIAN_FRONTEND=noninteractive apt-get install -y -qq "
       "build-essential pkg-config libssl-dev curl git rsync ca-certificates "
       "protobuf-compiler postgresql postgresql-contrib ufw")

    print("\n=== rustup (root) ===")
    rc, _ = sh("test -x /root/.cargo/bin/cargo && echo present || echo missing",
               check=False)
    if "missing" in _:
        sh("curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs "
           "| sh -s -- -y --default-toolchain 1.75 --profile minimal")
    sh("source $HOME/.cargo/env && cargo --version", label="cargo --version")

    print("\n=== postgres ===")
    sh("systemctl enable --now postgresql")
    pg_user = "helix"
    pg_db   = "helix"
    pg_pass = "launcher_pg_$(openssl rand -hex 8)"
    sh(f"""sudo -u postgres psql -tAc \"SELECT 1 FROM pg_roles WHERE rolname='{pg_user}'\" \
        | grep -q 1 || sudo -u postgres psql -c \"CREATE ROLE {pg_user} LOGIN PASSWORD '{{pg_pass}}'\"
""".replace("{{pg_pass}}", "REPLACE_ME_FIRST"),
       label="ensure postgres role helix",
       check=False)

    # 单独读出实际生成的密码，写到 /opt/systembackend/.pg_creds
    sh(f"""if ! sudo -u postgres psql -tAc "SELECT 1 FROM pg_roles WHERE rolname='{pg_user}'" | grep -q 1; then
  PGPASS=$(openssl rand -hex 16)
  sudo -u postgres psql -c "CREATE ROLE {pg_user} LOGIN PASSWORD '$PGPASS'"
  install -d -m 700 /opt/systembackend
  echo "DATABASE_URL=postgres://{pg_user}:$PGPASS@127.0.0.1:5432/{pg_db}" > /opt/systembackend/.pg_creds
  chmod 600 /opt/systembackend/.pg_creds
  echo created
else
  echo exists
fi""", label="create postgres role if absent")

    sh(f"""sudo -u postgres psql -tAc "SELECT 1 FROM pg_database WHERE datname='{pg_db}'" \
        | grep -q 1 || sudo -u postgres psql -c "CREATE DATABASE {pg_db} OWNER {pg_user}" """,
       label="ensure postgres db helix")

    print("\n=== system user + dirs ===")
    sh("id systembackend &>/dev/null || useradd --system --home-dir /opt/systembackend "
       "--shell /usr/sbin/nologin systembackend",
       label="ensure systembackend user")
    sh("install -d -o systembackend -g systembackend /opt/systembackend "
       "/opt/systembackend/migrations /opt/systembackend/logs",
       label="ensure /opt/systembackend tree")

    print("\n=== systemd unit ===")
    unit = """[Unit]
Description=Launcher SystemBackend (axum)
After=network.target postgresql.service

[Service]
Type=simple
User=systembackend
Group=systembackend
WorkingDirectory=/opt/systembackend
ExecStart=/opt/systembackend/systembackend
Environment=LAUNCHER_CONFIG=/opt/systembackend/config.toml
Environment=RUST_LOG=info,sqlx=warn
Restart=on-failure
RestartSec=3
NoNewPrivileges=true
PrivateTmp=true

[Install]
WantedBy=multi-user.target
"""
    sh(f"cat > /etc/systemd/system/systembackend.service <<'UNIT'\n{unit}UNIT\nsystemctl daemon-reload",
       label="install systembackend.service")

    print("\n=== firewall ===")
    sh("ufw status | grep -q '1337/tcp' || ufw allow 1337/tcp", check=False)

    print("\n=== summary ===")
    sh("cat /opt/systembackend/.pg_creds 2>/dev/null || echo no-creds-file")
    sh("systemctl status postgresql --no-pager | head -n 3", check=False)
    sh("ls -la /opt/systembackend")

    cli.close()
    print("\n[*] remote setup done. 下一步: scripts/deploy.ps1 -BootstrapServer 触发 cargo build")
    return 0

if __name__ == "__main__":
    sys.exit(main())
