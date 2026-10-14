@echo off
rem Installs Recline VR for the current user; see install.ps1.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1" %*
exit /b %ERRORLEVEL%
