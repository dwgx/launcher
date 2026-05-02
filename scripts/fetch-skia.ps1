# 拉 aseprite/skia Windows x64 预编译包到 third_party/skia
# 编译官方 Skia 要 depot_tools + GN，新人会卡两天，所以走预编译。

$ErrorActionPreference = 'Stop'
$root  = Split-Path -Parent $PSScriptRoot
$dest  = Join-Path $root 'third_party\skia'

if (Test-Path (Join-Path $dest 'include\core\SkCanvas.h')) {
    Write-Host 'Skia already present, skipping'
    exit 0
}

# 最新发布需要手动确认 tag；这里先指向一个稳定 tag（按需调整）
# 见 https://github.com/aseprite/skia/releases
$tag = 'm124-08a5439a6b'
$url = "https://github.com/aseprite/skia/releases/download/$tag/Skia-Windows-Release-x64.zip"

$zip = Join-Path $env:TEMP "skia-$tag.zip"
Write-Host "Downloading $url"
Invoke-WebRequest -Uri $url -OutFile $zip -UseBasicParsing

Write-Host "Extracting to $dest"
New-Item -ItemType Directory -Force -Path $dest | Out-Null
Expand-Archive -Path $zip -DestinationPath $dest -Force
Remove-Item $zip -Force

Write-Host 'Skia ready.'
