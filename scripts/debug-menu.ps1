# debug-menu.ps1 — Diagnostic script for Shortcut Grouper
# Checks all registration states, certificate trust, event logs.

$clsid = "{B5E3C5A1-7D4F-4E8B-9A2C-1F6D8E3B5A7C}"

Write-Host "============================================" -ForegroundColor Cyan
Write-Host "  Shortcut Grouper — Diagnostics" -ForegroundColor Cyan
Write-Host "============================================" -ForegroundColor Cyan

# ---- 1. Package ----
Write-Host "`n[1] MSIX Sparse Package" -ForegroundColor Yellow
$pkg = Get-AppxPackage -Name "ShortcutGrouper" -ErrorAction SilentlyContinue
if ($pkg)
{
    Write-Host "  Status: INSTALLED" -ForegroundColor Green
    Write-Host "  Name:             $($pkg.Name)"
    Write-Host "  Version:          $($pkg.Version)"
    Write-Host "  InstallLocation:  $($pkg.InstallLocation)"
    Write-Host "  PackageFullName:  $($pkg.PackageFullName)"

    # Check if external location files exist
    if (Test-Path $pkg.InstallLocation)
    {
        $files = Get-ChildItem $pkg.InstallLocation -File | Select-Object Name, Length
        Write-Host "  Files in location:"
        foreach ($f in $files) { Write-Host "    $($f.Name) ($($f.Length) bytes)" }
    }
    else
    {
        Write-Host "  WARNING: InstallLocation does not exist!" -ForegroundColor Red
    }
}
else
{
    Write-Host "  Status: NOT INSTALLED" -ForegroundColor Red
    Write-Host "  This is likely why the button doesn't appear."
    Write-Host "  Run: .\scripts\install-sparse.ps1" -ForegroundColor Yellow
}

# ---- 2. COM Registration ----
Write-Host "`n[2] COM CLSID Registration" -ForegroundColor Yellow
$clsidPath = "Registry::HKEY_CLASSES_ROOT\CLSID\$clsid"
if (Test-Path $clsidPath)
{
    Write-Host "  CLSID: REGISTERED" -ForegroundColor Green

    $inproc = Get-ItemProperty "$clsidPath\InprocServer32" -ErrorAction SilentlyContinue
    if ($inproc)
    {
        $dllPath = $inproc.'(Default)'
        Write-Host "  DLL:   $dllPath"
        Write-Host "  Model: $($inproc.ThreadingModel)"

        if (Test-Path $dllPath)
        {
            $dllInfo = Get-Item $dllPath
            Write-Host "  DLL exists: YES ($($dllInfo.Length) bytes)" -ForegroundColor Green
        }
        else
        {
            Write-Host "  DLL exists: NO!" -ForegroundColor Red
        }
    }
    else
    {
        Write-Host "  InprocServer32: MISSING!" -ForegroundColor Red
    }
}
else
{
    Write-Host "  CLSID: NOT REGISTERED" -ForegroundColor Red
    Write-Host "  Run regsvr32 on ShellExtension.dll"
}

# ---- 3. ExplorerCommandHandler ----
Write-Host "`n[3] Shell Verb (ExplorerCommandHandler)" -ForegroundColor Yellow
$verbPath = "Registry::HKEY_CLASSES_ROOT\lnkfile\shell\GroupShortcuts"
if (Test-Path $verbPath)
{
    Write-Host "  Verb: REGISTERED" -ForegroundColor Green
    $props = Get-ItemProperty $verbPath
    Write-Host "  Display:          $($props.'(Default)')"
    Write-Host "  Icon:             $($props.Icon)"
    Write-Host "  ExplorerCommandHandler: $($props.ExplorerCommandHandler)"

    if ($props.ExplorerCommandHandler -eq $clsid)
    {
        Write-Host "  ExplorerCommandHandler: MATCHES CLSID" -ForegroundColor Green
    }
    else
    {
        Write-Host "  ExplorerCommandHandler: MISMATCH! Expected $clsid" -ForegroundColor Red
    }

    $cmdPath = "$verbPath\command"
    if (Test-Path $cmdPath)
    {
        $cmdProps = Get-ItemProperty $cmdPath
        Write-Host "  command\DelegateExecute: $($cmdProps.DelegateExecute)"
        if ($cmdProps.DelegateExecute -eq $clsid)
        {
            Write-Host "  DelegateExecute: MATCHES CLSID" -ForegroundColor Green
        }
        else
        {
            Write-Host "  DelegateExecute: MISMATCH!" -ForegroundColor Red
        }
    }
}
else
{
    Write-Host "  Verb: NOT REGISTERED" -ForegroundColor Red
    Write-Host "  Run regsvr32 on ShellExtension.dll"
}

