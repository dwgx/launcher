# Deploy SystemBackend to the VPS.
#
# The canonical path is server-side build:
# 1. Pack the current checkout, excluding local artifacts and secrets.
# 2. Upload to remote_dir/build_src.
# 3. Apply migrations against the server PostgreSQL database.
# 4. Build with DATABASE_URL so sqlx::query! can validate against the schema.
# 5. Install the binary and restart systemd.

param(
    [switch]$SkipBuild = $false,
    [switch]$SkipMigrations = $false
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$local = Join-Path $root '.deploy.local'

if (-not (Test-Path $local)) {
    Write-Host 'Create .deploy.local in the repository root first:'
    Write-Host @'
host       = "<homecloud-host-or-ip>"
port       = "22"
user       = "root"
password   = ""
key_path   = "$HOME\\.ssh\\launcher_deploy"
remote_dir = "/opt/systembackend"
service    = "systembackend"
'@
    exit 1
}

$conf = @{}
Get-Content $local | ForEach-Object {
    if ($_ -match '^\s*([\w_]+)\s*=\s*"?([^"]*)"?\s*$') {
        $conf[$matches[1]] = $matches[2]
    }
}

$h = $conf['host']
$port = $conf['port']
$user = $conf['user']
$keyPath = [Environment]::ExpandEnvironmentVariables(
    $conf['key_path'].Replace('$HOME', $env:USERPROFILE).Replace('/', '\')
)
$remoteDir = $conf['remote_dir']
$svc = $conf['service']
$remoteSrc = "$remoteDir/build_src"

if (-not (Test-Path $keyPath)) {
    Write-Error "SSH key not found: $keyPath. Run scripts/setup-ssh-key.ps1 first."
    exit 1
}

$knownHosts = Join-Path (Split-Path -Parent $keyPath) 'launcher_deploy_known_hosts'
$sshBaseArgs = @(
    '-p', $port,
    '-i', $keyPath,
    '-o', "UserKnownHostsFile=$knownHosts",
    '-o', 'StrictHostKeyChecking=accept-new'
)
$scpBaseArgs = @(
    '-P', $port,
    '-i', $keyPath,
    '-o', "UserKnownHostsFile=$knownHosts",
    '-o', 'StrictHostKeyChecking=accept-new'
)

function Invoke-Remote {
    param([Parameter(Mandatory=$true)][string]$Command)
    & ssh @sshBaseArgs "$user@$h" $Command
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

function Invoke-RemoteScript {
    param([Parameter(Mandatory=$true)][string]$Script)
    $b64 = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($Script))
    & ssh @sshBaseArgs "$user@$h" "printf '%s' '$b64' | base64 -d | bash"
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

Write-Host '== create source archive ==' -ForegroundColor Cyan
$tmp = Join-Path ([IO.Path]::GetTempPath()) ("launcher-src-{0}.tar.gz" -f ([guid]::NewGuid().ToString('N')))
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
    Write-Host '== upload source ==' -ForegroundColor Cyan
    Invoke-Remote "mkdir -p /tmp/launcher-upload $remoteSrc"
    & scp @scpBaseArgs $tmp "$user@${h}:/tmp/launcher-upload/launcher-src.tar.gz"
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
finally {
    if (Test-Path $tmp) { Remove-Item -LiteralPath $tmp -Force }
}

Write-Host '== extract source ==' -ForegroundColor Cyan
Invoke-Remote "rm -rf $remoteSrc && mkdir -p $remoteSrc && tar -xzf /tmp/launcher-upload/launcher-src.tar.gz -C $remoteSrc && rm -f /tmp/launcher-upload/launcher-src.tar.gz"

if (-not $SkipMigrations) {
    Write-Host '== apply migrations ==' -ForegroundColor Cyan
    $migrateScript = @'
set -euo pipefail
DBURL=$(sed -n 's/^database_url *= *"\(.*\)"/\1/p' __REMOTE_DIR__/config.toml)
if [ -z "$DBURL" ]; then
  echo "database_url was not found in __REMOTE_DIR__/config.toml" >&2
  exit 20
fi
cd __REMOTE_SRC__/SystemBackend

SUDO=""
if [ "$(id -u)" -ne 0 ] && command -v sudo >/dev/null 2>&1; then
  SUDO="sudo"
fi

find_bin() {
  name="$1"
  if command -v "$name" >/dev/null 2>&1; then
    command -v "$name"
    return 0
  fi
  for p in "/usr/local/bin/$name" "/usr/bin/$name" "/bin/$name"; do
    if [ -x "$p" ]; then
      printf '%s\n' "$p"
      return 0
    fi
  done
  return 1
}

PSQL_BIN=$(find_bin psql || true)
DB_NAME=$(printf '%s' "$DBURL" | sed -E 's#^.*@[^/]+/([^?]+).*$#\1#')
DB_PORT=$(printf '%s' "$DBURL" | sed -nE 's#^.*@[^/:]+:([0-9]+)/.*$#\1#p')
DB_CONTAINER=""
if [ -z "$PSQL_BIN" ] && [ -n "$DB_PORT" ] && command -v docker >/dev/null 2>&1; then
  DB_CONTAINER=$($SUDO docker ps --format '{{.Names}} {{.Ports}}' 2>/dev/null \
    | awk -v port="$DB_PORT" '
        $0 ~ "127\\.0\\.0\\.1:" port "->5432" { print $1; exit }
        $0 ~ "0\\.0\\.0\\.0:" port "->5432" { print $1; exit }
        $0 ~ ":::" port "->5432" { print $1; exit }
      ')
fi

run_migration() {
  f="$1"
  if [ -n "$PSQL_BIN" ]; then
    "$PSQL_BIN" "$DBURL" -v ON_ERROR_STOP=1 -f "$f" >/tmp/launcher-migrate.log 2>&1 \
      || { cat /tmp/launcher-migrate.log; exit 1; }
    return
  fi
  if [ -n "$DB_CONTAINER" ] && [ -n "$DB_NAME" ]; then
    $SUDO docker exec -i "$DB_CONTAINER" psql -U postgres -d "$DB_NAME" -v ON_ERROR_STOP=1 \
      < "$f" >/tmp/launcher-migrate.log 2>&1 || { cat /tmp/launcher-migrate.log; exit 1; }
    return
  fi
  echo "No usable psql client found. Install postgresql-client, or run the PostgreSQL database in a Docker container with a port matching database_url." >&2
  exit 21
}

for f in migrations/*.sql; do
  echo "applying $f"
  run_migration "$f"
done
'@.Replace('__REMOTE_DIR__', $remoteDir).Replace('__REMOTE_SRC__', $remoteSrc)
    Invoke-RemoteScript $migrateScript
}

if (-not $SkipBuild) {
    Write-Host '== cargo build --release on server ==' -ForegroundColor Cyan
    $buildScript = @'
set -euo pipefail
if [ -f "$HOME/.cargo/env" ]; then
  . "$HOME/.cargo/env"
fi
CARGO_BIN=$(command -v cargo || true)
if [ -z "$CARGO_BIN" ]; then
  for p in "$HOME/.cargo/bin/cargo" "/usr/local/bin/cargo" "/usr/bin/cargo"; do
    if [ -x "$p" ]; then
      CARGO_BIN="$p"
      break
    fi
  done
fi
if [ -z "$CARGO_BIN" ]; then
  echo "cargo was not found. Install Rust/Cargo or make cargo available in PATH." >&2
  exit 22
fi
DBURL=$(sed -n 's/^database_url *= *"\(.*\)"/\1/p' __REMOTE_DIR__/config.toml)
cd __REMOTE_SRC__/SystemBackend
DATABASE_URL="$DBURL" "$CARGO_BIN" build --release -p launcher-api -p launcher-signer
install -m 755 target/release/systembackend __REMOTE_DIR__/systembackend
install -d __REMOTE_DIR__/migrations
rsync -a --delete migrations/ __REMOTE_DIR__/migrations/
chown -R systembackend:systembackend __REMOTE_DIR__
'@.Replace('__REMOTE_DIR__', $remoteDir).Replace('__REMOTE_SRC__', $remoteSrc)
    Invoke-RemoteScript $buildScript
}

Write-Host '== restart service ==' -ForegroundColor Cyan
# 健康检查默认做 TLS 校验。证书与连接地址不匹配（如用 IP 连域名证书）时，
# 在 .deploy.local 设 health_host 指向证书匹配的域名，或显式设 health_insecure = "1"。
$healthHost = if ($conf['health_host']) { $conf['health_host'] } else { $h }
$curlFlags = if ($conf['health_insecure'] -eq '1') { '-k --max-time 10' } else { '--max-time 10' }
Invoke-Remote "systemctl restart $svc && sleep 3 && systemctl status $svc --no-pager -l && curl $curlFlags https://$($healthHost):1337/api/market/categories >/dev/null"
