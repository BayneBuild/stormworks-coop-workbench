@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 ( echo [build] vcvars64 failed & exit /b 1 )
cd /d "%~dp0"
ml64 /nologo /c /Fo hook_dll\detour_detect.obj hook_dll\detour_detect.asm
if errorlevel 1 ( echo [build] ASM1 FAILED & exit /b 1 )
ml64 /nologo /c /Fo hook_dll\detour_add.obj hook_dll\detour_add.asm
if errorlevel 1 ( echo [build] ASM2 FAILED & exit /b 1 )
ml64 /nologo /c /Fo hook_dll\detour_del.obj hook_dll\detour_del.asm
if errorlevel 1 ( echo [build] ASM3 FAILED & exit /b 1 )
ml64 /nologo /c /Fo hook_dll\detour_delarm.obj hook_dll\detour_delarm.asm
if errorlevel 1 ( echo [build] ASM4 FAILED & exit /b 1 )
ml64 /nologo /c /Fo hook_dll\detour_dragarm.obj hook_dll\detour_dragarm.asm
if errorlevel 1 ( echo [build] ASM5 FAILED & exit /b 1 )
ml64 /nologo /c /Fo hook_dll\detour_factory.obj hook_dll\detour_factory.asm
if errorlevel 1 ( echo [build] ASM6 FAILED & exit /b 1 )
ml64 /nologo /c /Fo hook_dll\detour_conn_add.obj hook_dll\detour_conn_add.asm
if errorlevel 1 ( echo [build] ASM7 FAILED & exit /b 1 )
cl /nologo /LD /EHa /O2 /Fe:coopworkbench.dll hook_dll\coop.cpp hook_dll\wsdraw.cpp hook_dll\detour_detect.obj hook_dll\detour_add.obj hook_dll\detour_del.obj hook_dll\detour_delarm.obj hook_dll\detour_dragarm.obj hook_dll\detour_factory.obj hook_dll\detour_conn_add.obj
if errorlevel 1 ( echo [build] COMPILE FAILED & exit /b 1 )
echo [build] OK
