# 把本地 SystemBackend release 二进制 + migrations + config 同步到服务器并重启 systemd
#
# 第一次运行：用密码（来自 .deploy.local），随后自动追加 SSH key 走 key 认证
# 后续运行：纯 SSH key

param(
    [switch]$NoBuild = $false,
    [switch]$BootstrapServer = $false
)

$ErrorActionPreference = 'Stop'
$root  = Split-Path -Parent $PSScriptRoot
$local = Join-Path $root '.deploy.local'

if (-not (Test-Path $local)) {
    Write-Host '请先在仓库根创建 .deploy.local，内容如下（已 gitignored）:'
    Write-Host @'
[server]
host = "154.40.36.22"
port = 22
user = "root"
password = "..."        # 仅首次部署用，bootstrap 后清掉
key_path = "$HOME/.ssh/launcher_deploy"
remote_dir = "/opt/systembackend"
service = "systembackend"
'@
    exit 1
}
$cfg = Get-Content $local | Out-String | ConvertFrom-StringData -ErrorAction SilentlyContinue
# 简单 toml 用 powershell-yaml 包不够便，自己写两行解析
$conf = @{}
Get-Content $local | ForEach-Object {
    if ($_ -match '^\s*([\w_]+)\s*=\s*"?([^"]+)"?\s*$') {
        $conf[$matches[1]] = $matches[2]
    }
}
$h = $conf['host']; $port = $conf['port']; $user = $conf['user']
$keyPath = [Environment]::ExpandEnvironmentVariables($conf['key_path'].Replace('$HOME', $env:USERPROFILE))
$remoteDir = $conf['remote_dir']; $svc = $conf['service']

if (-not $NoBuild) {
    Write-Host '== cargo build --release ==' -ForegroundColor Cyan
    Push-Location (Join-Path $root 'SystemBackend')
    cargo build --release -p launcher-api
    if ($LASTEXITCODE -ne 0) { Pop-Location; exit 1 }
    Pop-Location
}

$bin = Join-Path $root 'SystemBackend\target\release\systembackend.exe'
if (-not (Test-Path $bin)) {
    # Windows 上交叉编译 Linux 复杂，建议直接服务器 cargo build
    Write-Warning '本地未发现 systembackend Linux 二进制；切换到服务器侧 cargo build'
    Write-Host '请用 -BootstrapServer 让脚本远端 git pull + cargo build'
    if (-not $BootstrapServer) { exit 1 }
}

if ($BootstrapServer) {
    Write-Host '== bootstrap: server-side cargo build ==' -ForegroundColor Cyan
    & ssh -p $port -i $keyPath "$user@$h" @"
set -e
mkdir -p $remoteDir
cd $remoteDir
if [ ! -d .git ]; then git clone https://github.com/dwgx1337/launcher.git .; else git pull; fi
cd SystemBackend
cargo build --release -p launcher-api
install -m 755 target/release/systembackend $remoteDir/systembackend
install -m 644 -D crates/api/Cargo.toml $remoteDir/.bin-info
install -d $remoteDir/migrations
cp -r migrations/. $remoteDir/migrations/
[ -f $remoteDir/config.toml ] || cp SystemBackend/config/config.example.toml $remoteDir/config.toml
chown -R systembackend:systembackend $remoteDir || true
systemctl daemon-reload
systemctl enable systembackend
systemctl restart $svc
systemctl status $svc --no-pager
"@
    exit $LASTEXITCODE
}

Write-Host '== rsync binary + migrations ==' -ForegroundColor Cyan
& scp -P $port -i $keyPath $bin "$user@${h}:${remoteDir}/systembackend"
& scp -P $port -i $keyPath -r (Join-Path $root 'SystemBackend\migrations') "$user@${h}:${remoteDir}/"

Write-Host '== restart service ==' -ForegroundColor Cyan
& ssh -p $port -i $keyPath "$user@$h" "systemctl restart $svc && systemctl status $svc --no-pager"
