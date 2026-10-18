@echo off
setlocal
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\local\Start-Local.ps1" -Role Client %*
set "result=%errorlevel%"
if not "%result%"=="0" pause
exit /b %result%
