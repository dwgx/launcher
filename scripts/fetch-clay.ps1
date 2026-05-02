# 拉单头文件 clay.h 到 third_party/clay

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$dest = Join-Path $root 'third_party\clay'
New-Item -ItemType Directory -Force -Path $dest | Out-Null

$target = Join-Path $dest 'clay.h'
if (Test-Path $target) { Write-Host 'clay.h already present'; exit 0 }

$url = 'https://raw.githubusercontent.com/nicbarker/clay/main/clay.h'
Invoke-WebRequest -Uri $url -OutFile $target -UseBasicParsing
Write-Host "clay.h -> $target"
