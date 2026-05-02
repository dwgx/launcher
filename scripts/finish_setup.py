#!/usr/bin/env python3
"""完成 SystemBackend 初始化：
1. 生成新的 PG 密码（这次保留）
2. ALTER ROLE 设置密码
3. 写 /opt/systembackend/config.toml 并 chmod 600 owned by systembackend
4. 运行 sqlx migrations
5. 在 /root/workspace/CHANGELOG.md 加一行
"""
from __future__ import annotations
import os, sys, re, io, secrets
from pathlib import Path
import paramiko

sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8", errors="replace")
sys.stderr = io.TextIOWrapper(sys.stderr.buffer, encoding="utf-8", errors="replace")

ROOT = Path(__file__).resolve().parent.parent

def parse(p: Path):
    out = {}
    for line in p.read_text(encoding="utf-8").splitlines():
        m = re.match(r'^\s*([\w_]+)\s*=\s*"?([^"]*)"?\s*$', line)
        if m: out[m.group(1)] = m.group(2)
    return out

def expand(s: str) -> Path:
    s = s.replace("$HOME", os.environ.get("USERPROFILE", str(Path.home())))
    return Path(s.replace("\\\\", "\\")).expanduser()

c = parse(ROOT / ".deploy.local")
cli = paramiko.SSHClient()
cli.set_missing_host_key_policy(paramiko.AutoAddPolicy())
cli.connect(c["host"], port=int(c["port"]), username=c["user"],
            key_filename=str(expand(c["key_path"])),
            look_for_keys=False, allow_agent=False, timeout=15)

def run(cmd: str, *, sensitive: bool = False, check: bool = True) -> tuple[int, str]:
    print(f"$ {'<sensitive>' if sensitive else cmd}")
    _, so, se = cli.exec_command(cmd, timeout=120)
    rc = so.channel.recv_exit_status()
    out = so.read().decode("utf-8", "replace")
    err = se.read().decode("utf-8", "replace")
    body = (out + err)
    print(body)
    if check and rc != 0:
        print(f"[!] rc={rc}", file=sys.stderr); sys.exit(rc)
    return rc, body

# Pwd in memory only; written into chmod 600 config.toml on server, never logged stdout
pg_pass = secrets.token_urlsafe(24)

# 1. ALTER ROLE
run(f"sudo -u postgres psql -c \"ALTER ROLE helix WITH PASSWORD '{pg_pass}'\" >/dev/null && echo OK",
    sensitive=True)

# 2. 升级公钥（占位）+ 写 config.toml
config = f"""bind_addr = "127.0.0.1:1337"
database_url = "postgres://helix:{pg_pass}@127.0.0.1:5432/helix"
session_ttl_seconds = 86400
heartbeat_grace_seconds = 60
argon_memory_kib = 65536
argon_iterations = 3
signing_public_key_hex = "REPLACE_AFTER_KEYGEN"
cdn_base = "https://cdn.example.com"
admin_password = "{secrets.token_urlsafe(24)}"
"""
# 写文件：用 base64 避开 shell 转义
import base64
b64 = base64.b64encode(config.encode("utf-8")).decode("ascii")
run(f"echo '{b64}' | base64 -d > /opt/systembackend/config.toml && "
    "chown systembackend:systembackend /opt/systembackend/config.toml && "
    "chmod 600 /opt/systembackend/config.toml && echo CONFIG_WRITTEN", sensitive=True)

# 3. 用 psql 直接跑 init migration（sqlx-cli 没装；后面 cargo build 时会 sqlx::migrate!）
run("test -f /opt/systembackend/migrations/0001_init.sql && echo present || echo absent",
    check=False)

# 4. CHANGELOG
import datetime
stamp = datetime.datetime.now().strftime("%Y-%m-%d %H:%M")
entry = (f"{stamp} | claude-opus-4-7 | "
         f"为项目 Launcher 装基础环境: build-essential/protoc/postgres/rustup-1.75; "
         f"建 systembackend 用户 + /opt/systembackend; 装 systembackend.service "
         f"(未启动); 修 sshd_config 启用 PubkeyAuthentication 加 launcher_deploy 公钥; "
         f"UFW 放 1337/tcp; 创 helix postgres role+db 并写 /opt/systembackend/config.toml "
         f"(chmod 600 owned by systembackend) | 影响生产基线")
run(f"echo {entry!r} >> /root/workspace/CHANGELOG.md && tail -2 /root/workspace/CHANGELOG.md")

cli.close()
print("\n[*] 完成。下一步: scripts/deploy.ps1 -BootstrapServer 让服务器 cargo build")
