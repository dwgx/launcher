#!/usr/bin/env python3
"""一次性：用 password 登录，确保 sshd 允许 pubkey auth + 把 launcher_deploy.pub 装进 authorized_keys。
之后所有脚本走 key auth。"""
from __future__ import annotations
import os, sys, re, io
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
host, port, user, pw = c["host"], int(c["port"]), c["user"], c["password"]
key  = expand(c["key_path"])
pub  = key.with_suffix(".pub").read_text(encoding="utf-8").strip()
if not pw:
    print("[!] .deploy.local 没 password，没法 bootstrap 回密码登录", file=sys.stderr); sys.exit(1)

cli = paramiko.SSHClient()
cli.set_missing_host_key_policy(paramiko.AutoAddPolicy())
cli.connect(host, port=port, username=user, password=pw,
            look_for_keys=False, allow_agent=False, timeout=15)

def run(cmd: str):
    _, so, se = cli.exec_command(cmd, timeout=30)
    rc = so.channel.recv_exit_status()
    return rc, so.read().decode(errors="replace"), se.read().decode(errors="replace")

print("=== 检查并修复 sshd_config ===")
# 备份 + 强制 pubkey 开启 + AuthenticationMethods 允许 publickey,password
fix = (
  "cp -n /etc/ssh/sshd_config /etc/ssh/sshd_config.bak.launcher; "
  "sed -ri 's/^[# ]*PubkeyAuthentication\\s+.*/PubkeyAuthentication yes/' /etc/ssh/sshd_config; "
  "grep -q '^PubkeyAuthentication' /etc/ssh/sshd_config || echo 'PubkeyAuthentication yes' >> /etc/ssh/sshd_config; "
  "sed -ri 's/^[# ]*AuthorizedKeysFile\\s+.*/AuthorizedKeysFile .ssh\\/authorized_keys/' /etc/ssh/sshd_config; "
  "grep -q '^AuthorizedKeysFile' /etc/ssh/sshd_config || echo 'AuthorizedKeysFile .ssh/authorized_keys' >> /etc/ssh/sshd_config; "
  # 不强制单一 method；保留两种 auth 都允许
  "sed -ri 's/^[# ]*AuthenticationMethods\\s+.*/AuthenticationMethods publickey,password publickey password/' /etc/ssh/sshd_config; "
  "sshd -t && systemctl reload ssh && echo SSHD_OK"
)
rc, out, err = run(fix)
print(out); print(err, file=sys.stderr)
if rc != 0 or "SSHD_OK" not in out: print("[!] sshd reload failed"); sys.exit(2)

print("=== 装公钥 ===")
push = (
  "mkdir -p ~/.ssh && chmod 700 ~/.ssh && "
  "touch ~/.ssh/authorized_keys && chmod 600 ~/.ssh/authorized_keys && "
  f"grep -qxF \"{pub}\" ~/.ssh/authorized_keys || echo \"{pub}\" >> ~/.ssh/authorized_keys && "
  "echo KEY_OK"
)
rc, out, err = run(push)
print(out); print(err, file=sys.stderr)

cli.close()
print("\n[*] 现在用 SSH key 验证：")
import subprocess
r = subprocess.run(["ssh", "-o", "BatchMode=yes", "-o", "StrictHostKeyChecking=accept-new",
                    "-i", str(key), "-p", str(port), f"{user}@{host}", "echo KEY_LOGIN_OK"],
                   capture_output=True, text=True, timeout=15)
print("rc=", r.returncode); print(r.stdout); print(r.stderr, file=sys.stderr)
