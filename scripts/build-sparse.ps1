# build-sparse.ps1 — Build MSIX via OPC API (bypasses MakeAppx schema validation)
#
# Strategy: MakeAppx creates a structurally valid MSIX (minimal manifest),
# then we replace AppxManifest.xml with the full manifest containing
# desktop4:Extension for context menus. SignTool then signs the result.

param(
    [string]$Configuration = "Release",
    [string]$Version = "1.0.0.0"
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path | Split-Path -Parent
$BuildDir = Join-Path $Root "build\bin\$Configuration"
$MsixDir = Join-Path $Root "build\msix"
$MsixPath = Join-Path $BuildDir "Win11ShortcutGrouper.msix"
$MsixUnsigned = Join-Path $BuildDir "Win11ShortcutGrouper_unsigned.msix"
$CertFile = Join-Path $Root "cert\sparse.pfx"
$CertPassword = if ($env:SPARSE_PFX_PASSWORD) { $env:SPARSE_PFX_PASSWORD } else { "12345" }

if ($Version -notmatch '^\d+\.\d+\.\d+\.\d+$') { throw "Version must be N.N.N.N, got: $Version" }
if (-not $env:SPARSE_PFX_PASSWORD)
{
    Write-Host "WARNING: SPARSE_PFX_PASSWORD not set, using default dev password." -ForegroundColor Yellow
}

# Find tools. NOTE: берём каталог ВЕРСИИ (10.0.xxxxx.0), а не первый попавшийся:
# сортировка всех подкаталогов давала arch-папку (x86) вместо версии.
$SDKRoot = "${env:ProgramFiles(x86)}\Windows Kits\10\bin"
$SDKVersion = (Get-ChildItem $SDKRoot -Directory |
    Where-Object { $_.Name -match '^\d+\.\d+\.\d+\.\d+$' } |
    Sort-Object Name -Descending | Select-Object -First 1).Name
if (-not $SDKVersion) { throw "No Windows SDK version found under $SDKRoot" }
$ArchDir = Join-Path $SDKRoot "$SDKVersion\x64"
$MakeAppx = Join-Path $ArchDir "MakeAppx.exe"
$SignTool = Join-Path $ArchDir "SignTool.exe"
if (!(Test-Path $MakeAppx)) { throw "MakeAppx not found at $MakeAppx" }
if (!(Test-Path $SignTool)) { throw "SignTool not found at $SignTool" }

Write-Host "=== Building MSIX Sparse Package ===" -ForegroundColor Cyan
Write-Host "SDK: $SDKVersion"

# ---- Step 1: Stage files with the FULL manifest ----
# Пакуем сразу полный манифест (desktop4:Extension) с флагом /nv:
# старый путь (минимальный манифест + OPC-замена через System.IO.Packaging)
# давал ZIP, который SignTool отвергает ("file format cannot be signed").
if (Test-Path $MsixDir) { Remove-Item $MsixDir -Recurse -Force }
New-Item -ItemType Directory -Path "$MsixDir\Assets" -Force | Out-Null

Copy-Item (Join-Path $BuildDir "ShellExtension.dll") $MsixDir
Copy-Item (Join-Path $BuildDir "GroupManager.exe") $MsixDir

$fullManifestPath = Join-Path $Root "src\AppxManifest.xml"
$fullManifestXml = [System.IO.File]::ReadAllText($fullManifestPath, [System.Text.Encoding]::UTF8)
# Версия пакета задаётся параметром сборки, а не захардкожена в манифесте
# (иначе апгрейды пакета не детектятся системой).
$fullManifestXml = [regex]::Replace($fullManifestXml,
    '(<Identity\b[^>]*?\bVersion=")[^"]+(")', "`${1}$Version`$2")
[System.IO.File]::WriteAllText((Join-Path $MsixDir "AppxManifest.xml"),
    $fullManifestXml, [System.Text.UTF8Encoding]::new($false))
Write-Host "  Full manifest staged (v$Version)" -ForegroundColor Green

# Dummy PNGs
$dummyPng = [byte[]](0x89,0x50,0x4E,0x47,0x0D,0x0A,0x1A,0x0A,0x00,0x00,0x00,0x0D,
    0x49,0x48,0x44,0x52,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x01,0x08,0x06,
    0x00,0x00,0x00,0x1F,0x15,0xC4,0x89,0x00,0x00,0x00,0x0A,0x49,0x44,0x41,
    0x54,0x78,0x9C,0x62,0x00,0x00,0x00,0x02,0x00,0x01,0xE5,0x27,0xDE,0xFC,
    0x00,0x00,0x00,0x00,0x49,0x45,0x4E,0x44,0xAE,0x42,0x60,0x82)
foreach ($logo in @("StoreLogo.png","Square150x150Logo.png","Square44x44Logo.png","Wide310x150Logo.png"))
{
    [System.IO.File]::WriteAllBytes((Join-Path $MsixDir "Assets\$logo"), $dummyPng)
}

# ---- Step 2: MakeAppx pack (direct, with /nv) ----
if (Test-Path $MsixUnsigned) { Remove-Item $MsixUnsigned -Force }
Write-Host "[1/3] Creating MSIX with MakeAppx (/nv: full manifest)..."
& $MakeAppx pack /d $MsixDir /p $MsixUnsigned /o /nv
if ($LASTEXITCODE -ne 0) { throw "MakeAppx failed" }

# Проверка содержимого: манифест внутри пакета — наш (полный, нужной версии).
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [System.IO.Compression.ZipFile]::OpenRead($MsixUnsigned)
try
{
    $entry = $zip.Entries | Where-Object { $_.FullName -eq "AppxManifest.xml" } | Select-Object -First 1
    if (-not $entry) { throw "AppxManifest.xml missing in packed MSIX" }
    $sr = New-Object System.IO.StreamReader($entry.Open(), [System.Text.Encoding]::UTF8)
    $vText = $sr.ReadToEnd()
    $sr.Close()
    if (-not $vText.Contains("Win11ShortcutGrouper") -or
        -not $vText.Contains("desktop4:Extension") -or
        -not $vText.Contains("Version=`"$Version`""))
    {
        throw "Packed manifest is not ours (missing desktop4:Extension or v$Version)"
    }
    Write-Host "  Manifest verified (full, v$Version)" -ForegroundColor Green
}
finally
{
    $zip.Dispose()
}

# ---- Step 3: Sign ----
Write-Host "[2/3] Signing MSIX..."
if (Test-Path $MsixPath) { Remove-Item $MsixPath -Force }
Copy-Item $MsixUnsigned $MsixPath

if (Test-Path $CertFile)
{
    & $SignTool sign /fd SHA256 /a /f $CertFile /p $CertPassword $MsixPath
    if ($LASTEXITCODE -ne 0) { throw "SignTool failed" }
    Write-Host "  Signed OK" -ForegroundColor Green
}
else
{
    Write-Host "  WARNING: No certificate" -ForegroundColor Yellow
}

Write-Host "[3/3] Cleanup..."
Remove-Item $MsixUnsigned -Force -ErrorAction SilentlyContinue

Write-Host ""
Write-Host "=== Done: $MsixPath ($((Get-Item $MsixPath).Length) bytes) ===" -ForegroundColor Green
