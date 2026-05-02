# 启动 LauncherPreview.exe，等动画稳定后截 1100x720 主窗口区域，保存为 png
# 环境变量：
#   LAUNCHER_DARK=1  暗色
#   LAUNCHER_LANG=e|z|j  语言
#   LAUNCHER_VIEW=h|l|c|s  Home/Library/Cloud/Settings
#   LAUNCHER_SKIP_LOADING=1  跳过加载直接进主界面
# 输出文件名：preview-{view}-{lang}-{theme}.png
param([int]$DelayMs = 600)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

Get-Process -Name LauncherPreview -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 250

$exe = Join-Path $PSScriptRoot 'LauncherPreview.exe'
$args = @()
if ($env:LAUNCHER_SKIP_LOADING -eq '1') { $args += '--main' }
Start-Process -FilePath $exe -ArgumentList $args
Start-Sleep -Milliseconds $DelayMs

$screen = [System.Windows.Forms.SystemInformation]::VirtualScreen
$bmp = New-Object System.Drawing.Bitmap $screen.Width, $screen.Height
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($screen.Left, $screen.Top, 0, 0, $bmp.Size)
$g.Dispose()

$w = if ($env:LAUNCHER_SKIP_LOADING -eq '1') { 1100 } else { 200 }
$h = if ($env:LAUNCHER_SKIP_LOADING -eq '1') { 720 } else { 200 }
$cx = [int](($screen.Width - $w) / 2)
$cy = [int](($screen.Height - $h) / 2)

$crop = New-Object System.Drawing.Bitmap $w, $h
$g = [System.Drawing.Graphics]::FromImage($crop)
$src = New-Object System.Drawing.Rectangle $cx, $cy, $w, $h
$dst = New-Object System.Drawing.Rectangle 0, 0, $w, $h
$g.DrawImage($bmp, $dst, $src, [System.Drawing.GraphicsUnit]::Pixel)
$g.Dispose()
$bmp.Dispose()

$theme = if ($env:LAUNCHER_DARK -eq '1') { 'dark' } else { 'light' }
$lang  = switch ($env:LAUNCHER_LANG) { 'e' { 'en' } 'z' { 'cn' } 'j' { 'jp' } default { 'cn' } }
$view  = switch ($env:LAUNCHER_VIEW) {
    'h' { 'home' } 'l' { 'library' } 'c' { 'cloud' } 's' { 'settings' }
    default { if ($env:LAUNCHER_SKIP_LOADING -eq '1') { 'home' } else { 'loading' } }
}

$out = Join-Path $PSScriptRoot ("preview-{0}-{1}-{2}.png" -f $view, $lang, $theme)
$crop.Save($out, [System.Drawing.Imaging.ImageFormat]::Png)
$crop.Dispose()
Write-Host "Saved: $out"

Get-Process -Name LauncherPreview -ErrorAction SilentlyContinue | Stop-Process -Force
