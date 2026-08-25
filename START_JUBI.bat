@echo off
setlocal
cd /d "%~dp0"
set "PY=.sarus-venv\Scripts\python.exe"
if exist "%PY%" goto run
where py >nul 2>nul
if errorlevel 1 (
  echo Jubi could not find its private Python runtime or the Windows py launcher.
  echo Run the Jubi installer first.
  pause
  exit /b 1
)
set "PY=py -3.11"
:run
echo Starting Jubi at http://127.0.0.1:8877 ...
%PY% -m jubi.server
set "RC=%ERRORLEVEL%"
if not "%RC%"=="0" (
  echo.
  echo Jubi stopped with exit code %RC%.
  pause
)
exit /b %RC%
