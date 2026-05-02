#!/usr/bin/env python3
"""一次性脚本：用 .deploy.local 的密码登录，推 SSH key，做环境侦察。

执行后：
  - $HOME/.ssh/launcher_deploy 存在
  - 公钥已加入 root@server:~/.ssh/authorized_keys
  - 服务器侦察结果打印到 stdout

完成后请把 .deploy.local 的 password 清空。
"""
from __future__ import annotations
import os, sys, re, subprocess, io
from pathlib import Path

# Why: Windows 控制台默认 GBK，远端命令输出可能含 emoji/CJK，强制 UTF-8 防 UnicodeEncodeError
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8", errors="replace")
sys.stderr = io.TextIOWrapper(sys.stderr.buffer, encoding="utf-8", errors="replace")

import paramiko

ROOT = Path(__file__).resolve().parent.parent
LOCAL_CFG = ROOT / ".deploy.local"

def parse_local() -> dict:
    out = {}
    for line in LOCAL_CFG.read_text(encoding="utf-8").splitlines():
        m = re.match(r'^\s*([\w_]+)\s*=\s*"?([^"]*)"?\s*$', line)
        if m: out[m.group(1)] = m.group(2)
    return out

def expand(p: str) -> Path:
    p = p.replace("$HOME", os.environ.get("USERPROFILE", str(Path.home())))
    return Path(p.replace("\\\\", "\\")).expanduser()

def ensure_keypair(key_path: Path) -> tuple[Path, Path]:
    pub = key_path.with_suffix(".pub")
    if not key_path.exists():
        key_path.parent.mkdir(parents=True, exist_ok=True)
        print(f"[*] generating ed25519 -> {key_path}")
        subprocess.check_call([
            "ssh-keygen", "-t", "ed25519",
            "-f", str(key_path), "-N", "",
            "-C", f"launcher-deploy@{os.environ.get('COMPUTERNAME','win')}",
        ])
    return key_path, pub

def main() -> int:
    cfg = parse_local()
    host = cfg["host"]; port = int(cfg["port"]); user = cfg["user"]
    pw   = cfg.get("password", "")
    key_path = expand(cfg["key_path"])
    if not pw:
        print("[!] .deploy.local 没有 password — 假定 SSH key 已就位，直接探测", file=sys.stderr)

    priv, pub = ensure_keypair(key_path)
    pub_text = pub.read_text(encoding="utf-8").strip()

    cli = paramiko.SSHClient()
    cli.set_missing_host_key_policy(paramiko.AutoAddPolicy())

    # 1) 先试 SSH key
    used_password = False
    try:
        cli.connect(host, port=port, username=user,
                    key_filename=str(priv), look_for_keys=False, allow_agent=False, timeout=10)
        print("[*] SSH key 认证 OK")
    except paramiko.AuthenticationException:
        if not pw:
            print("[!] 既无 key 又无 password，放弃", file=sys.stderr); return 2
        print("[*] key 不工作，回退到 password 一次性 bootstrap")
        cli.connect(host, port=port, username=user, password=pw,
                    look_for_keys=False, allow_agent=False, timeout=10)
        used_password = True

    def run(cmd: str) -> tuple[int, str, str]:
        _, so, se = cli.exec_command(cmd, timeout=60)
        rc = so.channel.recv_exit_status()
        return rc, so.read().decode(errors="replace"), se.read().decode(errors="replace")

    if used_password:
        # 推公钥
        cmd = (
            "mkdir -p ~/.ssh && chmod 700 ~/.ssh && "
            "touch ~/.ssh/authorized_keys && chmod 600 ~/.ssh/authorized_keys && "
            f"grep -qxF \"{pub_text}\" ~/.ssh/authorized_keys || "
            f"echo \"{pub_text}\" >> ~/.ssh/authorized_keys"
        )
        rc, out, err = run(cmd)
        if rc != 0:
            print(f"[!] 推 key 失败: rc={rc} err={err}", file=sys.stderr); return 3
        print("[*] 公钥已写入 authorized_keys")

    # 侦察
    print("\n=== Recon ===")
    for label, cmd in [
        ("uname",            "uname -a"),
        ("os-release",       "head -n 5 /etc/os-release"),
        ("uptime",           "uptime"),
        ("disk",             "df -h / | tail -n 1"),
        ("memory",           "free -h | head -n 2"),
        ("cargo",            "cargo --version 2>&1 || echo missing"),
        ("rustc",            "rustc --version 2>&1 || echo missing"),
        ("protoc",           "protoc --version 2>&1 || echo missing"),
        ("postgres",         "psql --version 2>&1 || echo missing"),
        ("postgres-running", "systemctl is-active postgresql 2>&1 || echo unknown"),
        ("1panel",           "command -v 1pctl >/dev/null && 1pctl status 2>&1 | head -n 4 || echo missing"),
        ("port-1337",        "ss -tlnp 2>/dev/null | grep -E ':1337' || echo not-listening"),
        ("git",              "git --version 2>&1 || echo missing"),
        ("systemd",          "systemctl --version | head -n 1"),
        ("svc-systembackend","systemctl status systembackend --no-pager 2>&1 | head -n 3 || echo not-installed"),
    ]:
        rc, out, err = run(cmd)
        out = (out + err).strip()
        print(f"-- {label} (rc={rc})\n{out}\n")

    cli.close()
    print("\n[*] done. 现在可以编辑 .deploy.local 把 password 清空（保留 key_path 即可）。")
    return 0

if __name__ == "__main__":
    sys.exit(main())
