@echo off
echo =============================================
echo  ShortcutGrouper — Registration Script
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
    echo ERROR: ShellExtension.dll not found at:
    echo   %DLL_PATH%
    echo.
    echo Please build the project first:
    echo   cmake -B build -G "Visual Studio 17 2022"
    echo   cmake --build build --config Release
    pause
    exit /b 1
)

echo Registering ShellExtension.dll ...
regsvr32 /s "%DLL_PATH%"
set REG_RC=%errorlevel%

if %REG_RC% equ 0 (
    echo.
    echo SUCCESS: Shell extension registered.
    echo.
    echo To test: Right-click 2+ files or folders on the desktop.
    echo          The "Объединить в группу" button should appear.
    echo.
    echo NOTE: You may need to restart Explorer for changes to take effect:
    echo   taskkill /f /im explorer.exe
    echo   start explorer.exe
) else (
    echo.
    echo ERROR: Registration failed (code %REG_RC%).
    echo Make sure you are running as Administrator.
)

pause
