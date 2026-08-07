@echo off
setlocal
cd /d "%~dp0"
echo =========================================
echo        SARUS ONE-CLICK INSTALLER
echo =========================================
echo Keep Internet ON. This may take time on first install.
echo.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0INSTALL-SARUS.ps1"
set "EC=%ERRORLEVEL%"
echo.
if "%EC%"=="0" (echo SARUS installation finished.) else (echo SARUS installer failed with exit code %EC%.)
pause
exit /b %EC%
