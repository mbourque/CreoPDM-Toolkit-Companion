@echo off
rem Builds and runs the non-ASCII text test (no Creo or Toolkit SDK needed; only Visual Studio's C compiler).
setlocal
set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
call "%VCVARS%" >nul || (echo vcvars64.bat not found & exit /b 1)
cd /d "%~dp0"
if not exist out mkdir out
cl /nologo /utf-8 /D_CRT_SECURE_NO_WARNINGS utf8_test.c ..\src\textutil.c /Fo:out\ /Fe:out\utf8_test.exe || exit /b 1
out\utf8_test.exe
exit /b %errorlevel%
