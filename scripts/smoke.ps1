param(
    [ValidateSet('audit', 'production', 'custom')]
    [string]$Target = 'audit',
    [ValidateSet('full', 'api', 'readonly', 'desktop', 'preflight', 'chat')]
    [string]$Depth = 'full',
    [ValidateSet('', 'none', 'launch-only', 'foreground-visual')]
    [string]$DesktopUiMode = '',
    [string]$RunId = ("smoke" + (Get-Date -Format 'yyyyMMddHHmmss')),
    [string]$BaseUrl = $env:LAUNCHER_AUDIT_BASE,
    [string]$JsonReport = "",
    [switch]$SkipDesktopBuild = $false,
    [switch]$SkipDesktopLaunch = $false,
    [switch]$KeepAuditData = $false,
    [switch]$AllowProductionWrites = $false,
    [ValidateSet('auto', 'system', 'docker')]
    [string]$DatabaseMode = 'auto'
)

$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..')
$startedAt = Get-Date
$script:ExitCode = 0
$script:Results = New-Object System.Collections.Generic.List[object]
$script:UiTelemetry = [ordered]@{
    foreground_used = $false
    mouse_used = $false
    topmost_used = $false
}

function Read-DeployConfig {
    $path = Join-Path $root '.deploy.local'
    $conf = @{}
    if (Test-Path $path) {
        Get-Content $path | ForEach-Object {
            if ($_ -match '^\s*([\w_]+)\s*=\s*"?([^"]*)"?\s*$') {
                $conf[$matches[1]] = $matches[2]
            }
        }
    }
    return $conf
}

function Add-StepResult {
    param(
        [string]$Name,
        [string]$Status,
        [datetime]$Started,
        [datetime]$Ended,
        [string]$Message = ""
    )
    $script:Results.Add([ordered]@{
        name = $Name
        status = $Status
        started_at = $Started.ToUniversalTime().ToString('o')
        ended_at = $Ended.ToUniversalTime().ToString('o')
        duration_ms = [int](($Ended - $Started).TotalMilliseconds)
        message = $Message
    }) | Out-Null
}

function Invoke-Native {
    param(
        [Parameter(Mandatory=$true)][string]$File,
        [string[]]$Arguments = @(),
        [hashtable]$Environment = $null
    )
    $psi = [System.Diagnostics.ProcessStartInfo]::new()
    $psi.FileName = $File
    $psi.WorkingDirectory = $root
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.Arguments = ($Arguments | ForEach-Object {
        $arg = [string]$_
        if ($arg -notmatch '[\s"]') { return $arg }
        '"' + ($arg -replace '(\\*)"', '$1$1\"' -replace '(\\+)$', '$1$1') + '"'
    }) -join ' '
    if ($null -ne $Environment) {
        foreach ($key in $Environment.Keys) {
            if ($null -ne $Environment[$key]) {
                $psi.EnvironmentVariables[$key] = [string]$Environment[$key]
            }
        }
    }

    $p = [System.Diagnostics.Process]::Start($psi)
    $stdout = $p.StandardOutput.ReadToEnd()
    $stderr = $p.StandardError.ReadToEnd()
    $p.WaitForExit()
    if ($stdout.Trim().Length -gt 0) { Write-Host $stdout.TrimEnd() }
    if ($stderr.Trim().Length -gt 0) { Write-Host $stderr.TrimEnd() -ForegroundColor DarkYellow }
    if ($p.ExitCode -ne 0) {
        throw "$File exited with $($p.ExitCode)"
    }
    return [ordered]@{
        exit_code = $p.ExitCode
        stdout = $stdout
        stderr = $stderr
    }
}

function Invoke-SmokeStep {
    param(
        [Parameter(Mandatory=$true)][string]$Name,
        [Parameter(Mandatory=$true)][int]$ExitCodeOnFail,
        [Parameter(Mandatory=$true)][scriptblock]$Block
    )
    Write-Host "== $Name ==" -ForegroundColor Cyan
    $s = Get-Date
    try {
        $result = & $Block
        $msg = ""
        if ($result -is [string]) { $msg = $result }
        elseif ($null -ne $result -and $result.Contains('message')) { $msg = [string]$result['message'] }
        Add-StepResult -Name $Name -Status 'pass' -Started $s -Ended (Get-Date) -Message $msg
        Write-Host "PASS $Name" -ForegroundColor Green
    }
    catch {
        $script:ExitCode = $ExitCodeOnFail
        $msg = $_.Exception.Message
        Add-StepResult -Name $Name -Status 'fail' -Started $s -Ended (Get-Date) -Message $msg
        Write-Host "FAIL $Name | $msg" -ForegroundColor Red
        throw
    }
}

