# 下载 Launcher 字体集到 assets/fonts/。
# 来源全部 OFL/free，可商用。再次运行会跳过已存在的 ttf。

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$dest = Join-Path $root 'assets\fonts'
New-Item -ItemType Directory -Force -Path $dest | Out-Null

function Need($name) {
    $path = Join-Path $dest $name
    -not (Test-Path $path)
}

function Fetch($url, $name) {
    $tmp = Join-Path $env:TEMP "launcher-font-$([guid]::NewGuid().Guid).bin"
    Write-Host "↓ $url"
    Invoke-WebRequest -Uri $url -OutFile $tmp -UseBasicParsing
    Move-Item -Force $tmp (Join-Path $dest $name)
}

# Space Grotesk (英文/数字)
$sgBase = 'https://github.com/floriankarsten/space-grotesk/raw/master/fonts/ttf'
foreach ($w in @('Regular','Medium','SemiBold','Bold')) {
    $name = "SpaceGrotesk-$w.ttf"
    if (Need $name) { Fetch "$sgBase/$name" $name }
}

# DejaVu Sans / Mono (等宽 + 兜底英文)
$dvUrl = 'https://github.com/dejavu-fonts/dejavu-fonts/releases/download/version_2_37/dejavu-fonts-ttf-2.37.zip'
if (Need 'DejaVuSansMono.ttf') {
    $zip = Join-Path $env:TEMP 'dejavu.zip'
    Invoke-WebRequest -Uri $dvUrl -OutFile $zip -UseBasicParsing
    $tmpDir = Join-Path $env:TEMP "dejavu-$([guid]::NewGuid().Guid)"
    Expand-Archive -Path $zip -DestinationPath $tmpDir
    Get-ChildItem -Recurse -Filter '*.ttf' $tmpDir | ForEach-Object {
        if ($_.Name -in @('DejaVuSans.ttf','DejaVuSansMono.ttf','DejaVuSansMono-Bold.ttf','DejaVuSerif.ttf')) {
            Copy-Item -Force $_.FullName (Join-Path $dest $_.Name)
        }
    }
    Remove-Item -Recurse -Force $tmpDir, $zip
}

# Source Han Sans CN (中文 — SubsetOTF/CN, ~18MB)
if (Need 'SourceHanSansCN-Regular.otf') {
    Write-Host '注意: Source Han Sans CN SubsetOTF 体积约 18MB，下载可能较慢。'
    $shsBase = 'https://github.com/adobe-fonts/source-han-sans/raw/release/SubsetOTF/CN'
    foreach ($w in @('Regular','Medium','Bold')) {
        $name = "SourceHanSansCN-$w.otf"
        if (Need $name) { Fetch "$shsBase/$name" $name }
    }
}

# BIZ UDPGothic (日文 UD)
if (Need 'BIZUDPGothic-Regular.ttf') {
    $bgBase = 'https://github.com/googlefonts/morisawa-biz-ud-gothic/raw/main/fonts/ttf'
    foreach ($name in @('BIZUDPGothic-Regular.ttf','BIZUDPGothic-Bold.ttf')) {
        if (Need $name) { Fetch "$bgBase/$name" $name }
    }
}

# BIZ UDPMincho (日文 UD 衬线)
if (Need 'BIZUDPMincho-Regular.ttf') {
    $bmBase = 'https://github.com/googlefonts/morisawa-biz-ud-mincho/raw/main/fonts/ttf'
    foreach ($name in @('BIZUDPMincho-Regular.ttf','BIZUDPMincho-Bold.ttf')) {
        if (Need $name) { Fetch "$bmBase/$name" $name }
    }
}

# Source Serif 4 (英文衬线)
if (Need 'SourceSerif4-Regular.ttf') {
    $ssBase = 'https://github.com/adobe-fonts/source-serif/raw/release/TTF'
    foreach ($name in @('SourceSerif4-Regular.ttf','SourceSerif4-Semibold.ttf','SourceSerif4-Bold.ttf')) {
        if (Need $name) { Fetch "$ssBase/$name" $name }
    }
}

Write-Host ''
Write-Host 'Fonts ready in' $dest
Get-ChildItem $dest -File | Where-Object { $_.Extension -in '.ttf','.otf' } |
    ForEach-Object { '  {0,-40} {1,8:N0} bytes' -f $_.Name, $_.Length } | Write-Host
