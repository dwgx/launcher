param(
    [string]$Prefix = "audit",
    [string]$RemoteDir = "/opt/systembackend",
    [string]$Host_ = $env:LAUNCHER_DEPLOY_HOST,
    [string]$Port = $(if ($env:LAUNCHER_DEPLOY_PORT) { $env:LAUNCHER_DEPLOY_PORT } else { "22" }),
    [string]$User = $env:LAUNCHER_DEPLOY_USER,
    [string]$KeyPath = $(if ($env:LAUNCHER_DEPLOY_KEY) { $env:LAUNCHER_DEPLOY_KEY } else { "$HOME\.ssh\launcher_deploy" })
)

$ErrorActionPreference = 'Stop'
if (-not $Host_) {
    Write-Error "Set -Host_ or LAUNCHER_DEPLOY_HOST before cleaning audit data."
    exit 1
}
if (-not $User) {
    Write-Error "Set -User or LAUNCHER_DEPLOY_USER before cleaning audit data."
    exit 1
}

$root = Resolve-Path (Join-Path $PSScriptRoot '..\..')
$sqlPath = Join-Path $PSScriptRoot 'cleanup-audit-data.sql'
$key = [Environment]::ExpandEnvironmentVariables($KeyPath.Replace('$HOME', $env:USERPROFILE).Replace('/', '\'))
$knownHosts = Join-Path (Split-Path -Parent $key) 'launcher_deploy_known_hosts'
$prefixLike = "$Prefix%"

$remoteScript = @"
set -euo pipefail
DBURL=`$(sudo sed -n 's/^database_url *= *"\(.*\)"/\1/p' $RemoteDir/config.toml)
DB_MODE="system"
DB_CONTAINER=""
DB_NAME=""
if sudo test -f $RemoteDir/db.env; then
  DB_MODE=`$(sudo sed -n 's/^DB_MODE=//p' $RemoteDir/db.env)
  DB_CONTAINER=`$(sudo sed -n 's/^DB_CONTAINER=//p' $RemoteDir/db.env)
  DB_NAME=`$(sudo sed -n 's/^DB_NAME=//p' $RemoteDir/db.env)
fi
if [ "`$DB_MODE" = "docker" ]; then
  if ! sudo docker exec -i "`$DB_CONTAINER" \
      psql -U postgres -d "`$DB_NAME" -Atqc "SELECT to_regclass('public.users')" | grep -q users; then
    echo "audit cleanup skipped: users table not present"
    rm -f /tmp/launcher-cleanup-audit-data.sql
    exit 0
  fi
  sudo docker exec -i "`$DB_CONTAINER" \
    psql -U postgres -d "`$DB_NAME" -v ON_ERROR_STOP=1 -v prefix_like="$prefixLike" \
    < /tmp/launcher-cleanup-audit-data.sql
else
  if ! psql "`$DBURL" -Atqc "SELECT to_regclass('public.users')" | grep -q users; then
    echo "audit cleanup skipped: users table not present"
    rm -f /tmp/launcher-cleanup-audit-data.sql
    exit 0
  fi
  psql "`$DBURL" -v ON_ERROR_STOP=1 -v prefix_like="$prefixLike" -f - < /tmp/launcher-cleanup-audit-data.sql
fi
rm -f /tmp/launcher-cleanup-audit-data.sql
"@

scp -O -P $Port -i $key -o IdentitiesOnly=yes -o PreferredAuthentications=publickey `
    -o PasswordAuthentication=no -o UserKnownHostsFile=$knownHosts `
    -o StrictHostKeyChecking=accept-new $sqlPath "$User@${Host_}:/tmp/launcher-cleanup-audit-data.sql"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$b64 = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($remoteScript))
ssh -p $Port -i $key -o IdentitiesOnly=yes -o PreferredAuthentications=publickey `
    -o PasswordAuthentication=no -o UserKnownHostsFile=$knownHosts `
    -o StrictHostKeyChecking=accept-new "$User@$Host_" "printf '%s' '$b64' | base64 -d | bash"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
