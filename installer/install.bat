@echo off
rem Entry point of Setup.exe payload (runs in extraction temp dir).
rem Install.ps1 self-elevates via UAC when run as regular user.
rem All output goes to %TEMP%\wsg_install.log (console may close early).
set LOG=%TEMP%\wsg_install.log
echo [%DATE% %TIME%] Setup started > "%LOG%"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Install.ps1" >> "%LOG%" 2>&1
echo [%DATE% %TIME%] ExitCode=%ERRORLEVEL% >> "%LOG%"
type "%LOG%"
pause
