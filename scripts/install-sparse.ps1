# install-sparse.ps1 — Install sparse package + register COM DLL
# Must run as Administrator.

param(
    [string]$Configuration = "Release"
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path | Split-Path -Parent

# --- Check admin ---
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) { throw "This script must be run as Administrator!" }

$MsixPath = Join-Path $Root "build\bin\$Configuration\ShortcutGrouper.msix"
$StagingDir = Join-Path $Root "build\msix"
$CertFile = Join-Path $Root "cert\sparse.pfx"
$CertPassword = if ($env:SPARSE_PFX_PASSWORD) { $env:SPARSE_PFX_PASSWORD } else { "12345" }
$DllPath = Join-Path $Root "build\bin\$Configuration\ShellExtension.dll"
# Стейджинг sparse-пакета — в ProgramData, а НЕ в $env:TEMP: очистка TEMP
# (Disk Cleanup/пользователь) молча убивала и ExternalLocation пакета,
# и exe, прописанный в автозапуске.
$extractDir = Join-Path $env:ProgramData "ShortcutGrouper\sparse"
$legacyTempDir = Join-Path $env:TEMP "ShortcutGrouper_sparse"

if (!(Test-Path $MsixPath)) { throw "MSIX not found at $MsixPath. Run build-sparse.ps1 first." }
if (!(Test-Path $DllPath))  { throw "DLL not found at $DllPath. Build the project first." }

Write-Host "=== Installing Shortcut Grouper ===" -ForegroundColor Cyan

# ---- Step 1: Import certificate to LocalMachine\Root ----
if (Test-Path $CertFile)
{
    Write-Host "`n[1/5] Importing certificate to LocalMachine\Root..."
    $securePwd = ConvertTo-SecureString $CertPassword -AsPlainText -Force
    $cert = New-Object System.Security.Cryptography.X509Certificates.X509Certificate2(
        (Resolve-Path $CertFile).Path, $securePwd)

    $store = New-Object System.Security.Cryptography.X509Certificates.X509Store(
        "Root", "LocalMachine")
    $store.Open("ReadWrite")
    $store.Add($cert)
    $store.Close()

    # Also add to CurrentUser\Root for good measure
    $store2 = New-Object System.Security.Cryptography.X509Certificates.X509Store(
        "Root", "CurrentUser")
    $store2.Open("ReadWrite")
    $store2.Add($cert)
    $store2.Close()

    Write-Host "  Certificate imported (Thumbprint: $($cert.Thumbprint))" -ForegroundColor Green
}
else
{
    Write-Host "  WARNING: No certificate at $CertFile" -ForegroundColor Yellow
}

# ---- Step 2: Enable Developer Mode ----
Write-Host "`n[2/5] Enabling Developer Mode..."
$devModePath = "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\AppModelUnlock"
$val = Get-ItemProperty $devModePath -ErrorAction SilentlyContinue
if ($val.AllowDevelopmentWithoutDevLicense -ne 1)
{
    Set-ItemProperty $devModePath -Name AllowDevelopmentWithoutDevLicense -Value 1 -Type DWord
    Set-ItemProperty $devModePath -Name AllowAllTrustedApps -Value 1 -Type DWord
    Write-Host "  Developer Mode enabled" -ForegroundColor Green
}
else
{
    Write-Host "  Developer Mode already enabled" -ForegroundColor Green
}

# ---- Step 3: Uninstall old package if present ----
Write-Host "`n[3/5] Cleaning old package..."
$oldPkg = Get-AppxPackage -Name "ShortcutGrouper" -ErrorAction SilentlyContinue
if ($oldPkg)
{
    Remove-AppxPackage -Package $oldPkg.PackageFullName -ErrorAction SilentlyContinue
    Write-Host "  Old package removed" -ForegroundColor Green
}

# ---- Step 4: Register sparse package ----
Write-Host "`n[4/5] Registering sparse package..."

