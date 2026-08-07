@echo off
setlocal
cd /d "%~dp0\.."
where py >nul 2>nul && (set "PY=py -3") || set "PY=python"
%PY% -m sarus.acceptance --full
pause
