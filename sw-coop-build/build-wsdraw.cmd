@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 ( echo [build] vcvars64 failed & exit /b 1 )
cd /d "%~dp0"
cl /nologo /LD /EHa /O2 /Fe:wsdraw.dll hook_dll\wsdraw.cpp
if errorlevel 1 ( echo [build] COMPILE FAILED & exit /b 1 )
echo [build] OK - wsdraw.dll
