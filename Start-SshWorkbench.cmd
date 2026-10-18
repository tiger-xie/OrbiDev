@echo off
setlocal
set "APP=%~dp0build-ssh-workbench\Release\OrbiDevWorkbench.exe"
if not exist "%APP%" (
  echo Run Build-SshWorkbench.cmd first.
  pause
  exit /b 1
)
start "" "%APP%" --legacy-config "%~dp0.local-run\launcher-projects.json"
