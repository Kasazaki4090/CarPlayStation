@echo off
setlocal
cd /d "%~dp0"
echo =======================================================
echo   Downloading libmpv-2.dll for CarPlayStation
echo =======================================================
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0download_libmpv.ps1" %*
if %ERRORLEVEL% neq 0 (
    echo.
    echo [ERROR] Download or extraction failed!
    pause
    exit /b %ERRORLEVEL%
)
echo.
echo [DONE] libmpv-2.dll is ready.
pause

