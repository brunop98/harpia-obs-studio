<#
.SYNOPSIS
    One-command build for the Harpia recorder on Windows.

.DESCRIPTION
    Configures and builds just the `harpia-recorder` target using the OBS
    `windows-x64` CMake preset. The configure step automatically downloads the
    prebuilt dependencies (Qt6, CEF, obs-deps) declared in buildspec.json, so no
    manual dependency setup is required.

    Prerequisites (install once):
      * Visual Studio 2022 with the "Desktop development with C++" workload
      * CMake >= 3.28  (winget install Kitware.CMake) — then reopen the terminal
      * Git

.PARAMETER Configuration
    Build configuration. Default: RelWithDebInfo.

.PARAMETER Target
    CMake target to build. Default: harpia-recorder. Pass an empty string or
    "all" to build the whole solution (stock OBS + Harpia).

.PARAMETER Reconfigure
    Force a fresh CMake configure even if build_x64/CMakeCache.txt already exists.

.EXAMPLE
    .\harpia\scripts\Build-Harpia.ps1
    Configures (fetching deps on first run) and builds harpia-recorder.

.EXAMPLE
    .\harpia\scripts\Build-Harpia.ps1 -Configuration Release -Reconfigure
#>
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'RelWithDebInfo', 'Release', 'MinSizeRel')]
    [string] $Configuration = 'RelWithDebInfo',
    [string] $Target = 'harpia-recorder',
    [switch] $Reconfigure
)

$ErrorActionPreference = 'Stop'

# Repo root is two levels up from this script (harpia/scripts -> repo root).
$RepoRoot = Resolve-Path -Path "$PSScriptRoot/../.."
$BuildDir = Join-Path $RepoRoot 'build_x64'

function Assert-Cmake {
    if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
        Write-Host ''
        Write-Host 'ERROR: cmake was not found on your PATH.' -ForegroundColor Red
        Write-Host ''
        Write-Host 'Install it, then open a NEW terminal so PATH refreshes:'
        Write-Host '    winget install Kitware.CMake'
        Write-Host ''
        Write-Host 'You also need Visual Studio 2022 with the'
        Write-Host '"Desktop development with C++" workload.'
        Write-Host ''
        exit 1
    }
}

Assert-Cmake

Push-Location $RepoRoot
try {
    $cacheExists = Test-Path (Join-Path $BuildDir 'CMakeCache.txt')

    if ($Reconfigure -or -not $cacheExists) {
        Write-Host '==> Configuring (downloads Qt6/CEF/obs-deps on first run)...' -ForegroundColor Cyan
        & cmake --preset windows-x64
        if ($LASTEXITCODE -ne 0) { throw "CMake configure failed ($LASTEXITCODE)" }
    }
    else {
        Write-Host "==> Reusing existing configuration in $BuildDir (pass -Reconfigure to redo)." -ForegroundColor DarkGray
    }

    Write-Host "==> Building target '$Target' ($Configuration)..." -ForegroundColor Cyan
    $buildArgs = @('--build', '--preset', 'windows-x64', '--config', $Configuration, '--parallel')
    if ($Target -and $Target -ne 'all') {
        $buildArgs += @('--target', $Target)
    }
    & cmake @buildArgs
    if ($LASTEXITCODE -ne 0) { throw "CMake build failed ($LASTEXITCODE)" }

    $exe = Join-Path $BuildDir "rundir/$Configuration/bin/64bit/harpia.exe"
    Write-Host ''
    Write-Host '==> Build succeeded.' -ForegroundColor Green
    if (Test-Path $exe) {
        Write-Host "    Run it: $exe"
    }
    else {
        Write-Host "    Look for harpia.exe under: $(Join-Path $BuildDir "rundir/$Configuration/bin/64bit")"
    }
    Write-Host '    (Run from that folder so the OBS plugins next to it are found.)'
}
finally {
    Pop-Location
}
