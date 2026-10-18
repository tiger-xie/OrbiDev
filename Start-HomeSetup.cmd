@echo off
setlocal
set "APP=%~dp0build-ssh-setup\Release\OrbiDevHomeSetup.exe"
if not exist "%APP%" (
  echo Run Build-SshSetup.cmd first.
  pause
  exit /b 1
)
start "" "%APP%"
