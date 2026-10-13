@echo off
rem Unlocks bin\creopdm_structure.dll so Creo will load it (needs a Toolkit license).
rem Run AFTER build.bat; rebuilding un-unlocks the DLL.
call "C:\PTC\Creo 13.4.1.0\Parametric\bin\protk_unlock.bat" "%~dp0bin\creopdm_structure.dll"
