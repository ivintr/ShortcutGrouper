# Uninstall.ps1 — removes Win11 Shortcut Grouper installed by Setup.exe.
# Installed to %ProgramFiles%\Win11ShortcutGrouper with a Start Menu shortcut.
# Self-elevates. Safe to delete its own directory at the end: PowerShell
# parses the whole script before executing it.

param(
    [string]$InstallDir = (Split-Path -Parent $PSCommandPath)
)

$ErrorActionPreference = "Stop"

$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
    ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin)
{
    $args = "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`" -InstallDir `"$InstallDir`""
    Start-Process powershell -ArgumentList $args -Verb RunAs -Wait
    exit $LASTEXITCODE
}

Write-Host "=== Uninstalling Win11 Shortcut Grouper ===" -ForegroundColor Cyan

# Stop the app (otherwise exe/dll are locked).
Get-Process GroupManager -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 2

# Remove AppxPackage.
$pkg = Get-AppxPackage -Name "Win11ShortcutGrouper" -ErrorAction SilentlyContinue
if ($pkg)
{
    Remove-AppxPackage -Package $pkg.PackageFullName -ErrorAction SilentlyContinue
    Write-Host "AppxPackage removed." -ForegroundColor Green
}

# Unregister COM (DllUnregisterServer cleans HKCR + HKCU\Software\Classes).
$dllPath = Join-Path $InstallDir "ShellExtension.dll"
if (Test-Path $dllPath)
{
    & "C:\Windows\System32\regsvr32.exe" /s /u "$dllPath"
    Write-Host "DLL unregistered." -ForegroundColor Green
}
$clsidGroup = "{B5E3C5A1-7D4F-4E8B-9A2C-1F6D8E3B5A7C}"
$clsidFilter = "{D4E8C2A1-3F5B-4D9A-AE7C-3B1DF29E6C04}"
foreach ($root in @("HKCR", "HKCU\Software\Classes"))
{
    reg delete "$root\CLSID\$clsidGroup" /f 2>$null
    reg delete "$root\CLSID\$clsidFilter" /f 2>$null
    foreach ($cls in @("lnkfile", "*", "Directory"))
    {
        reg delete "$root\$cls\shell\GroupShortcuts" /f 2>$null
        reg delete "$root\$cls\shellex\ContextMenuHandlers\GroupShortcuts" /f 2>$null
    }
    reg delete "$root\*\shellex\ContextMenuHandlers\DesktopOpenWithFilter" /f 2>$null
}
Write-Host "COM entries removed." -ForegroundColor Green

# Remove autostart + app settings (current user hive).
reg delete "HKCU\Software\Microsoft\Windows\CurrentVersion\Run" /v DesktopGroupManager /f 2>$null
reg delete "HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\Run" /v DesktopGroupManager /f 2>$null
reg delete "HKCU\Software\DesktopGroupManager" /f 2>$null
Write-Host "Autostart removed." -ForegroundColor Green

# Remove self-signed cert (ours only: subject match).
foreach ($store in @("Cert:\LocalMachine\Root", "Cert:\CurrentUser\Root"))
{
    Get-ChildItem $store -ErrorAction SilentlyContinue |
        Where-Object { $_.Subject -like "*CN=Win11ShortcutGrouper*" } |
        ForEach-Object { Remove-Item "$store\$($_.Thumbprint)" -ErrorAction SilentlyContinue }
}
Write-Host "Certificate removed." -ForegroundColor Green

# Remove Start Menu folder.
$smDir = Join-Path $env:ProgramData "Microsoft\Windows\Start Menu\Programs\Win11 Shortcut Grouper"
if (Test-Path $smDir) { Remove-Item $smDir -Recurse -Force }
Write-Host "Start Menu cleaned." -ForegroundColor Green

# Remove install directory (self-deletion at the end is safe).
if (Test-Path $InstallDir) { Remove-Item $InstallDir -Recurse -Force }
Write-Host "Files removed." -ForegroundColor Green

# Restart Explorer.
Write-Host "Restarting Explorer..."
taskkill /f /im explorer.exe 2>$null
Start-Sleep -Seconds 2
Start-Process explorer.exe

Write-Host "=== Uninstall Complete ===" -ForegroundColor Green
