param(
    [string]$Host_ = $env:LAUNCHER_DEPLOY_HOST,
    [string]$Port = $(if ($env:LAUNCHER_DEPLOY_PORT) { $env:LAUNCHER_DEPLOY_PORT } else { "22" }),
    [string]$User = $env:LAUNCHER_DEPLOY_USER,
    [string]$KeyPath = $(if ($env:LAUNCHER_DEPLOY_KEY) { $env:LAUNCHER_DEPLOY_KEY } else { "$HOME\.ssh\launcher_deploy" }),
    [string]$RemoteDir = "/opt/systembackend-audit",
    [string]$ProdDir = "/opt/systembackend",
    [string]$Service = "systembackend-audit",
    [string]$BindAddr = "0.0.0.0:1338",
    [string]$DbName = "helix_audit",
    [string]$DbUser = "helix_audit"
)

$ErrorActionPreference = 'Stop'
if (-not $Host_) {
    Write-Error "Set -Host_ or LAUNCHER_DEPLOY_HOST before deploying the audit service."
    exit 1
}
if (-not $User) {
    Write-Error "Set -User or LAUNCHER_DEPLOY_USER before deploying the audit service."
    exit 1
}

$root = Resolve-Path (Join-Path $PSScriptRoot '..\..')
$key = [Environment]::ExpandEnvironmentVariables($KeyPath.Replace('$HOME', $env:USERPROFILE).Replace('/', '\'))
$knownHosts = Join-Path (Split-Path -Parent $key) 'launcher_deploy_known_hosts'
$remoteSrc = "$RemoteDir/build_src"
$auditPort = ($BindAddr -split ':')[-1]

$sshArgs = @(
    '-p', $Port,
    '-i', $key,
    '-o', 'IdentitiesOnly=yes',
    '-o', 'PreferredAuthentications=publickey',
    '-o', 'PasswordAuthentication=no',
    '-o', "UserKnownHostsFile=$knownHosts",
    '-o', 'StrictHostKeyChecking=accept-new'
)
$scpArgs = @(
    '-O',
    '-P', $Port,
    '-i', $key,
    '-o', 'IdentitiesOnly=yes',
    '-o', 'PreferredAuthentications=publickey',
    '-o', 'PasswordAuthentication=no',
    '-o', "UserKnownHostsFile=$knownHosts",
    '-o', 'StrictHostKeyChecking=accept-new'
)

function Invoke-RemoteScript {
    param([Parameter(Mandatory=$true)][string]$Script)
    $b64 = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($Script))
    & ssh @sshArgs "$User@$Host_" "printf '%s' '$b64' | base64 -d | bash"
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

Write-Host '== create audit source archive ==' -ForegroundColor Cyan
$tmp = Join-Path ([IO.Path]::GetTempPath()) ("launcher-audit-src-{0}.tar.gz" -f ([guid]::NewGuid().ToString('N')))
& tar -C $root `
    --exclude=.git `
    --exclude=dist `
    --exclude=third_party `
    --exclude=build `
    --exclude=out `
    --exclude=SystemBackend/target `
    --exclude=.deploy.local `
    -czf $tmp .
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

try {
    Write-Host '== upload audit source ==' -ForegroundColor Cyan
    Invoke-RemoteScript "set -e`nmkdir -p /tmp/launcher-audit-upload $RemoteDir $remoteSrc"
    & scp @scpArgs $tmp "$User@${Host_}:/tmp/launcher-audit-upload/launcher-src.tar.gz"
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
finally {
    if (Test-Path $tmp) { Remove-Item -LiteralPath $tmp -Force }
}

$remoteScript = @"
set -e
. "`$HOME/.cargo/env"
mkdir -p $RemoteDir/media $RemoteDir/avatars $remoteSrc
rm -rf $remoteSrc
mkdir -p $remoteSrc
tar -xzf /tmp/launcher-audit-upload/launcher-src.tar.gz -C $remoteSrc
rm -f /tmp/launcher-audit-upload/launcher-src.tar.gz

DBPASS=`$(openssl rand -hex 24 | tr -d '\n')
if ! sudo -u postgres psql -Atqc "SELECT 1 FROM pg_roles WHERE rolname='$DbUser'" | grep -q 1; then
  sudo -u postgres psql -v ON_ERROR_STOP=1 -c "CREATE ROLE $DbUser LOGIN PASSWORD '`$DBPASS';"
else
  sudo -u postgres psql -v ON_ERROR_STOP=1 -c "ALTER ROLE $DbUser WITH PASSWORD '`$DBPASS';"
fi
sudo -u postgres psql -v ON_ERROR_STOP=1 -tc "SELECT 1 FROM pg_database WHERE datname='$DbName'" | grep -q 1 || \
  sudo -u postgres createdb -O $DbUser $DbName

PROD_CFG="$ProdDir/config.toml"
ADMIN_PASS=`$(openssl rand -hex 24 | tr -d '\n')
SIGNING=`$(sed -n 's/^signing_public_key_hex *= *"\(.*\)"/\1/p' "`$PROD_CFG")
CDN=`$(sed -n 's/^cdn_base *= *"\(.*\)"/\1/p' "`$PROD_CFG")
cat > $RemoteDir/config.toml <<EOF
bind_addr = "$BindAddr"
database_url = "postgres://${DbUser}:`$DBPASS@127.0.0.1:5432/${DbName}"
session_ttl_seconds = 86400
heartbeat_grace_seconds = 60
argon_memory_kib = 65536
argon_iterations = 3
signing_public_key_hex = "`$SIGNING"
cdn_base = "`$CDN"
admin_password = "`$ADMIN_PASS"
require_invite_code = false
media_image_max_bytes = 8388608
media_video_max_bytes = 33554432
media_generic_max_bytes = 104857600
sticker_per_user_limit = 50
EOF
chmod 600 $RemoteDir/config.toml

cd $remoteSrc/SystemBackend
DBURL=`$(sed -n 's/^database_url *= *"\(.*\)"/\1/p' $RemoteDir/config.toml)
for f in migrations/*.sql; do
  psql "`$DBURL" -v ON_ERROR_STOP=1 -f "`$f" >/tmp/launcher-audit-migrate.log 2>&1 || { cat /tmp/launcher-audit-migrate.log; exit 1; }
done
DATABASE_URL="`$DBURL" cargo build --release -p launcher-api -p launcher-signer
install -m 755 target/release/systembackend $RemoteDir/systembackend
install -d $RemoteDir/migrations
rsync -a --delete migrations/ $RemoteDir/migrations/
id -u systembackend >/dev/null 2>&1 || useradd --system --home $RemoteDir --shell /usr/sbin/nologin systembackend
chown -R systembackend:systembackend $RemoteDir

cat > /etc/systemd/system/$Service.service <<EOF
[Unit]
Description=Launcher SystemBackend Audit
After=network.target postgresql.service

[Service]
Type=simple
User=systembackend
Group=systembackend
WorkingDirectory=$RemoteDir
ExecStart=$RemoteDir/systembackend
Environment=LAUNCHER_CONFIG=$RemoteDir/config.toml
Environment=RUST_LOG=info,sqlx=warn,hyper=warn
Restart=on-failure
RestartSec=3
NoNewPrivileges=true
PrivateTmp=true
ProtectSystem=full
ProtectHome=true
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=multi-user.target
EOF
systemctl daemon-reload
systemctl enable $Service >/dev/null
systemctl restart $Service
sleep 3
systemctl is-active $Service
curl -s --max-time 10 http://127.0.0.1:$auditPort/api/market/categories >/dev/null
echo "audit_url=http://${Host_}:$auditPort"
"@

Write-Host '== remote audit build/deploy ==' -ForegroundColor Cyan
Invoke-RemoteScript $remoteScript