# ---- 4. Shellex Handlers ----
Write-Host "`n[4] Shellex ContextMenuHandlers" -ForegroundColor Yellow
$shellexPath = "Registry::HKEY_CLASSES_ROOT\lnkfile\shellex\ContextMenuHandlers\GroupShortcuts"
if (Test-Path $shellexPath)
{
    Write-Host "  GroupShortcuts shellex: PRESENT (legacy leftover — new versions don't register it)" -ForegroundColor Yellow
}
else
{
    Write-Host "  GroupShortcuts shellex: absent (expected — IExplorerCommand needs no shellex)" -ForegroundColor Green
}
$filterPath = "Registry::HKEY_CLASSES_ROOT\*\shellex\ContextMenuHandlers\DesktopOpenWithFilter"
if (Test-Path $filterPath)
{
    Write-Host "  DesktopOpenWithFilter: REGISTERED" -ForegroundColor Green
}
else
{
    Write-Host "  DesktopOpenWithFilter: NOT REGISTERED" -ForegroundColor Yellow
}

# ---- 5. Certificate ----
Write-Host "`n[5] Certificate Trust" -ForegroundColor Yellow
$certRoot = Get-ChildItem Cert:\LocalMachine\Root -CodeSigningCert -ErrorAction SilentlyContinue |
    Where-Object { $_.Subject -like "*ShortcutGrouper*" } | Select-Object -First 1
if ($certRoot)
{
    Write-Host "  LocalMachine\Root: PRESENT" -ForegroundColor Green
    Write-Host "    Subject:    $($certRoot.Subject)"
    Write-Host "    Thumbprint: $($certRoot.Thumbprint)"
    Write-Host "    NotAfter:   $($certRoot.NotAfter)"
    if ($certRoot.NotAfter -lt (Get-Date))
    {
        Write-Host "    WARNING: Certificate has EXPIRED!" -ForegroundColor Red
    }
}
else
{
    Write-Host "  LocalMachine\Root: MISSING" -ForegroundColor Red
    Write-Host "  This is REQUIRED for sparse package trust."
    Write-Host "  Run install-sparse.ps1 (imports cert automatically)" -ForegroundColor Yellow
}

# ---- 6. Developer Mode ----
Write-Host "`n[6] Developer Mode" -ForegroundColor Yellow
$devMode = Get-ItemProperty "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\AppModelUnlock" -ErrorAction SilentlyContinue
if ($devMode.AllowDevelopmentWithoutDevLicense -eq 1)
{
    Write-Host "  Developer Mode: ENABLED" -ForegroundColor Green
}
else
{
    Write-Host "  Developer Mode: DISABLED" -ForegroundColor Red
    Write-Host "  Enable in: Settings -> System -> For developers" -ForegroundColor Yellow
}

# ---- 7. Event Viewer (recent errors) ----
Write-Host "`n[7] Recent Event Viewer Errors (AppX)" -ForegroundColor Yellow
$events = Get-WinEvent -FilterHashtable @{
    LogName = 'Microsoft-Windows-AppXDeploymentServer/Operational'
    Level = 2
    StartTime = (Get-Date).AddHours(-24)
} -ErrorAction SilentlyContinue | Select-Object -First 5

