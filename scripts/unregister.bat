@echo off
echo =============================================
echo  Win11ShortcutGrouper — Unregistration Script
echo =============================================
echo.

:: Check admin rights.
net session >nul 2>&1
if %errorlevel% neq 0 (
    echo ERROR: This script must be run as Administrator.
    echo Right-click and select "Run as administrator".
    pause
    exit /b 1
)

:: Determine paths.
set SCRIPT_DIR=%~dp0
set DLL_PATH=%SCRIPT_DIR%..\build\bin\Release\ShellExtension.dll

if not exist "%DLL_PATH%" (
    echo WARNING: ShellExtension.dll not found, attempting registry cleanup...
    goto :cleanup
)

echo Unregistering ShellExtension.dll ...
regsvr32 /s /u "%DLL_PATH%"
if %errorlevel% neq 0 (
    echo WARNING: regsvr32 returned %errorlevel%, continuing with manual cleanup...
)

:cleanup
:: Clean up registry keys manually as a safety net (both CLSIDs, all verbs,
:: legacy leftovers from old versions, HKCU mirror).
echo Cleaning up registry entries...
reg delete "HKCR\CLSID\{B5E3C5A1-7D4F-4E8B-9A2C-1F6D8E3B5A7C}" /f >nul 2>&1
reg delete "HKCR\CLSID\{D4E8C2A1-3F5B-4D9A-AE7C-3B1DF29E6C04}" /f >nul 2>&1
reg delete "HKCU\Software\Classes\CLSID\{B5E3C5A1-7D4F-4E8B-9A2C-1F6D8E3B5A7C}" /f >nul 2>&1
reg delete "HKCU\Software\Classes\CLSID\{D4E8C2A1-3F5B-4D9A-AE7C-3B1DF29E6C04}" /f >nul 2>&1
for %%C in (lnkfile * Directory) do (
    reg delete "HKCR\%%C\shell\GroupShortcuts" /f >nul 2>&1
    reg delete "HKCR\%%C\shellex\ContextMenuHandlers\GroupShortcuts" /f >nul 2>&1
    reg delete "HKCU\Software\Classes\%%C\shell\GroupShortcuts" /f >nul 2>&1
    reg delete "HKCU\Software\Classes\%%C\shellex\ContextMenuHandlers\GroupShortcuts" /f >nul 2>&1
)
reg delete "HKCR\*\shellex\ContextMenuHandlers\DesktopOpenWithFilter" /f >nul 2>&1
reg delete "HKCU\Software\Classes\*\shellex\ContextMenuHandlers\DesktopOpenWithFilter" /f >nul 2>&1

echo.
echo SUCCESS: Shell extension unregistered.
echo.
echo NOTE: You may need to restart Explorer for changes to take effect:
echo   taskkill /f /im explorer.exe
echo   start explorer.exe

pause
