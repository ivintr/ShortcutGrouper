# Install.ps1 — machine installer for Shortcut Grouper.
# Ships inside the IExpress Setup.exe payload (run from extraction temp dir).
# Self-elevates: double-click friendly, UAC prompt appears once.
# Layout: single copy in %ProgramFiles%\ShortcutGrouper, sparse package
# registered with -ExternalLocation on that dir (no TEMP staging).

param(
    [string]$InstallDir = (Join-Path $env:ProgramFiles "ShortcutGrouper")
)

$ErrorActionPreference = "Stop"

# ---- Self-elevate ----
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
    ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin)
{
    Write-Host "Requesting administrator rights..."
    $args = "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`" -InstallDir `"$InstallDir`""
    Start-Process powershell -ArgumentList $args -Verb RunAs -Wait
    exit $LASTEXITCODE
}

$SrcDir = Split-Path -Parent $PSCommandPath
$ExeName = "GroupManager.exe"
$DllName = "ShellExtension.dll"
$MsixName = "ShortcutGrouper.msix"
$CertName = "sparse.pfx"
$CertPassword = if ($env:SPARSE_PFX_PASSWORD) { $env:SPARSE_PFX_PASSWORD } else { "12345" }

foreach ($f in @($ExeName, $DllName, $MsixName, $CertName, "Uninstall.ps1"))
{
    if (!(Test-Path (Join-Path $SrcDir $f))) { throw "Payload file missing: $f" }
}

Write-Host "=== Installing Shortcut Grouper ===" -ForegroundColor Cyan
Write-Host "Target: $InstallDir"

# ---- Step 1: Remove previous installation (package + files) ----
$oldPkg = Get-AppxPackage -Name "ShortcutGrouper" -ErrorAction SilentlyContinue
if ($oldPkg)
{
    Write-Host "[1/7] Removing previous package..."
    Remove-AppxPackage -Package $oldPkg.PackageFullName -ErrorAction SilentlyContinue
}
else
{
    Write-Host "[1/7] No previous package."
}

# ---- Step 2: Copy files ----
Write-Host "[2/7] Copying files..."
New-Item -ItemType Directory -Path $InstallDir -Force | Out-Null
Copy-Item (Join-Path $SrcDir $ExeName) (Join-Path $InstallDir $ExeName) -Force
Copy-Item (Join-Path $SrcDir $DllName) (Join-Path $InstallDir $DllName) -Force
Copy-Item (Join-Path $SrcDir "Uninstall.ps1") (Join-Path $InstallDir "Uninstall.ps1") -Force
# Sparse payload: extract MSIX next to the binaries (single copy on disk).
# NOTE: 3-arg ExtractToDirectory(overwrite) exists only on .NET Core 3.0+,
# Windows PowerShell 5.1 has just the 2-arg overload — clear dest first.
Add-Type -AssemblyName System.IO.Compression.FileSystem
$stageZip = Join-Path $env:TEMP "wsg_payload.zip"
Copy-Item (Join-Path $SrcDir $MsixName) $stageZip -Force
Remove-Item (Join-Path $InstallDir "AppxManifest.xml") -Force -ErrorAction SilentlyContinue
Remove-Item (Join-Path $InstallDir "Assets") -Recurse -Force -ErrorAction SilentlyContinue
[System.IO.Compression.ZipFile]::ExtractToDirectory($stageZip, $InstallDir)
Remove-Item $stageZip -Force -ErrorAction SilentlyContinue
if (!(Test-Path (Join-Path $InstallDir "AppxManifest.xml")))
{
    throw "MSIX extraction failed (no AppxManifest.xml in $InstallDir)"
}
# Legacy TEMP staging from old installs.
$legacyTemp = Join-Path $env:TEMP "ShortcutGrouper_sparse"
if (Test-Path $legacyTemp) { Remove-Item $legacyTemp -Recurse -Force -ErrorAction SilentlyContinue }
Write-Host "  Files ready." -ForegroundColor Green

