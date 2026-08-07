@echo off
setlocal
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0UPLOAD_RELEASE.ps1"
if errorlevel 1 pause
endlocal
