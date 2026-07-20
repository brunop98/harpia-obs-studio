@echo off
REM ===========================================================================
REM  run-harpia.bat - launch the last-built Harpia recorder (no rebuild).
REM
REM  Runs harpia.exe straight from its rundir so the OBS plugins beside it are
REM  found.  Does NOT build - use build-harpia.bat / build-and-run-harpia.bat
REM  for that.
REM
REM  Usage:
REM    run-harpia.bat          run RelWithDebInfo build
REM    run-harpia.bat Debug    run a different config
REM ===========================================================================
setlocal
set "CONFIG=%~1"
if "%CONFIG%"=="" set "CONFIG=RelWithDebInfo"

set "SCRIPT_DIR=%~dp0"
pushd "%SCRIPT_DIR%..\.." || ( echo ERROR: cannot cd to repo root & pause & exit /b 1 )
set "BIN=%CD%\build_x64\rundir\%CONFIG%\bin\64bit"
popd

if not exist "%BIN%\harpia.exe" (
  echo ERROR: harpia.exe not found for config "%CONFIG%":
  echo        %BIN%\harpia.exe
  echo Build it first ^(build-harpia.bat %CONFIG%^).
  pause
  exit /b 1
)

echo ==^> Launching harpia.exe ^(%CONFIG%^) ...
pushd "%BIN%"
start "" "harpia.exe"
popd
endlocal
