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
host       = "154.40.36.22"
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
set -e
DBURL=$(sed -n 's/^database_url *= *"\(.*\)"/\1/p' __REMOTE_DIR__/config.toml)
cd __REMOTE_SRC__/SystemBackend
for f in migrations/*.sql; do
  echo "applying $f"
  psql "$DBURL" -v ON_ERROR_STOP=1 -f "$f" >/tmp/launcher-migrate.log 2>&1 || { cat /tmp/launcher-migrate.log; exit 1; }
done
'@.Replace('__REMOTE_DIR__', $remoteDir).Replace('__REMOTE_SRC__', $remoteSrc)
    Invoke-RemoteScript $migrateScript
}

if (-not $SkipBuild) {
    Write-Host '== cargo build --release on server ==' -ForegroundColor Cyan
    $buildScript = @'
set -e
. "$HOME/.cargo/env"
DBURL=$(sed -n 's/^database_url *= *"\(.*\)"/\1/p' __REMOTE_DIR__/config.toml)
cd __REMOTE_SRC__/SystemBackend
DATABASE_URL="$DBURL" cargo build --release -p launcher-api -p launcher-signer
install -m 755 target/release/systembackend __REMOTE_DIR__/systembackend
install -d __REMOTE_DIR__/migrations
rsync -a --delete migrations/ __REMOTE_DIR__/migrations/
chown -R systembackend:systembackend __REMOTE_DIR__
'@.Replace('__REMOTE_DIR__', $remoteDir).Replace('__REMOTE_SRC__', $remoteSrc)
    Invoke-RemoteScript $buildScript
}

Write-Host '== restart service ==' -ForegroundColor Cyan
Invoke-Remote "systemctl restart $svc && sleep 3 && systemctl status $svc --no-pager -l && curl -k --max-time 10 https://154.40.36.22:1337/api/market/categories >/dev/null"
