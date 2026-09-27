@echo off
REM One-click build. Share toolchain detection with the PowerShell entry point.
REM Usage: build-harpia.bat [Debug^|Release^|RelWithDebInfo^|MinSizeRel] [run]
setlocal
set "CONFIG=RelWithDebInfo"
set "RUNARG="
if /I "%~1"=="run" (set "RUNARG=-Run") else if not "%~1"=="" (set "CONFIG=%~1")
if /I "%~2"=="run" set "RUNARG=-Run"

REM Preserve the original build behavior: close Harpia so its DLLs can be replaced.
taskkill /F /IM harpia.exe >nul 2>&1

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Build-Harpia.ps1" -Configuration "%CONFIG%" -Reconfigure %RUNARG%
if errorlevel 1 goto :fail
endlocal
exit /b 0

:fail
echo.
echo ***************************************************************************
echo  BUILD FAILED - window kept open. Scroll up to copy the error above.
echo ***************************************************************************
if not defined HARPIA_NO_PAUSE pause
endlocal
exit /b 1
