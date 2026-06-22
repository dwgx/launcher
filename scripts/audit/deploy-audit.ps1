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
    [string]$DbUser = "helix_audit",
    [ValidateSet("auto", "system", "docker")]
    [string]$DatabaseMode = "auto",
    [string]$DockerDbContainer = "launcher-audit-postgres",
    [string]$DockerDbPort = "15432"
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

function Assert-MigrationSqlUsesLf {
    $migrationDir = Join-Path $root 'SystemBackend\migrations'
    $bad = @()
    Get-ChildItem -Path $migrationDir -Filter '*.sql' | ForEach-Object {
        $bytes = [IO.File]::ReadAllBytes($_.FullName)
        for ($i = 0; $i -lt ($bytes.Length - 1); $i++) {
            if ($bytes[$i] -eq 13 -and $bytes[$i + 1] -eq 10) {
                $bad += $_.Name
                break
            }
        }
    }
    if ($bad.Count -gt 0) {
        Write-Error "SQL migration files must use LF line endings because sqlx checks migration bytes: $($bad -join ', ')"
        exit 1
    }
}

Write-Host '== verify migration line endings ==' -ForegroundColor Cyan
Assert-MigrationSqlUsesLf

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
    --exclude=.env `
    --exclude=.env.local `
    -czf $tmp .
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

