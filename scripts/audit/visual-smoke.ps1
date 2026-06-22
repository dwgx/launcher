param(
    [string]$BaseUrl = $(if ($env:LAUNCHER_AUDIT_BASE) { $env:LAUNCHER_AUDIT_BASE } else { "http://127.0.0.1:1338" }),
    [string]$OutDir = $(Join-Path (Resolve-Path (Join-Path $PSScriptRoot '..\..')) 'tmp\smoke-visual'),
    [string]$RunId = ("visual" + (Get-Date -Format 'yyyyMMddHHmmss')),
    [switch]$Foreground = $false,
    [switch]$AllowForegroundUi = $false,
    [switch]$KeepProcess = $false
)

$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..\..')
$exe = Join-Path $root 'dist\LauncherD2D.exe'

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$script:ForegroundUiAllowed = [bool]($Foreground -and $AllowForegroundUi)
$script:UiUsage = [ordered]@{
    foreground_used = $false
    mouse_used = $false
    topmost_used = $false
}

function Write-GuardReport {
    param([Parameter(Mandatory=$true)][string]$Failure)
    $report = [ordered]@{
        run_id = $RunId
        base_url = $BaseUrl
        foreground_requested = [bool]$Foreground
        allow_foreground_ui = [bool]$AllowForegroundUi
        foreground_used = [bool]$script:UiUsage.foreground_used
        mouse_used = [bool]$script:UiUsage.mouse_used
        topmost_used = [bool]$script:UiUsage.topmost_used
        failure = $Failure
        out_dir = $OutDir
        screenshots = @()
    }
    $reportPath = Join-Path $OutDir ("{0}-report.json" -f $RunId)
    $report | ConvertTo-Json -Depth 6 | Set-Content -Encoding UTF8 -Path $reportPath
    Write-Host "VISUAL_SMOKE_REPORT $reportPath"
    $report | ConvertTo-Json -Depth 6
}

function Assert-ForegroundUiAllowed {
    param([Parameter(Mandatory=$true)][string]$Action)
    if (-not $script:ForegroundUiAllowed) {
        throw "$Action requires both -Foreground and -AllowForegroundUi"
    }
}

if (-not $script:ForegroundUiAllowed) {
    $msg = "visual smoke is foreground-only; pass both -Foreground and -AllowForegroundUi to allow SetForegroundWindow/SetCursorPos/topmost/CopyFromScreen"
    Write-GuardReport -Failure $msg
    throw $msg
}

if (-not (Test-Path $exe)) {
    throw "dist\LauncherD2D.exe not found; build the D2D client first."
}

Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
Add-Type @'
using System;
using System.Runtime.InteropServices;

public static class Win32VisualSmoke {
    [StructLayout(LayoutKind.Sequential)]
    public struct RECT {
        public int Left;
        public int Top;
        public int Right;
        public int Bottom;
    }

    [DllImport("user32.dll")]
    public static extern bool GetWindowRect(IntPtr hWnd, out RECT rect);

    [DllImport("user32.dll")]
    public static extern bool SetForegroundWindow(IntPtr hWnd);

    [DllImport("user32.dll")]
    public static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);

    [DllImport("user32.dll")]
    public static extern bool SetCursorPos(int x, int y);

    [DllImport("user32.dll")]
    public static extern void mouse_event(uint dwFlags, uint dx, uint dy, uint dwData, UIntPtr dwExtraInfo);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern bool PostMessageW(IntPtr hWnd, uint Msg, UIntPtr wParam, IntPtr lParam);

    [DllImport("user32.dll")]
    public static extern bool SetWindowPos(IntPtr hWnd, IntPtr hWndInsertAfter, int X, int Y, int cx, int cy, uint uFlags);
}
'@

function Get-WindowRect {
    param([Parameter(Mandatory=$true)][IntPtr]$Hwnd)
    $rect = New-Object Win32VisualSmoke+RECT
    if (-not [Win32VisualSmoke]::GetWindowRect($Hwnd, [ref]$rect)) {
        throw "GetWindowRect failed"
    }
    [ordered]@{
        left = $rect.Left
        top = $rect.Top
        right = $rect.Right
        bottom = $rect.Bottom
        width = $rect.Right - $rect.Left
        height = $rect.Bottom - $rect.Top
    }
}

