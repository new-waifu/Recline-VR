@echo off
rem Uninstalls Recline VR; see uninstall.ps1.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0uninstall.ps1" %*
exit /b %ERRORLEVEL%
