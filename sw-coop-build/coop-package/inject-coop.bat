@echo off
cd /d "%~dp0"
if not exist "%~dp0inject-coop.ps1" goto notextracted
if not exist "%~dp0coop.dll" goto notextracted
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0inject-coop.ps1"
echo.
pause
exit /b 0

:notextracted
echo.
echo   ============================================================
echo    Missing files next to this launcher.
echo    You are probably running this from INSIDE the .zip.
echo    EXTRACT / unzip the whole folder to your Desktop first,
echo    then open the extracted folder and run inject-coop.bat.
echo   ============================================================
echo.
pause
exit /b 1
