@echo off
rem Unlocks bin\creopdm_structure.dll so Creo will load it (needs a Toolkit license).
rem Run AFTER build.bat; rebuilding un-unlocks the DLL. Uses CREO_HOME (default Creo 10).
rem If build.bat was run with OUT_DLL (a staged copy), set the same OUT_DLL first.
setlocal
if "%CREO_HOME%"=="" set "CREO_HOME=C:\PTC\Creo 10.0.10.0"
if "%OUT_DLL%"=="" set "OUT_DLL=bin\creopdm_structure.dll"
call "%CREO_HOME%\Parametric\bin\protk_unlock.bat" "%~dp0%OUT_DLL%"
