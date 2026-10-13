@echo off
rem Builds bin\creopdm_structure.dll against the Creo 13.4.1.0 Toolkit (64-bit).
setlocal
set "TK=C:\PTC\Creo 13.4.1.0\Common Files\protoolkit"
set "LIBDIR=%TK%\x86e_win64\obj"
set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"

call "%VCVARS%" >nul || (echo vcvars64.bat not found & exit /b 1)
cd /d "%~dp0"
if not exist bin mkdir bin
if not exist obj mkdir obj

cl -c -nologo -GS -fp:precise -D_WSTDIO_DEFINED -D_CRT_SECURE_NO_WARNINGS -DPRO_MACHINE=36 -DPRO_OS=4 ^
   -I"%TK%\includes" -I"%TK%\protk_appls\includes" ^
   -Fo:obj\ src\main.c src\common.c src\http_server.c src\router.c src\structure.c ^
   src\model.c src\features.c src\assembly.c src\drawing.c src\mfg.c src\bom.c src\cabling.c || exit /b 1

link /nologo /dll /machine:amd64 /debug:none /out:bin\creopdm_structure.dll ^
   obj\main.obj obj\common.obj obj\http_server.obj obj\router.obj obj\structure.obj ^
   obj\model.obj obj\features.obj obj\assembly.obj obj\drawing.obj obj\mfg.obj obj\bom.obj obj\cabling.obj ^
   "%LIBDIR%\protk_dll_NU.lib" "%LIBDIR%\ucore.lib" "%LIBDIR%\udata.lib" ^
   libcmt.lib kernel32.lib user32.lib wsock32.lib advapi32.lib mpr.lib ^
   winspool.lib netapi32.lib psapi.lib gdi32.lib shell32.lib comdlg32.lib ^
   ole32.lib ws2_32.lib || exit /b 1

echo.
echo Built %~dp0bin\creopdm_structure.dll  (now run unlock.bat)
