@echo off
rem Coop Workbench installer. Runs install.ps1 next to this file, which locates Stormworks via Steam's own
rem library index, shows exactly what it will copy, and waits for a yes before writing anything.
cd /d "%~dp0"
if not exist "%~dp0install.ps1" goto :nozip
if not exist "%~dp0coopworkbench.asi" goto :nozip
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1"
exit /b

:nozip
echo.
echo   Files are missing next to this script.
echo   You are probably running this from INSIDE the .zip.
echo   Extract the whole folder somewhere first, then run install.bat from there.
echo.
pause