if ($events)
{
    foreach ($event in $events)
    {
        $msg = $event.Message -replace "`n", " " -replace "`r", ""
        if ($msg.Length -gt 200) { $msg = $msg.Substring(0, 200) + "..." }
        Write-Host "  [$($event.TimeCreated)] $msg" -ForegroundColor Red
    }
}
else
{
    Write-Host "  No AppX deployment errors in last 24h" -ForegroundColor Green
}

$shellEvents = Get-WinEvent -FilterHashtable @{
    LogName = 'Microsoft-Windows-Shell-Core/Operational'
    Level = 2
    StartTime = (Get-Date).AddHours(-1)
} -ErrorAction SilentlyContinue | Select-Object -First 3

if ($shellEvents)
{
    Write-Host "`n  Shell-Core errors (last hour):"
    foreach ($event in $shellEvents)
    {
        $msg = $event.Message -replace "`n", " " -replace "`r", ""
        if ($msg.Length -gt 200) { $msg = $msg.Substring(0, 200) + "..." }
        Write-Host "    [$($event.TimeCreated)] $msg" -ForegroundColor Red
    }
}

# ---- 8. Autostart ----
Write-Host "`n[8] Autostart (HKCU Run + StartupApproved)" -ForegroundColor Yellow
$runVal = Get-ItemProperty "HKCU:\Software\Microsoft\Windows\CurrentVersion\Run" -Name DesktopGroupManager -ErrorAction SilentlyContinue
if ($runVal)
{
    Write-Host "  Run value: $($runVal.DesktopGroupManager)" -ForegroundColor Green
}
else
{
    Write-Host "  Run value: MISSING (autostart OFF)" -ForegroundColor Red
}
$appr = Get-ItemProperty "HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\Run" -Name DesktopGroupManager -ErrorAction SilentlyContinue
if ($appr)
{
    $bytes = $appr.DesktopGroupManager
    $state = if ($bytes -and $bytes[0] -eq 3) { "DISABLED by user (Task Manager/Settings) — Explorer ignores Run!" } elseif ($bytes -and $bytes[0] -eq 2) { "enabled" } else { "unknown ($($bytes[0]))" }
    $color = if ($bytes -and $bytes[0] -eq 3) { "Red" } else { "Green" }
    Write-Host "  StartupApproved: $state" -ForegroundColor $color
    if ($bytes -and $bytes[0] -eq 3)
    {
        Write-Host "  Fix: enable the toggle in app Settings, or run:" -ForegroundColor Yellow
        Write-Host '    reg delete "HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\Run" /v DesktopGroupManager /f' -ForegroundColor Yellow
    }
}
else
{
    Write-Host "  StartupApproved: no block (good)" -ForegroundColor Green
}

# ---- Summary ----
Write-Host "`n============================================" -ForegroundColor Cyan
Write-Host "  Summary" -ForegroundColor Cyan
Write-Host "============================================" -ForegroundColor Cyan

$allGood = $true
if (-not $pkg)           { Write-Host "  [FAIL] MSIX package not installed" -ForegroundColor Red; $allGood = $false }
if (-not (Test-Path $clsidPath)) { Write-Host "  [FAIL] COM CLSID not registered" -ForegroundColor Red; $allGood = $false }
if (-not (Test-Path $verbPath))  { Write-Host "  [FAIL] Shell verb not registered" -ForegroundColor Red; $allGood = $false }
if (-not $certRoot)      { Write-Host "  [FAIL] Certificate not in LocalMachine\Root" -ForegroundColor Red; $allGood = $false }

if ($allGood)
{
    Write-Host "  All checks PASSED. Button should appear when selecting 2+ .lnk files." -ForegroundColor Green
    Write-Host "  If it doesn't appear, try: taskkill /f /im explorer.exe; start explorer.exe" -ForegroundColor Yellow
}