function Wait-WindowSize {
    param(
        [Parameter(Mandatory=$true)][System.Diagnostics.Process]$Process,
        [Parameter(Mandatory=$true)][int]$MinWidth,
        [Parameter(Mandatory=$true)][int]$MinHeight,
        [int]$TimeoutMs = 10000
    )
    $deadline = [Environment]::TickCount64 + $TimeoutMs
    do {
        $Process.Refresh()
        if ($Process.HasExited) {
            throw "LauncherD2D exited early with code $($Process.ExitCode)"
        }
        if ($Process.MainWindowHandle -ne [IntPtr]::Zero) {
            $r = Get-WindowRect $Process.MainWindowHandle
            if ($r.width -ge $MinWidth -and $r.height -ge $MinHeight) {
                return $r
            }
        }
        Start-Sleep -Milliseconds 100
    } while ([Environment]::TickCount64 -lt $deadline)
    throw "Timed out waiting for window >= ${MinWidth}x${MinHeight}"
}

function Capture-Window {
    param(
        [Parameter(Mandatory=$true)][System.Diagnostics.Process]$Process,
        [Parameter(Mandatory=$true)][string]$Name
    )
    Assert-ForegroundUiAllowed 'CopyFromScreen'
    $Process.Refresh()
    if ($Process.MainWindowHandle -eq [IntPtr]::Zero) {
        throw "No main window handle"
    }
    Focus-App $Process
    $r = Get-WindowRect $Process.MainWindowHandle
    $path = Join-Path $OutDir ("{0}-{1}.png" -f $RunId, $Name)
    $bmp = [System.Drawing.Bitmap]::new($r.width, $r.height)
    $gfx = [System.Drawing.Graphics]::FromImage($bmp)
    try {
        $script:UiUsage.foreground_used = $true
        $gfx.CopyFromScreen($r.left, $r.top, 0, 0, [System.Drawing.Size]::new($r.width, $r.height))
        $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    }
    finally {
        $gfx.Dispose()
        $bmp.Dispose()
    }
    [ordered]@{ name = $Name; path = $path; width = $r.width; height = $r.height }
}

function Focus-App {
    param([Parameter(Mandatory=$true)][System.Diagnostics.Process]$Process)
    Assert-ForegroundUiAllowed 'SetForegroundWindow/topmost'
    $Process.Refresh()
    [void][Win32VisualSmoke]::ShowWindow($Process.MainWindowHandle, 5)
    [void][Win32VisualSmoke]::SetForegroundWindow($Process.MainWindowHandle)
    $script:UiUsage.foreground_used = $true
    $noMoveSizeActivate = 0x0001 -bor 0x0002 -bor 0x0010
    [void][Win32VisualSmoke]::SetWindowPos(
        $Process.MainWindowHandle,
        [IntPtr]::new(-1),
        0, 0, 0, 0,
        [uint32]$noMoveSizeActivate
    )
    $script:UiUsage.topmost_used = $true
    Start-Sleep -Milliseconds 60
    [void][Win32VisualSmoke]::SetWindowPos(
        $Process.MainWindowHandle,
        [IntPtr]::new(-2),
        0, 0, 0, 0,
        [uint32]$noMoveSizeActivate
    )
    Start-Sleep -Milliseconds 180
}

function Send-Text {
    param(
        [Parameter(Mandatory=$true)][System.Diagnostics.Process]$Process,
        [Parameter(Mandatory=$true)][string]$Text
    )
    Assert-ForegroundUiAllowed 'keyboard foreground input'
    Focus-App $Process
    foreach ($ch in $Text.ToCharArray()) {
        [void][Win32VisualSmoke]::PostMessageW(
            $Process.MainWindowHandle,
            0x0102,
            [UIntPtr]::new([uint64][int][char]$ch),
            [IntPtr]1
        )
        Start-Sleep -Milliseconds 12
    }
    Start-Sleep -Milliseconds 80
}

