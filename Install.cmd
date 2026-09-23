@echo off
setlocal
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "%~dp0Install-BadMojoMod.ps1"
set "result=%errorlevel%"
if not "%result%"=="0" (
    echo.
    echo Installation failed. Read the error above.
)
echo.
pause
exit /b %result%
