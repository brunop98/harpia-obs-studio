@echo off
REM ===========================================================================
REM  build-harpia.bat - one-click build for the Harpia recorder (Windows).
REM
REM  Why not just `cmake --preset windows-x64`?
REM    The preset pins generator "Visual Studio 18 2026". This machine has
REM    Build Tools 2022 (VS17), so the preset errors with a generator mismatch.
REM    This script configures with an explicit -G "Visual Studio 17 2022" the
REM    first time, then regenerates the existing tree in place on later runs.
REM
REM  Usage:
REM    build-harpia.bat          build harpia-recorder (RelWithDebInfo)
REM    build-harpia.bat run      build, then launch harpia.exe
REM    build-harpia.bat Debug    build a different config (Debug/Release/...)
REM
REM  Note: win-dshow (webcam) and obs-qsv11 (Intel QSV) need the Visual Studio
REM  ATL component. Without it they are skipped; everything else builds.
REM ===========================================================================
setlocal

REM --- Config (arg 1 may be a config name or "run") -------------------------
set "CONFIG=RelWithDebInfo"
set "DORUN=0"
if /I "%~1"=="run" ( set "DORUN=1" ) else if not "%~1"=="" ( set "CONFIG=%~1" )
if /I "%~2"=="run" set "DORUN=1"

REM --- Repo root = two levels up from this script (harpia\scripts) ----------
set "SCRIPT_DIR=%~dp0"
pushd "%SCRIPT_DIR%..\.." || ( echo ERROR: cannot cd to repo root & pause & exit /b 1 )
set "REPO=%CD%"
set "BUILD=%REPO%\build_x64"

REM --- cmake present? ------------------------------------------------------
where cmake >nul 2>&1
if errorlevel 1 (
  echo ERROR: cmake not on PATH.  Install it ^(winget install Kitware.CMake^)
  echo        and open a new terminal so PATH refreshes.
  goto :fail
)

REM --- Free the rundir: a running harpia.exe locks the DLL copy step --------
taskkill /F /IM harpia.exe >nul 2>&1

REM --- Configure (first run) or regenerate (later runs) --------------------
if exist "%BUILD%\CMakeCache.txt" (
  echo ==^> Regenerating existing build tree ...
  cmake "%BUILD%"
) else (
  echo ==^> Configuring fresh ^(downloads Qt6/obs-deps on first run^) ...
  cmake -S "%REPO%" -B "%BUILD%" -G "Visual Studio 17 2022" -A x64 ^
    -DENABLE_NEW_MPEGTS_OUTPUT=OFF -DENABLE_BROWSER=OFF
)
if errorlevel 1 ( echo ERROR: CMake configure failed. & goto :fail )

REM --- Build --------------------------------------------------------------
echo ==^> Building harpia-recorder ^(%CONFIG%^) ...
cmake --build "%BUILD%" --target harpia-recorder --config %CONFIG% --parallel
if errorlevel 1 ( echo ERROR: build failed. & goto :fail )

set "BIN=%BUILD%\rundir\%CONFIG%\bin\64bit"
echo.
echo ==^> Build succeeded.
echo     Exe:  %BIN%\harpia.exe

if "%DORUN%"=="1" (
  echo ==^> Launching harpia.exe ...
  pushd "%BIN%"
  start "" "harpia.exe"
  popd
)

popd
endlocal
exit /b 0

:fail
echo.
echo ***************************************************************************
echo  BUILD FAILED - window kept open.  Scroll up to copy the error above.
echo ***************************************************************************
popd
endlocal
pause
exit /b 1
