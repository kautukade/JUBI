@echo off
setlocal
cd /d "%~dp0"
echo ==========================================
echo       SARUS Windows Installer
echo ==========================================
echo.
if not exist "installer\INSTALL-SARUS.ps1" (
  echo ERROR: installer\INSTALL-SARUS.ps1 not found.
  pause
  exit /b 1
)
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0installer\INSTALL-SARUS.ps1"
set "RC=%ERRORLEVEL%"
if not "%RC%"=="0" (
  echo.
  echo SARUS installation returned error %RC%.
  pause
)
exit /b %RC%
