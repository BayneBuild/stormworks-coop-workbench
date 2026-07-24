@echo off
rem Cleanly unload the co-op mod from the running game (no game restart).
rem Restores all hooks and frees coop.dll. Re-run inject-coop.bat to load it again.
cd /d "%~dp0"
echo unload> "%~dp0coop-cmd.txt"
echo Sent unload command. The mod will unhook and unload within ~1 second.
echo (Run inject-coop.bat to load it again.)
timeout /t 2 >nul
