@echo off
rem Cleanly unload BOTH mods from the running game (no game restart).
rem Each mod restores its hooks and frees itself. Re-run inject-both.bat to reload.
cd /d "%~dp0"
echo unload> "%~dp0wsdraw-cmd.txt"
echo unload> "%~dp0coop-cmd.txt"
echo Sent unload to overlay + co-op mod. Both unhook and unload within ~1-2 seconds.
echo (Run inject-both.bat to load them again.)
timeout /t 3 >nul
