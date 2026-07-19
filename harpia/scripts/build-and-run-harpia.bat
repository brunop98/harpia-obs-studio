@echo off
REM ===========================================================================
REM  build-and-run-harpia.bat - build the Harpia recorder, then launch it.
REM
REM  Thin convenience wrapper over build-harpia.bat: builds harpia-recorder and
REM  immediately runs harpia.exe from its rundir so the OBS plugins beside it
REM  are found.
REM
REM  Usage:
REM    build-and-run-harpia.bat          build+run (RelWithDebInfo)
REM    build-and-run-harpia.bat Debug    build+run a different config
REM ===========================================================================
setlocal
set "CONFIG=%~1"
call "%~dp0build-harpia.bat" %CONFIG% run
exit /b %errorlevel%
