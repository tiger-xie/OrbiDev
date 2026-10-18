@echo off
setlocal
set "APP=%~dp0build-ssh-setup\Release\OrbiDevSetup.exe"
if not exist "%APP%" (
  echo Run Build-SshSetup.cmd first.
  pause
  exit /b 1
)
start "" "%APP%" --legacy-config "%~dp0.local-run\launcher-projects.json"
