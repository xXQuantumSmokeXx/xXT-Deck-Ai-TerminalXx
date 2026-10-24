@echo off
setlocal
cd /d "%~dp0"

for /f %%i in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd-HHmmss"') do set STAMP=%%i
if not exist captures mkdir captures

python screenshot.py "captures\screen-%STAMP%.png"

if errorlevel 1 (
  echo.
  echo Capture failed.
  echo   - Is the Ai-Terminal app running, not M5Launcher?
  echo   - Is another program holding the COM port?
) else (
  echo.
  echo Saved captures\screen-%STAMP%.png
)

echo.
pause
