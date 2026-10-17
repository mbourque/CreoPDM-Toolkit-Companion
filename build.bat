@echo off
rem Builds bin\creopdm_structure.dll (64-bit) against the Toolkit SDK of the Creo install in CREO_HOME.
rem Default target: Creo 10 (oldest supported release; the DLL is meant to load in 10 and later).
rem Override:  set CREO_HOME=C:\PTC\Creo 13.4.1.0   before running build.bat / unlock.bat
rem Optional:  set OUT_DLL=bin\creopdm_structure.new.dll   to build a copy while Creo is running (Creo locks the real DLL);
rem            then run   set OUT_DLL=...  &  unlock.bat   and copy it over the real DLL after closing Creo.
setlocal
if "%OUT_DLL%"=="" set "OUT_DLL=bin\creopdm_structure.dll"
if "%CREO_HOME%"=="" set "CREO_HOME=C:\PTC\Creo 10.0.10.0"
set "TK=%CREO_HOME%\Common Files\protoolkit"
set "LIBDIR=%TK%\x86e_win64\obj"
set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"

call "%VCVARS%" >nul || (echo vcvars64.bat not found & exit /b 1)
cd /d "%~dp0"
if not exist bin mkdir bin
if not exist obj mkdir obj

cl -c -nologo -GS -fp:precise -D_WSTDIO_DEFINED -D_CRT_SECURE_NO_WARNINGS -DPRO_MACHINE=36 -DPRO_OS=4 ^
   -I"%TK%\includes" -I"%TK%\protk_appls\includes" ^
   -Fo:obj\ src\main.c src\common.c src\textutil.c src\http_server.c src\router.c src\structure.c ^
   src\model.c src\features.c src\assembly.c src\drawing.c src\mfg.c src\bom.c src\cabling.c src\dimensions.c || exit /b 1

link /nologo /dll /machine:amd64 /debug:none /out:%OUT_DLL% ^
   obj\main.obj obj\common.obj obj\textutil.obj obj\http_server.obj obj\router.obj obj\structure.obj ^
   obj\model.obj obj\features.obj obj\assembly.obj obj\drawing.obj obj\mfg.obj obj\bom.obj obj\cabling.obj obj\dimensions.obj ^
   "%LIBDIR%\protk_dll_NU.lib" "%LIBDIR%\ucore.lib" "%LIBDIR%\udata.lib" ^
   libcmt.lib kernel32.lib user32.lib wsock32.lib advapi32.lib mpr.lib ^
   winspool.lib netapi32.lib psapi.lib gdi32.lib shell32.lib comdlg32.lib ^
   ole32.lib ws2_32.lib || exit /b 1

echo.
echo Built %~dp0%OUT_DLL% against %CREO_HOME%  (now run unlock.bat)