function Send-Key {
    param(
        [Parameter(Mandatory=$true)][System.Diagnostics.Process]$Process,
        [Parameter(Mandatory=$true)][string]$Key
    )
    Assert-ForegroundUiAllowed 'keyboard foreground input'
    Focus-App $Process
    $vk = switch ($Key) {
        '{TAB}' { 0x09; break }
        '{ENTER}' { 0x0D; break }
        default { throw "Unsupported key: $Key" }
    }
    [void][Win32VisualSmoke]::PostMessageW(
        $Process.MainWindowHandle,
        0x0100,
        [UIntPtr]::new([uint64]$vk),
        [IntPtr]1
    )
    Start-Sleep -Milliseconds 120
}

function Click-Dip {
    param(
        [Parameter(Mandatory=$true)][System.Diagnostics.Process]$Process,
        [Parameter(Mandatory=$true)][float]$X,
        [Parameter(Mandatory=$true)][float]$Y,
        [Parameter(Mandatory=$true)][float]$DesignWidth,
        [Parameter(Mandatory=$true)][float]$DesignHeight
    )
    Assert-ForegroundUiAllowed 'SetCursorPos/mouse_event'
    Focus-App $Process
    $r = Get-WindowRect $Process.MainWindowHandle
    $sx = $r.left + [int]($X * $r.width / $DesignWidth)
    $sy = $r.top + [int]($Y * $r.height / $DesignHeight)
    [void][Win32VisualSmoke]::SetCursorPos($sx, $sy)
    $script:UiUsage.mouse_used = $true
    Start-Sleep -Milliseconds 50
    [Win32VisualSmoke]::mouse_event(0x0002, 0, 0, 0, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 40
    [Win32VisualSmoke]::mouse_event(0x0004, 0, 0, 0, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 180
}

function Measure-Image {
    param([Parameter(Mandatory=$true)][string]$Path)
    $bmp = [System.Drawing.Bitmap]::new($Path)
    try {
        $samples = 0
        $sum = 0.0
        $nonDark = 0
        $stepX = [Math]::Max(1, [int]($bmp.Width / 80))
        $stepY = [Math]::Max(1, [int]($bmp.Height / 60))
        for ($y = 0; $y -lt $bmp.Height; $y += $stepY) {
            for ($x = 0; $x -lt $bmp.Width; $x += $stepX) {
                $c = $bmp.GetPixel($x, $y)
                $lum = (0.2126 * $c.R) + (0.7152 * $c.G) + (0.0722 * $c.B)
                $sum += $lum
                $samples++
                if ($lum -gt 16) { $nonDark++ }
            }
        }
        [ordered]@{
            width = $bmp.Width
            height = $bmp.Height
            mean_luma = [Math]::Round($sum / [Math]::Max(1, $samples), 2)
            non_dark_ratio = [Math]::Round($nonDark / [Math]::Max(1, $samples), 4)
        }
    }
    finally {
        $bmp.Dispose()
    }
}

$uri = [Uri]$BaseUrl
$psi = [System.Diagnostics.ProcessStartInfo]::new()
$psi.FileName = $exe
$psi.WorkingDirectory = Join-Path $root 'dist'
$psi.UseShellExecute = $false
$psi.EnvironmentVariables['LAUNCHER_API_SCHEME'] = $uri.Scheme
$psi.EnvironmentVariables['LAUNCHER_API_HOST'] = $uri.Host
$psi.EnvironmentVariables['LAUNCHER_API_PORT'] = [string]$uri.Port
$psi.EnvironmentVariables['LAUNCHER_DISABLE_PERSIST'] = '1'

$username = ($RunId -replace '[^a-zA-Z0-9_.-]', '').ToLowerInvariant()
if ($username.Length -gt 28) { $username = $username.Substring(0, 28) }
$password = 'VisualPass123'
$shots = New-Object System.Collections.Generic.List[object]
$process = $null
$failure = $null

try {
    $process = [System.Diagnostics.Process]::Start($psi)
    [void](Wait-WindowSize -Process $process -MinWidth 430 -MinHeight 480 -TimeoutMs 12000)
    Start-Sleep -Milliseconds 700
    $shots.Add((Capture-Window -Process $process -Name '01-auth-login')) | Out-Null

    Click-Dip -Process $process -X 235 -Y 391 -DesignWidth 480 -DesignHeight 540
    Start-Sleep -Milliseconds 500
    $shots.Add((Capture-Window -Process $process -Name '02-auth-register')) | Out-Null

    Send-Text -Process $process -Text $username
    Send-Key -Process $process -Key '{TAB}'
    Send-Text -Process $process -Text $password
    Send-Key -Process $process -Key '{TAB}'
    Send-Text -Process $process -Text 'VISUAL'
    Send-Key -Process $process -Key '{ENTER}'

    [void](Wait-WindowSize -Process $process -MinWidth 1000 -MinHeight 650 -TimeoutMs 15000)
    Start-Sleep -Milliseconds 1400
    $shots.Add((Capture-Window -Process $process -Name '03-main-home')) | Out-Null

    Click-Dip -Process $process -X 100 -Y 165 -DesignWidth 1100 -DesignHeight 720
    Start-Sleep -Milliseconds 1200
    $shots.Add((Capture-Window -Process $process -Name '04-main-chat')) | Out-Null

    Click-Dip -Process $process -X 100 -Y 207 -DesignWidth 1100 -DesignHeight 720
    Start-Sleep -Milliseconds 900
    $shots.Add((Capture-Window -Process $process -Name '05-main-market')) | Out-Null

    Click-Dip -Process $process -X 100 -Y 291 -DesignWidth 1100 -DesignHeight 720
    Start-Sleep -Milliseconds 700
    $shots.Add((Capture-Window -Process $process -Name '06-main-settings')) | Out-Null

    Click-Dip -Process $process -X 1032 -Y 24 -DesignWidth 1100 -DesignHeight 720
    Start-Sleep -Milliseconds 500
    $shots.Add((Capture-Window -Process $process -Name '07-account-dropdown')) | Out-Null

    Click-Dip -Process $process -X 900 -Y 190 -DesignWidth 1100 -DesignHeight 720
    Start-Sleep -Milliseconds 800
    $shots.Add((Capture-Window -Process $process -Name '08-profile')) | Out-Null
}
catch {
    $failure = $_.Exception.Message
    if ($process -and -not $process.HasExited -and $process.MainWindowHandle -ne [IntPtr]::Zero) {
        try {
            $shots.Add((Capture-Window -Process $process -Name '99-failure')) | Out-Null
        } catch {}
    }
}
finally {
    if ($process -and -not $KeepProcess) {
        try {
            if (-not $process.HasExited) {
                [void]$process.CloseMainWindow()
                if (-not $process.WaitForExit(3000)) {
                    Stop-Process -Id $process.Id -Force
                }
            }
        } catch {}
    }
}

$report = [ordered]@{
    run_id = $RunId
    base_url = $BaseUrl
    username = $username
    foreground_requested = [bool]$Foreground
    allow_foreground_ui = [bool]$AllowForegroundUi
    foreground_used = [bool]$script:UiUsage.foreground_used
    mouse_used = [bool]$script:UiUsage.mouse_used
    topmost_used = [bool]$script:UiUsage.topmost_used
    failure = $failure
    out_dir = $OutDir
    screenshots = @($shots | ForEach-Object {
        [ordered]@{
            name = $_.name
            path = $_.path
            width = $_.width
            height = $_.height
            metrics = Measure-Image $_.path
        }
    })
}
$reportPath = Join-Path $OutDir ("{0}-report.json" -f $RunId)
$report | ConvertTo-Json -Depth 6 | Set-Content -Encoding UTF8 -Path $reportPath
Write-Host "VISUAL_SMOKE_REPORT $reportPath"
$report | ConvertTo-Json -Depth 6
if ($failure) {
    throw $failure
}