# ---- Step 3: Certificate to LocalMachine\Root (+ CurrentUser\Root) ----
Write-Host "[3/7] Installing certificate..."
$pfxPath = Join-Path $SrcDir $CertName
$securePwd = ConvertTo-SecureString $CertPassword -AsPlainText -Force
$cert = New-Object System.Security.Cryptography.X509Certificates.X509Certificate2($pfxPath, $securePwd)
foreach ($scope in @("LocalMachine", "CurrentUser"))
{
    $store = New-Object System.Security.Cryptography.X509Certificates.X509Store("Root", $scope)
    $store.Open("ReadWrite")
    $store.Add($cert)
    $store.Close()
}
Write-Host "  Certificate installed ($($cert.Thumbprint))" -ForegroundColor Green

# ---- Step 4: Developer Mode ----
Write-Host "[4/7] Checking Developer Mode..."
$devModePath = "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\AppModelUnlock"
Set-ItemProperty $devModePath -Name AllowDevelopmentWithoutDevLicense -Value 1 -Type DWord -ErrorAction SilentlyContinue
Set-ItemProperty $devModePath -Name AllowAllTrustedApps -Value 1 -Type DWord -ErrorAction SilentlyContinue
Write-Host "  Developer Mode ensured." -ForegroundColor Green

# ---- Step 5: Register sparse package ----
Write-Host "[5/7] Registering sparse package..."
Add-AppxPackage -Register (Join-Path $InstallDir "AppxManifest.xml") -ExternalLocation $InstallDir -ErrorAction Stop
$pkg = Get-AppxPackage -Name "ShortcutGrouper" -ErrorAction SilentlyContinue
if (-not $pkg) { throw "Sparse package registration failed." }
Write-Host "  Package: $($pkg.Name) v$($pkg.Version)" -ForegroundColor Green

# ---- Step 6: Register COM DLL (already elevated) ----
Write-Host "[6/7] Registering COM DLL..."
$dllPath = Join-Path $InstallDir $DllName
& "C:\Windows\System32\regsvr32.exe" /s "$dllPath"
if ($LASTEXITCODE -ne 0) { throw "regsvr32 failed with exit code $LASTEXITCODE" }
Write-Host "  DLL registered." -ForegroundColor Green

# ---- Step 7: Start Menu shortcuts + Explorer restart ----
Write-Host "[7/7] Finishing..."
$smDir = Join-Path $env:ProgramData "Microsoft\Windows\Start Menu\Programs\Shortcut Grouper"
New-Item -ItemType Directory -Path $smDir -Force | Out-Null
$shell = New-Object -ComObject WScript.Shell
$lnk = $shell.CreateShortcut((Join-Path $smDir "Shortcut Grouper.lnk"))
$lnk.TargetPath = Join-Path $InstallDir $ExeName
$lnk.WorkingDirectory = $InstallDir
$lnk.Save()
$un = $shell.CreateShortcut((Join-Path $smDir "Uninstall.lnk"))
$un.TargetPath = "powershell.exe"
$un.Arguments = "-NoProfile -ExecutionPolicy Bypass -File `"$InstallDir\Uninstall.ps1`""
$un.WorkingDirectory = $InstallDir
$un.Save()
[System.Runtime.InteropServices.Marshal]::ReleaseComObject($shell) | Out-Null
Write-Host "  Start Menu shortcuts created." -ForegroundColor Green

$exp = Get-Process explorer -ErrorAction SilentlyContinue
if ($exp)
{
    Write-Host "Restarting Explorer..."
    taskkill /f /im explorer.exe 2>$null
    Start-Sleep -Seconds 2
    Start-Process explorer.exe
    Start-Sleep -Seconds 3
}

Write-Host ""
Write-Host "=== Installation Complete ===" -ForegroundColor Green
Write-Host "Launch 'Shortcut Grouper' from Start Menu." -ForegroundColor Yellow
Write-Host "Autostart enables itself on first launch (per user, no admin needed)." -ForegroundColor Yellow