# Extract MSIX to stable machine-wide staging for sparse package registration.
# NOTE: no $env:TEMP here (see $extractDir definition above).
if (Test-Path $extractDir) { Remove-Item $extractDir -Recurse -Force }
New-Item -ItemType Directory -Path $extractDir -Force | Out-Null
# Миграция со старых версий: чистим legacy-стейджинг в TEMP.
if (Test-Path $legacyTempDir) { Remove-Item $legacyTempDir -Recurse -Force -ErrorAction SilentlyContinue }

Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
[System.IO.Compression.ZipFile]::ExtractToDirectory($MsixPath, $extractDir)
Write-Host "  MSIX extracted to: $extractDir"

# Register sparse package with -ExternalLocation
$registerOk = $false
try
{
    Add-AppxPackage -Register (Join-Path $extractDir "AppxManifest.xml") -ExternalLocation $extractDir -ErrorAction Stop
    Write-Host "  Sparse package registered via -Register -ExternalLocation" -ForegroundColor Green
    $registerOk = $true
}
catch
{
    Write-Host "  -Register -ExternalLocation failed: $_" -ForegroundColor Yellow
    Write-Host "  Trying -Register without -ExternalLocation..."
    try
    {
        Add-AppxPackage -Register (Join-Path $extractDir "AppxManifest.xml") -ErrorAction Stop
        Write-Host "  Sparse package registered via -Register" -ForegroundColor Green
        $registerOk = $true
    }
    catch
    {
        throw "Sparse package registration failed (no silent -Path fallback: it would change install type silently): $_"
    }
}

# Verify package
$pkg = Get-AppxPackage -Name "ShortcutGrouper" -ErrorAction SilentlyContinue
if ($pkg)
{
    Write-Host "  Package verified: $($pkg.Name) v$($pkg.Version)" -ForegroundColor Green
}
else
{
    Write-Host "  WARNING: Package not found after install" -ForegroundColor Yellow
}

# ---- Step 5: Register COM DLL ----
Write-Host "`n[5/5] Registering COM DLL..."
$regProc = Start-Process "C:\Windows\System32\regsvr32.exe" -ArgumentList "/s `"$DllPath`"" -Verb RunAs -Wait -PassThru
if ($regProc.ExitCode -ne 0) { throw "regsvr32 failed with exit code $($regProc.ExitCode) for $DllPath" }
Write-Host "  DLL registered: $DllPath" -ForegroundColor Green

# ---- Restart Explorer (needed to pick up the new shell extension) ----
$exp = Get-Process explorer -ErrorAction SilentlyContinue
if ($exp)
{
    Write-Host "`nRestarting Explorer (reload shell extension)..."
    taskkill /f /im explorer.exe 2>$null
    Start-Sleep -Seconds 2
    Start-Process explorer.exe
    Start-Sleep -Seconds 5
}
else
{
    Write-Host "`nExplorer is not running, skipping restart." -ForegroundColor Yellow
}

Write-Host ""
Write-Host "NOTE: autostart is per-user (HKCU...\Run) and is enabled on the first" -ForegroundColor Yellow
Write-Host "launch of GroupManager.exe — no admin needed. If Task Manager shows it" -ForegroundColor Yellow
Write-Host "disabled, re-enable the toggle in the app Settings (it now clears the" -ForegroundColor Yellow
Write-Host "StartupApproved block as well)." -ForegroundColor Yellow

Write-Host ""
Write-Host "=== Installation Complete ===" -ForegroundColor Green
Write-Host ""
Write-Host "Test instructions:" -ForegroundColor Yellow
Write-Host "  1. Place 2+ .lnk shortcut files on the desktop"
Write-Host "  2. Select them (Ctrl+Click or rubber band)"
Write-Host "  3. Right-click the selection"
Write-Host "  4. Look for 'Group Shortcuts' in the command bar (top row)"
Write-Host "  5. If not in command bar, click 'Show more options' -> should appear there"
Write-Host ""
Write-Host "If neither appears, run: .\scripts\debug-menu.ps1" -ForegroundColor Yellow
