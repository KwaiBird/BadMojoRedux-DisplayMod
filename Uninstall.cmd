@echo off
setlocal
set "log=%~dp0BadMojoMod-uninstall.log"
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "%~dp0Uninstall-BadMojoMod.ps1"
set "result=%errorlevel%"
if not "%result%"=="0" (
    echo.
    echo Uninstall stopped. Read the error above and %log%.
    echo.
    pause
    exit /b %result%
)
del /f /q "%~dp0Uninstall-BadMojoMod.ps1"
if exist "%~dp0Uninstall-BadMojoMod.ps1" (
    >> "%log%" echo Could not remove Uninstall-BadMojoMod.ps1.
    echo Could not remove Uninstall-BadMojoMod.ps1. See %log%.
    pause
    exit /b 1
)
>> "%log%" echo Removed Uninstall-BadMojoMod.ps1.
>> "%log%" echo Removing Uninstall.cmd; uninstall log remains here.
echo Uninstall complete. The original launcher and SAVE directory are preserved.
echo Log: %log%
(goto) 2>nul & del /f /q "%~f0"
