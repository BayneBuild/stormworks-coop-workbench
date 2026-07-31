@echo off
rem Removes exactly the files install.bat added. Leaves the plugins folder alone unless it is empty.
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0uninstall.ps1"
exit /b
