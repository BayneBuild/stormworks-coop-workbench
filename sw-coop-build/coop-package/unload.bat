@echo off
rem Cleanly unload the mod from the running game (no game restart).
rem Restores all hooks and frees coopworkbench.dll. Re-run inject.bat to load it again.
cd /d "%~dp0"
echo unload> "%~dp0coopworkbench-cmd.txt"
echo Sent unload command. The mod will unhook and unload within ~1-2 seconds.
echo (Run inject.bat to load it again.)
timeout /t 3 >nul
