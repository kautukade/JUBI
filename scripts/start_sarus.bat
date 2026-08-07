@echo off
setlocal
cd /d "%~dp0\.."
set "SARUS_PORT=8877"
where py >nul 2>nul && (set "PY=py -3") || set "PY=python"
%PY% -c "import sys; assert sys.version_info >= (3,11)" >nul 2>nul || (echo Python 3.11+ is required.& echo Run SARUS-Setup.exe first.& pause & exit /b 1)
start "" "http://127.0.0.1:8877"
%PY% -m sarus.server
