@echo off
setlocal
cd /d "%~dp0"
echo ==========================================
echo        SARUS Ring0 Bridge Installer
echo ==========================================
echo.
if not exist "driver\SarusRing0\INSTALL-RING0.ps1" (
  echo ERROR: Ring0 installer files are missing.
  pause
  exit /b 1
)
powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "Start-Process powershell.exe -Verb RunAs -Wait -ArgumentList '-NoProfile -ExecutionPolicy Bypass -File ""%~dp0driver\SarusRing0\INSTALL-RING0.ps1""'"
set "RC=%ERRORLEVEL%"
if not "%RC%"=="0" (
  echo Ring0 installation did not complete successfully.
  pause
)
exit /b %RC%
