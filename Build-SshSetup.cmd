@echo off
setlocal
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\SshSetup\Build.ps1"
if errorlevel 1 (
  pause
  exit /b 1
)
echo Build completed. Run Start-SshSetup.cmd or Start-HomeSetup.cmd.
pause
