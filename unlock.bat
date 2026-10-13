@echo off
rem Unlocks bin\creopdm_structure.dll so Creo will load it (needs a Toolkit license).
rem Run AFTER build.bat; rebuilding un-unlocks the DLL. Uses CREO_HOME (default Creo 10).
setlocal
if "%CREO_HOME%"=="" set "CREO_HOME=C:\PTC\Creo 10.0.10.0"
call "%CREO_HOME%\Parametric\bin\protk_unlock.bat" "%~dp0bin\creopdm_structure.dll"
