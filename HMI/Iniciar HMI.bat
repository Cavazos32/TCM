@echo off
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\start-hmi.ps1"
exit /b %ERRORLEVEL%
