# 一次性：生成 launcher_deploy SSH key 并推送到服务器 authorized_keys
# 之后所有 ssh/scp 用 key，不再走密码

param(
    [string]$Host_ = '154.40.36.22',
    [int]$Port = 22,
    [string]$User = 'root'
)

$ErrorActionPreference = 'Stop'

$keyDir  = Join-Path $env:USERPROFILE '.ssh'
$keyPath = Join-Path $keyDir 'launcher_deploy'
$pubPath = "$keyPath.pub"

New-Item -ItemType Directory -Force -Path $keyDir | Out-Null

if (-not (Test-Path $keyPath)) {
    Write-Host '== generating ed25519 key =='
    & ssh-keygen -t ed25519 -f $keyPath -N '""' -C "launcher-deploy@$env:COMPUTERNAME"
}

Write-Host '== uploading public key =='
$pub = Get-Content $pubPath -Raw

$cmd = @"
mkdir -p ~/.ssh
chmod 700 ~/.ssh
grep -qxF '$($pub.Trim())' ~/.ssh/authorized_keys 2>/dev/null \
  || echo '$($pub.Trim())' >> ~/.ssh/authorized_keys
chmod 600 ~/.ssh/authorized_keys
echo OK
"@

# 第一次会要求输入密码 — 见 .deploy.local 的 password 字段
& ssh -p $Port "$User@$Host_" $cmd

Write-Host ''
Write-Host '完成。后续部署使用 SSH key:'
Write-Host "  ssh -i $keyPath -p $Port $User@$Host_"
Write-Host '建议立刻把 .deploy.local 里的 password 字段清空。'
