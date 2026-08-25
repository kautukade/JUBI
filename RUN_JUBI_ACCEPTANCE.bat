@echo off
setlocal
cd /d "%~dp0"
set "PY=.sarus-venv\Scripts\python.exe"
if exist "%PY%" goto run
where py >nul 2>nul
if errorlevel 1 (
  echo Python was not found.
  pause
  exit /b 1
)
set "PY=py -3.11"
:run
%PY% -m jubi.acceptance --full
set "RC=%ERRORLEVEL%"
echo.
if "%RC%"=="0" (
  echo Jubi acceptance PASSED.
) else (
  echo Jubi acceptance FAILED with exit code %RC%.
)
pause
exit /b %RC%
