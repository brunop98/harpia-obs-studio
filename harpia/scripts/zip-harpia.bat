@echo off
REM ===========================================================================
REM  zip-harpia.bat - zip the current Harpia build and open its folder.
REM
REM  Builds a small Harpia.exe launcher at the distribution root (so the zip
REM  root has a clickable EXE), then compresses the whole rundir for a config
REM  (Harpia.exe + bin\64bit + obs-plugins + data) into
REM  build_x64\dist\harpia-<config>-<timestamp>.zip and opens Explorer with the
REM  new zip selected.
REM
REM  The root Harpia.exe launcher is required because libobs resolves its core
REM  data as "..\..\data\libobs" relative to the working directory, so the real
REM  harpia.exe must run from bin\64bit. The launcher does exactly that.
REM
REM  Usage:
REM    zip-harpia.bat          zip RelWithDebInfo build
REM    zip-harpia.bat Debug    zip a different config
REM ===========================================================================
setlocal
set "CONFIG=%~1"
if "%CONFIG%"=="" set "CONFIG=RelWithDebInfo"

set "SCRIPT_DIR=%~dp0"
pushd "%SCRIPT_DIR%..\.." || ( echo ERROR: cannot cd to repo root & pause & exit /b 1 )
set "REPO=%CD%"
popd

set "SRC=%REPO%\build_x64\rundir\%CONFIG%"
set "DIST=%REPO%\build_x64\dist"
set "LAUNCHER_SRC=%SCRIPT_DIR%launcher.cs"

if not exist "%SRC%\bin\64bit\harpia.exe" (
  echo ERROR: no build found for config "%CONFIG%":
  echo        %SRC%\bin\64bit\harpia.exe
  echo Build it first ^(build-harpia.bat %CONFIG%^).
  pause
  exit /b 1
)

REM --- Build the root launcher Harpia.exe with the .NET C# compiler ---------
set "CSC=%WINDIR%\Microsoft.NET\Framework64\v4.0.30319\csc.exe"
if not exist "%CSC%" (
  echo WARNING: csc.exe not found - zipping without a root launcher EXE.
) else (
  echo ==^> Building root launcher Harpia.exe ...
  "%CSC%" /nologo /target:winexe /out:"%SRC%\Harpia.exe" ^
    /reference:System.Windows.Forms.dll "%LAUNCHER_SRC%"
  if errorlevel 1 ( echo ERROR: launcher compile failed. & pause & exit /b 1 )
)

if not exist "%DIST%" mkdir "%DIST%"

echo ==^> Zipping %CONFIG% build ...
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$ts = Get-Date -Format 'yyyyMMdd-HHmmss';" ^
  "$zip = Join-Path '%DIST%' ('harpia-%CONFIG%-' + $ts + '.zip');" ^
  "Compress-Archive -Path (Join-Path '%SRC%' '*') -DestinationPath $zip -Force;" ^
  "Write-Host ('    Created: ' + $zip);" ^
  "Start-Process explorer.exe -ArgumentList ('/select,\"' + $zip + '\"')"
if errorlevel 1 ( echo ERROR: zip failed. & pause & exit /b 1 )

echo ==^> Done.  Zip root contains Harpia.exe ^(launcher^) + bin\64bit\harpia.exe.
endlocal
