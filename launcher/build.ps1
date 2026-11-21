# Builds the Triforce launcher: one self-contained file per system.
#   powershell -NoProfile -ExecutionPolicy Bypass -File launcher\build.ps1 -Version v0.6.0 [-Rid win-x64|linux-x64|all]
# Output: launcher\out\TriforceSetup.exe (Windows; the release asset old installs update onto) and launcher\out\triforce-launcher (Linux).
param([string]$Version = 'dev', [string]$Rid = 'all', [string]$Dotnet = 'D:\src\tools\dotnet\dotnet.exe')
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path; $repo = Split-Path -Parent $here
$gen = Join-Path $here 'App\obj\gen'; New-Item -ItemType Directory -Force $gen | Out-Null
$env:DOTNET_CLI_TELEMETRY_OPTOUT = '1'; $env:DOTNET_NOLOGO = '1'

# ---- the icon: our crest (assets\triforce.png) centred in a square, PNG entries 16..256 (as setup\build.ps1) ----
Add-Type -AssemblyName System.Drawing
function Square([System.Drawing.Image]$src, [int]$s) {
    $b = New-Object System.Drawing.Bitmap $s, $s, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($b); $g.InterpolationMode = 'HighQualityBicubic'; $g.PixelOffsetMode = 'HighQuality'
    $k = [math]::Min($s / $src.Width, $s / $src.Height); $w = $src.Width * $k; $h = $src.Height * $k
    $g.DrawImage($src, [single](($s - $w) / 2), [single](($s - $h) / 2), [single]$w, [single]$h); $g.Dispose(); return $b
}
$crest = [System.Drawing.Image]::FromFile((Join-Path $repo 'assets\triforce.png'))
$sizes = 16, 20, 24, 32, 40, 48, 64, 96, 128, 256
$pngs = foreach ($s in $sizes) { $ms = New-Object IO.MemoryStream; (Square $crest $s).Save($ms, [System.Drawing.Imaging.ImageFormat]::Png); , $ms.ToArray() }
(Square $crest 256).Save((Join-Path $gen 'triforce.png'), [System.Drawing.Imaging.ImageFormat]::Png)
$ico = New-Object IO.MemoryStream; $bw = New-Object IO.BinaryWriter $ico
$bw.Write([uint16]0); $bw.Write([uint16]1); $bw.Write([uint16]$sizes.Count); $off = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) { $s = $sizes[$i]; $d = if ($s -ge 256) { 0 } else { $s }
    $bw.Write([byte]$d); $bw.Write([byte]$d); $bw.Write([byte]0); $bw.Write([byte]0); $bw.Write([uint16]1); $bw.Write([uint16]32)
    $bw.Write([uint32]$pngs[$i].Length); $bw.Write([uint32]$off); $off += $pngs[$i].Length }
foreach ($p in $pngs) { $bw.Write($p) }; $bw.Flush(); [IO.File]::WriteAllBytes((Join-Path $gen 'triforce.ico'), $ico.ToArray()); $crest.Dispose()

# ---- publish ----
$out = Join-Path $here 'out'; New-Item -ItemType Directory -Force $out | Out-Null
$numeric = if ($Version -match '(\d+)\.(\d+)\.(\d+)') { "$($Matches[1]).$($Matches[2]).$($Matches[3])" } else { '0.0.0' }
$common = @('-c', 'Release', "-p:Version=$numeric", "-p:InformationalVersion=$Version", "-p:TriforceVersion=$Version", '-nowarn:MSB3277', '-v', 'q')
if ($Rid -in 'all', 'win-x64') {
    & $Dotnet publish (Join-Path $here 'App\Triforce.Launcher.csproj') -f net10.0-windows -r win-x64 -o (Join-Path $out 'win') @common
    if ($LASTEXITCODE) { throw 'windows publish failed' }
    Copy-Item (Join-Path $out 'win\TriforceSetup.exe') $out -Force
}
if ($Rid -in 'all', 'linux-x64') {
    & $Dotnet publish (Join-Path $here 'App\Triforce.Launcher.csproj') -f net10.0 -r linux-x64 -o (Join-Path $out 'linux') @common
    if ($LASTEXITCODE) { throw 'linux publish failed' }
    Copy-Item (Join-Path $out 'linux\triforce-launcher') $out -Force
    Copy-Item (Join-Path $gen 'triforce.png') (Join-Path $out 'triforce.png') -Force
}
Get-ChildItem $out -File | ForEach-Object { '{0,-24} {1,8:N1} MB' -f $_.Name, ($_.Length / 1MB) }
