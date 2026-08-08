@echo off
setlocal
cd /d "%~dp0"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0installer\SETUP-BROKER.ps1"
if errorlevel 1 (
  echo SARUS broker security setup failed.
  pause
  exit /b 1
)
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0installer\INSTALL-SARUS.ps1"
if errorlevel 1 pause