function Write-SmokeReport {
    param([string]$Path, [string]$Result)
    if ([string]::IsNullOrWhiteSpace($Path)) { return }
    $report = [ordered]@{
        run_id = $RunId
        target = $Target
        depth = $Depth
        desktop_ui_mode = $DesktopUiMode
        base_url = $script:ResolvedBaseUrl
        started_at = $startedAt.ToUniversalTime().ToString('o')
        ended_at = (Get-Date).ToUniversalTime().ToString('o')
        result = $Result
        exit_code = $script:ExitCode
        foreground_used = [bool]$script:UiTelemetry.foreground_used
        mouse_used = [bool]$script:UiTelemetry.mouse_used
        topmost_used = [bool]$script:UiTelemetry.topmost_used
        steps = $script:Results
    }
    $dir = Split-Path -Parent $Path
    if ($dir -and -not (Test-Path $dir)) {
        New-Item -ItemType Directory -Force -Path $dir | Out-Null
    }
    $report | ConvertTo-Json -Depth 8 | Set-Content -Encoding UTF8 -Path $Path
}

function Resolve-HostValue {
    param([hashtable]$Config)
    if ($env:LAUNCHER_DEPLOY_HOST) { return $env:LAUNCHER_DEPLOY_HOST }
    if ($Config.ContainsKey('host')) { return $Config['host'] }
    return ""
}

function Invoke-DesktopLaunchProbe {
    param([string]$ApiUrl)
    $exe = Join-Path $root 'dist\LauncherD2D.exe'
    if (-not (Test-Path $exe)) {
        throw "dist\LauncherD2D.exe not found"
    }
    $uri = [Uri]$ApiUrl
    $oldScheme = $env:LAUNCHER_API_SCHEME
    $oldHost = $env:LAUNCHER_API_HOST
    $oldPort = $env:LAUNCHER_API_PORT
    try {
        $env:LAUNCHER_API_SCHEME = $uri.Scheme
        $env:LAUNCHER_API_HOST = $uri.Host
        $env:LAUNCHER_API_PORT = [string]$uri.Port
        $p = Start-Process -FilePath $exe -WindowStyle Minimized -PassThru
        Start-Sleep -Seconds 5
        $p.Refresh()
        if ($p.HasExited) {
            throw "LauncherD2D exited early with code $($p.ExitCode)"
        }
        $deadline = (Get-Date).AddSeconds(8)
        while ((Get-Date) -lt $deadline) {
            $p.Refresh()
            if ($p.MainWindowTitle -like '*Launcher*') { break }
            Start-Sleep -Milliseconds 500
        }
        $p.Refresh()
        if ($p.MainWindowTitle -notlike '*Launcher*') {
            throw "LauncherD2D process is running, but no Launcher window title was observed"
        }
        [void]$p.CloseMainWindow()
        if (-not $p.WaitForExit(5000)) {
            Stop-Process -Id $p.Id -Force
        }
        return "pid=$($p.Id)"
    }
    finally {
        $env:LAUNCHER_API_SCHEME = $oldScheme
        $env:LAUNCHER_API_HOST = $oldHost
        $env:LAUNCHER_API_PORT = $oldPort
    }
}

$deployConf = Read-DeployConfig
$deployHost = Resolve-HostValue $deployConf
$deployUser = if ($env:LAUNCHER_DEPLOY_USER) { $env:LAUNCHER_DEPLOY_USER } elseif ($deployConf.ContainsKey('user')) { $deployConf['user'] } else { "" }
$deployPort = if ($env:LAUNCHER_DEPLOY_PORT) { $env:LAUNCHER_DEPLOY_PORT } elseif ($deployConf.ContainsKey('port')) { $deployConf['port'] } else { "22" }

if ($SkipDesktopLaunch) {
    $DesktopUiMode = 'none'
} elseif ([string]::IsNullOrWhiteSpace($DesktopUiMode)) {
    $DesktopUiMode = if ($Depth -in @('full', 'desktop')) { 'launch-only' } else { 'none' }
}

if ([string]::IsNullOrWhiteSpace($BaseUrl)) {
    if ($Target -eq 'audit') {
        if ($deployHost) { $BaseUrl = "http://$deployHost`:1338" }
    } elseif ($Target -eq 'production') {
        if ($deployHost) { $BaseUrl = "https://$deployHost`:1337" }
    }
}
$script:ResolvedBaseUrl = $BaseUrl

