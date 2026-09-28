# uninstall.ps1 — Remove sparse package + COM registration
# Must run as Administrator.

$ErrorActionPreference = "Stop"

$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) { throw "This script must be run as Administrator!" }

Write-Host "=== Uninstalling Shortcut Grouper ===" -ForegroundColor Cyan

# Remove AppxPackage
$pkg = Get-AppxPackage -Name "ShortcutGrouper" -ErrorAction SilentlyContinue
if ($pkg)
{
    Remove-AppxPackage -Package $pkg.PackageFullName -ErrorAction SilentlyContinue
    Write-Host "AppxPackage removed." -ForegroundColor Green
}

# Deregister COM. DllUnregisterServer cleans HKCR + HKCU\Software\Classes,
# but old versions left extra keys — remove them explicitly too.
$dllCandidates = @(
    (Join-Path $PSScriptRoot "..\build\bin\Release\ShellExtension.dll"),
    (Join-Path ${env:ProgramFiles} "ShortcutGrouper\ShellExtension.dll")
)
$dll = $dllCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if ($dll)
{
    Start-Process "C:\Windows\System32\regsvr32.exe" -ArgumentList "/s /u `"$dll`"" -Wait
    Write-Host "DLL unregistered via regsvr32: $dll" -ForegroundColor Green
}

$clsidGroup = "{B5E3C5A1-7D4F-4E8B-9A2C-1F6D8E3B5A7C}"
$clsidFilter = "{D4E8C2A1-3F5B-4D9A-AE7C-3B1DF29E6C04}"
# NOTE: filter CLSID must match DesktopOpenWithFilter.h (D4E8C2A1-...-F29E6C04).
foreach ($root in @("HKCR", "HKCU\Software\Classes"))
{
    reg delete "$root\CLSID\$clsidGroup" /f 2>$null
    reg delete "$root\CLSID\$clsidFilter" /f 2>$null
    foreach ($cls in @("lnkfile", "*", "Directory"))
    {
        reg delete "$root\$cls\shell\GroupShortcuts" /f 2>$null
        # Leftovers from old versions (bogus rundll32 \command, legacy shellex).
        reg delete "$root\$cls\shellex\ContextMenuHandlers\GroupShortcuts" /f 2>$null
    }
    reg delete "$root\*\shellex\ContextMenuHandlers\DesktopOpenWithFilter" /f 2>$null
}
Write-Host "COM entries removed (both CLSIDs, all verbs, legacy leftovers)." -ForegroundColor Green

# Remove autostart (HKCU Run value + StartupApproved consent + app settings).
# NOTE: when run elevated, HKCU is the ADMIN's hive — also clean the
# interactive user's hive so autostart doesn't survive there.
reg delete "HKCU\Software\Microsoft\Windows\CurrentVersion\Run" /v DesktopGroupManager /f 2>$null
reg delete "HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\Run" /v DesktopGroupManager /f 2>$null
reg delete "HKCU\Software\DesktopGroupManager" /f 2>$null
try
{
    $explorerUser = (Get-CimInstance Win32_Process -Filter "Name='explorer.exe'" -ErrorAction Stop |
        Select-Object -First 1)
    $owner = Invoke-CimMethod -InputObject $explorerUser -MethodName GetOwner -ErrorAction Stop
    if ($owner.User -and ($owner.User -ne $env:USERNAME))
    {
        Write-Host "  NOTE: elevated HKCU is '$env:USERNAME', interactive user is '$($owner.User)'." -ForegroundColor Yellow
        Write-Host "  Autostart entries above were cleaned for the admin hive only." -ForegroundColor Yellow
        Write-Host "  Ask $($owner.User) to run: reg delete HKCU\...\Run /v DesktopGroupManager /f" -ForegroundColor Yellow
    }
}
catch { }
Write-Host "Autostart removed." -ForegroundColor Green

# Remove self-signed cert (ours only: subject match) from both stores.
foreach ($store in @("Cert:\LocalMachine\Root", "Cert:\CurrentUser\Root"))
{
    Get-ChildItem $store -ErrorAction SilentlyContinue |
        Where-Object { $_.Subject -like "*CN=ShortcutGrouper*" } |
        ForEach-Object { Remove-Item "$store\$($_.Thumbprint)" -ErrorAction SilentlyContinue }
}
Write-Host "Certificate removed (subject match)." -ForegroundColor Green

# Clean staging: current (ProgramData) + legacy TEMP location.
$stageDir = Join-Path $env:ProgramData "ShortcutGrouper\sparse"
if (Test-Path $stageDir) { Remove-Item $stageDir -Recurse -Force }
$legacyTemp = Join-Path $env:TEMP "ShortcutGrouper_sparse"
if (Test-Path $legacyTemp) { Remove-Item $legacyTemp -Recurse -Force }
Write-Host "Staging cleaned." -ForegroundColor Green

# Restart Explorer
Write-Host "Restarting Explorer..."
taskkill /f /im explorer.exe 2>$null
Start-Sleep -Seconds 2
Start-Process explorer.exe

Write-Host "=== Uninstall Complete ===" -ForegroundColor Green
