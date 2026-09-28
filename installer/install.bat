@echo off
rem Entry point of Setup.exe payload (runs in extraction temp dir as invoking user).
rem Install.ps1 self-elevates via UAC.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Install.ps1"
pause