$doPreflight = $true
$doDeploy = ($Target -eq 'audit' -and $Depth -eq 'full' -and -not $env:LAUNCHER_SMOKE_SKIP_DEPLOY)
$doApi = $Depth -in @('full', 'api', 'readonly')
$doChatApi = ($Depth -in @('full', 'api', 'chat') -and $Target -ne 'production')
$doDesktopBuild = ($Depth -in @('full', 'desktop') -and -not $SkipDesktopBuild)
$doDesktopLaunch = ($Depth -in @('full', 'desktop') -and $DesktopUiMode -eq 'launch-only')
$doForegroundVisual = ($Depth -in @('full', 'desktop') -and $DesktopUiMode -eq 'foreground-visual')
$apiMode = if ($Depth -eq 'readonly') { 'readonly' } elseif ($Target -eq 'production' -and -not $AllowProductionWrites) { 'readonly' } else { 'write' }
$cleanupRemoteDir = if ($Target -eq 'production') {
    if ($deployConf.ContainsKey('remote_dir')) { $deployConf['remote_dir'] } else { '/opt/systembackend' }
} else {
    '/opt/systembackend-audit'
}

try {
    if ($doPreflight) {
        Invoke-SmokeStep 'preflight' 1 {
            Invoke-Native -File 'git' -Arguments @('diff', '--check') | Out-Null
            Invoke-Native -File 'node' -Arguments @('--version') | Out-Null
            if (-not (Test-Path (Join-Path $root 'scripts\audit\api-smoke.mjs'))) {
                throw "scripts\audit\api-smoke.mjs not found"
            }
            if ($doChatApi -and -not (Test-Path (Join-Path $root 'scripts\audit\chat-smoke.mjs'))) {
                throw "scripts\audit\chat-smoke.mjs not found"
            }
            if ($doDeploy) {
                if (-not $deployHost -or -not $deployUser) {
                    throw "audit deploy needs LAUNCHER_DEPLOY_HOST/LAUNCHER_DEPLOY_USER or .deploy.local"
                }
                if (-not (Test-Path (Join-Path $root 'scripts\audit\deploy-audit.ps1'))) {
                    throw "scripts\audit\deploy-audit.ps1 not found"
                }
            }
            if (($doApi -or $doDesktopLaunch) -and [string]::IsNullOrWhiteSpace($script:ResolvedBaseUrl)) {
                throw "BaseUrl could not be resolved"
            }
            if (($doChatApi -or $doForegroundVisual) -and [string]::IsNullOrWhiteSpace($script:ResolvedBaseUrl)) {
                throw "BaseUrl could not be resolved"
            }
            if ($Depth -eq 'chat' -and $Target -eq 'production') {
                throw "chat smoke refuses Target=production"
            }
            if ($Target -eq 'production' -and $apiMode -eq 'write' -and (-not $AllowProductionWrites)) {
                throw "production write smoke needs -AllowProductionWrites"
            }
            if ($Target -eq 'production' -and $apiMode -eq 'write' -and (-not $deployHost -or -not $deployUser)) {
                throw "production write smoke needs deployment SSH config so cleanup can run"
            }
            if ($doDesktopBuild) {
                $wv2Header = Join-Path $root 'third_party\webview2\build\native\include\WebView2.h'
                if (-not (Test-Path $wv2Header)) {
                    throw "WebView2 SDK missing: $wv2Header"
                }
            }
            "target=$Target depth=$Depth mode=$apiMode base=$script:ResolvedBaseUrl"
        }
    }

    if ($doDeploy) {
        Invoke-SmokeStep 'deploy.audit' 3 {
            $deployScript = Join-Path $root 'scripts\audit\deploy-audit.ps1'
            Invoke-Native -File 'powershell.exe' -Arguments @(
                '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $deployScript,
                '-Host_', $deployHost,
                '-Port', $deployPort,
                '-User', $deployUser,
                '-RemoteDir', '/opt/systembackend-audit',
                '-Service', 'systembackend-audit',
                '-BindAddr', '0.0.0.0:1338',
                '-DatabaseMode', $DatabaseMode
            ) | Out-Null
            "audit_url=$script:ResolvedBaseUrl"
        }
    }

    if ($doDesktopBuild) {
        Invoke-SmokeStep 'desktop.build' 2 {
            Invoke-Native -File 'cmd.exe' -Arguments @('/c', 'tools\preview-d2d\build_d2d.bat') | Out-Null
            if (-not (Test-Path (Join-Path $root 'dist\LauncherD2D.exe'))) {
                throw "dist\LauncherD2D.exe not produced"
            }
            if (-not (Test-Path (Join-Path $root 'dist\WebView2Loader.dll'))) {
                throw "dist\WebView2Loader.dll not produced"
            }
            "dist ready"
        }
    }

    if ($doApi) {
        Invoke-SmokeStep "api.$apiMode" 4 {
            $apiJson = Join-Path ([IO.Path]::GetTempPath()) ("launcher-api-smoke-$RunId.json")
            $envs = @{
                LAUNCHER_AUDIT_BASE = $script:ResolvedBaseUrl
                LAUNCHER_AUDIT_RUN_ID = $RunId
                LAUNCHER_AUDIT_MODE = $apiMode
                LAUNCHER_AUDIT_JSON = '1'
                LAUNCHER_AUDIT_JSON_PATH = $apiJson
            }
            if ($script:ResolvedBaseUrl -like 'https://*') {
                $envs['LAUNCHER_AUDIT_INSECURE_TLS'] = '1'
            }
            Invoke-Native -File 'node' -Arguments @('scripts\audit\api-smoke.mjs') -Environment $envs | Out-Null
            "json=$apiJson"
        }
    }

    if ($doChatApi) {
        Invoke-SmokeStep 'api.chat' 7 {
            $chatJson = Join-Path ([IO.Path]::GetTempPath()) ("launcher-chat-smoke-$RunId.json")
            $envs = @{
                LAUNCHER_AUDIT_BASE = $script:ResolvedBaseUrl
                LAUNCHER_AUDIT_RUN_ID = $RunId
                LAUNCHER_AUDIT_JSON = '1'
                LAUNCHER_AUDIT_JSON_PATH = $chatJson
            }
            if ($script:ResolvedBaseUrl -like 'https://*') {
                $envs['LAUNCHER_AUDIT_INSECURE_TLS'] = '1'
            }
            Invoke-Native -File 'node' -Arguments @('scripts\audit\chat-smoke.mjs') -Environment $envs | Out-Null
            "json=$chatJson"
        }
    }

    if ($doDesktopLaunch) {
        Invoke-SmokeStep 'desktop.launch' 5 {
            Invoke-DesktopLaunchProbe $script:ResolvedBaseUrl
        }
    }

    if ($doForegroundVisual) {
        Invoke-SmokeStep 'desktop.visual.foreground' 8 {
            $visualScript = Join-Path $root 'scripts\audit\visual-smoke.ps1'
            if (-not (Test-Path $visualScript)) {
                throw "scripts\audit\visual-smoke.ps1 not found"
            }
            $visualOut = Join-Path ([IO.Path]::GetTempPath()) ("launcher-visual-smoke-$RunId")
            $visualReport = Join-Path $visualOut ("{0}-report.json" -f $RunId)
            $visualFailure = $null
            try {
                Invoke-Native -File 'powershell.exe' -Arguments @(
                    '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $visualScript,
                    '-BaseUrl', $script:ResolvedBaseUrl,
                    '-OutDir', $visualOut,
                    '-RunId', $RunId,
                    '-Foreground',
                    '-AllowForegroundUi'
                ) | Out-Null
            }
            catch {
                $visualFailure = $_
            }
            finally {
                if (Test-Path $visualReport) {
                    $visual = Get-Content -Raw -Path $visualReport | ConvertFrom-Json
                    $script:UiTelemetry.foreground_used = [bool]$visual.foreground_used
                    $script:UiTelemetry.mouse_used = [bool]$visual.mouse_used
                    $script:UiTelemetry.topmost_used = [bool]$visual.topmost_used
                }
            }
            if ($visualFailure) { throw $visualFailure }
            "foreground visual smoke completed"
        }
    }
}
catch {
    if ($script:ExitCode -eq 0) { $script:ExitCode = 1 }
}
finally {
    $cleanupFailed = $false
    if ($Target -in @('audit', 'production') -and $apiMode -eq 'write' -and -not $KeepAuditData -and ($Depth -in @('full', 'api', 'chat'))) {
        try {
            if (-not $deployHost -or -not $deployUser) {
                Add-StepResult -Name 'cleanup.audit_data' -Status 'skip' -Started (Get-Date) -Ended (Get-Date) -Message 'no SSH deploy config available'
            } else {
                Invoke-SmokeStep 'cleanup.audit_data' 6 {
                    $cleanupScript = Join-Path $root 'scripts\audit\cleanup-audit-data.ps1'
                    if (-not (Test-Path $cleanupScript)) {
                        throw "scripts\audit\cleanup-audit-data.ps1 not found"
                    }
                    Invoke-Native -File 'powershell.exe' -Arguments @(
                        '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $cleanupScript,
                        '-Prefix', $RunId,
                        '-RemoteDir', $cleanupRemoteDir,
                        '-Host_', $deployHost,
                        '-Port', $deployPort,
                        '-User', $deployUser
                    ) | Out-Null
                    "prefix=$RunId"
                }
            }
        }
        catch {
            $cleanupFailed = $true
            if ($script:ExitCode -eq 0) { $script:ExitCode = 6 }
        }
    }
    $result = if ($script:ExitCode -eq 0) { 'pass' } elseif ($cleanupFailed) { 'cleanup_fail' } else { 'fail' }
    Write-SmokeReport -Path $JsonReport -Result $result
    if ($script:ExitCode -eq 0) {
        Write-Host "SMOKE_RESULT ok | run_id=$RunId" -ForegroundColor Green
    } else {
        Write-Host "SMOKE_RESULT fail | run_id=$RunId exit=$script:ExitCode" -ForegroundColor Red
    }
    exit $script:ExitCode
}