try {
    Write-Host '== upload audit source ==' -ForegroundColor Cyan
    Invoke-RemoteScript "set -e`nsudo -n true`nmkdir -p /tmp/launcher-audit-upload`nsudo install -d -m 755 -o `"`$(id -u)`" -g `"`$(id -g)`" $RemoteDir $remoteSrc"
    & scp @scpArgs $tmp "$User@${Host_}:/tmp/launcher-audit-upload/launcher-src.tar.gz"
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
finally {
    if (Test-Path $tmp) { Remove-Item -LiteralPath $tmp -Force }
}

$remoteScript = @"
set -euo pipefail
if [ -f "`$HOME/.cargo/env" ]; then
  . "`$HOME/.cargo/env"
fi
sudo -n true
SELF_UID=`$(id -u)
SELF_GID=`$(id -g)
sudo install -d -m 755 -o "`$SELF_UID" -g "`$SELF_GID" $RemoteDir $RemoteDir/media $RemoteDir/avatars
sudo rm -rf $remoteSrc
sudo install -d -m 755 -o "`$SELF_UID" -g "`$SELF_GID" $remoteSrc
tar -xzf /tmp/launcher-audit-upload/launcher-src.tar.gz -C $remoteSrc
rm -f /tmp/launcher-audit-upload/launcher-src.tar.gz
sudo systemctl stop $Service >/dev/null 2>&1 || true

DBPASS=`$(openssl rand -hex 24 | tr -d '\n')
REQUESTED_DB_MODE="$DatabaseMode"
DB_MODE=""
DB_PORT="5432"
DB_CONTAINER=""

if { [ "`$REQUESTED_DB_MODE" = "system" ] || [ "`$REQUESTED_DB_MODE" = "auto" ]; } \
   && getent passwd postgres >/dev/null 2>&1 \
   && command -v psql >/dev/null 2>&1 \
   && systemctl is-active --quiet postgresql; then
  DB_MODE="system"
fi

if [ -z "`$DB_MODE" ] && { [ "`$REQUESTED_DB_MODE" = "docker" ] || [ "`$REQUESTED_DB_MODE" = "auto" ]; }; then
  if command -v docker >/dev/null 2>&1; then
    DB_MODE="docker"
    DB_CONTAINER="$DockerDbContainer"
    DB_PORT="$DockerDbPort"
  fi
fi

if [ -z "`$DB_MODE" ]; then
  echo "No usable PostgreSQL backend. Install/start PostgreSQL, or install Docker and rerun with -DatabaseMode docker." >&2
  exit 20
fi

if [ "`$DB_MODE" = "system" ]; then
  if ! sudo -u postgres psql -Atqc "SELECT 1 FROM pg_roles WHERE rolname='$DbUser'" | grep -q 1; then
    sudo -u postgres psql -v ON_ERROR_STOP=1 -c "CREATE ROLE $DbUser LOGIN PASSWORD '`$DBPASS';"
  else
    sudo -u postgres psql -v ON_ERROR_STOP=1 -c "ALTER ROLE $DbUser WITH PASSWORD '`$DBPASS';"
  fi
  sudo -u postgres psql -v ON_ERROR_STOP=1 -d postgres -c "SELECT pg_terminate_backend(pid) FROM pg_stat_activity WHERE datname='$DbName';" >/dev/null
  sudo -u postgres dropdb --if-exists $DbName
  sudo -u postgres createdb -O $DbUser $DbName
else
  if ! sudo docker image inspect postgres:17-alpine >/dev/null 2>&1; then
    sudo docker pull postgres:17-alpine
  fi
  if sudo docker ps -a --format '{{.Names}}' | grep -Fxq "`$DB_CONTAINER"; then
    sudo docker start "`$DB_CONTAINER" >/dev/null
  else
    sudo docker run -d \
      --name "`$DB_CONTAINER" \
      --restart unless-stopped \
      -e POSTGRES_PASSWORD="`$DBPASS" \
      -p "127.0.0.1:`$DB_PORT:5432" \
      postgres:17-alpine >/dev/null
  fi
  for _ in `$(seq 1 60); do
    if sudo docker exec "`$DB_CONTAINER" pg_isready -U postgres >/dev/null 2>&1; then
      break
    fi
    sleep 1
  done
  sudo docker exec "`$DB_CONTAINER" pg_isready -U postgres >/dev/null
  if ! sudo docker exec "`$DB_CONTAINER" psql -U postgres -Atqc "SELECT 1 FROM pg_roles WHERE rolname='$DbUser'" | grep -q 1; then
    sudo docker exec "`$DB_CONTAINER" psql -U postgres -v ON_ERROR_STOP=1 -c "CREATE ROLE $DbUser LOGIN PASSWORD '`$DBPASS';"
  else
    sudo docker exec "`$DB_CONTAINER" psql -U postgres -v ON_ERROR_STOP=1 -c "ALTER ROLE $DbUser WITH PASSWORD '`$DBPASS';"
  fi
  sudo docker exec "`$DB_CONTAINER" psql -U postgres -d postgres -v ON_ERROR_STOP=1 -c "SELECT pg_terminate_backend(pid) FROM pg_stat_activity WHERE datname='$DbName';" >/dev/null
  sudo docker exec "`$DB_CONTAINER" dropdb -U postgres --if-exists $DbName
  sudo docker exec "`$DB_CONTAINER" createdb -U postgres -O $DbUser $DbName
fi

PROD_CFG="$ProdDir/config.toml"
ADMIN_PASS=`$(openssl rand -hex 24 | tr -d '\n')
SIGNING=""
CDN=""
if [ -f "`$PROD_CFG" ]; then
  SIGNING=`$(sed -n 's/^signing_public_key_hex *= *"\(.*\)"/\1/p' "`$PROD_CFG" || true)
  CDN=`$(sed -n 's/^cdn_base *= *"\(.*\)"/\1/p' "`$PROD_CFG" || true)
fi
SIGNING=`${SIGNING:-REPLACE_AFTER_KEYGEN}
CDN=`${CDN:-http://${Host_}:$auditPort}
DBURL="postgres://${DbUser}:`$DBPASS@127.0.0.1:`$DB_PORT/${DbName}"
CFG_TMP=`$(mktemp /tmp/launcher-audit-config.XXXXXX)
cat > "`$CFG_TMP" <<EOF
bind_addr = "$BindAddr"
database_url = "`$DBURL"
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
media_root = "$RemoteDir/media"
avatar_root = "$RemoteDir/avatars"
sticker_per_user_limit = 50
EOF
sudo install -m 600 "`$CFG_TMP" $RemoteDir/config.toml
rm -f "`$CFG_TMP"
DB_ENV_TMP=`$(mktemp /tmp/launcher-audit-db.XXXXXX)
cat > "`$DB_ENV_TMP" <<EOF
DB_MODE=`$DB_MODE
DB_CONTAINER=`$DB_CONTAINER
DB_USER=$DbUser
DB_NAME=$DbName
DB_PORT=`$DB_PORT
EOF
sudo install -m 600 "`$DB_ENV_TMP" $RemoteDir/db.env
rm -f "`$DB_ENV_TMP"

cd $remoteSrc/SystemBackend
for f in migrations/*.sql; do
  if [ "`$DB_MODE" = "docker" ]; then
    sudo docker exec -i -e PGPASSWORD="`$DBPASS" "`$DB_CONTAINER" \
      psql -h 127.0.0.1 -U $DbUser -d $DbName -v ON_ERROR_STOP=1 \
      < "`$f" >/tmp/launcher-audit-migrate.log 2>&1 || { cat /tmp/launcher-audit-migrate.log; exit 1; }
  else
    psql "`$DBURL" -v ON_ERROR_STOP=1 -f "`$f" >/tmp/launcher-audit-migrate.log 2>&1 || { cat /tmp/launcher-audit-migrate.log; exit 1; }
  fi
done
DATABASE_URL="`$DBURL" cargo build --release -p launcher-api -p launcher-signer
sudo install -m 755 target/release/systembackend $RemoteDir/systembackend
sudo install -d $RemoteDir/migrations
sudo rsync -a --delete migrations/ $RemoteDir/migrations/
sudo id -u systembackend >/dev/null 2>&1 || sudo useradd --system --home $RemoteDir --shell /usr/sbin/nologin systembackend
sudo chown -R systembackend:systembackend $RemoteDir

UNIT_AFTER="network.target postgresql.service"
UNIT_WANTS="postgresql.service"
if [ "`$DB_MODE" = "docker" ]; then
  UNIT_AFTER="network.target docker.service"
  UNIT_WANTS="docker.service"
fi
cat > /tmp/$Service.service <<EOF
[Unit]
Description=Launcher SystemBackend Audit
After=`$UNIT_AFTER
Wants=`$UNIT_WANTS

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
sudo install -m 644 /tmp/$Service.service /etc/systemd/system/$Service.service
rm -f /tmp/$Service.service
sudo systemctl daemon-reload
sudo systemctl enable $Service >/dev/null
sudo systemctl restart $Service
sleep 3
sudo systemctl is-active $Service
curl -s --max-time 10 http://127.0.0.1:$auditPort/api/market/categories >/dev/null
echo "audit_url=http://${Host_}:$auditPort"
echo "database_mode=`$DB_MODE"
"@

Write-Host '== remote audit build/deploy ==' -ForegroundColor Cyan
Invoke-RemoteScript $remoteScript
