@echo off
cd /d "%~dp0"
title TCM HMI
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\start-hmi.ps1"
if errorlevel 1 (
  echo.
  echo El HMI se detuvo con error.
  pause
)
exit /b %ERRORLEVEL%
