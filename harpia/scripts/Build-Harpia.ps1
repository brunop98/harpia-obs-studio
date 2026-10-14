<#
.SYNOPSIS
    One-command build for the Harpia recorder on Windows.

.DESCRIPTION
    Configures and builds the Harpia recorder with an installed Visual Studio C++ toolchain.
    The configure step automatically downloads the
    prebuilt dependencies (Qt6 and obs-deps) declared in CMakePresets.json, so no
    manual dependency setup is required.

    Prerequisites (install once):
      * Visual Studio 2022 or 2026 with the "Desktop development with C++" workload
      * CMake >= 3.28 (>= 4.2 for VS 2026) (winget install Kitware.CMake) — then reopen the terminal
      * Git

.PARAMETER Configuration
    Build configuration. Default: RelWithDebInfo.

.PARAMETER Target
    CMake target to build. Default: harpia-recorder. Pass an empty string or
    "all" to build the whole solution.

.PARAMETER Reconfigure
    Run CMake configure again even if build_x64/CMakeCache.txt already exists.

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
    [switch] $Reconfigure,
    # Empty = reuse a compatible cached generator, otherwise detect VS 2026/2022.
    [string] $Generator = '',
    [switch] $Run,
    # After building, assemble a self-contained distributable folder
    # (build_x64/dist/Harpia) with the exe, all runtime DLLs, the OBS plugins +
    # data, and the Microsoft Visual C++ runtime, so it runs on a clean machine.
    [switch] $Package
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
        Write-Host 'You also need Visual Studio 2022 or 2026 with the'
        Write-Host '"Desktop development with C++" workload.'
        Write-Host ''
        exit 1
    }
}

# Intersect installed C++ toolchains with generators supported by this CMake.
function Select-VisualStudioGenerator([string] $Requested, [string] $Cached) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) {
        throw 'Visual Studio Installer was not found. Install Desktop development with C++.'
    }
    # Assign the JSON array directly: Windows PowerShell 5.1 otherwise nests it.
    $installs = & $vswhere -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -format json | ConvertFrom-Json
    if ($LASTEXITCODE -ne 0) { throw 'Visual Studio detection failed.' }
    $capabilities = & cmake -E capabilities | ConvertFrom-Json
    if ($LASTEXITCODE -ne 0) { throw 'CMake generator detection failed.' }
    $supported = @($capabilities.generators | ForEach-Object { $_.name })
    $available = @(foreach ($candidate in @(
        @{ Major = 18; Name = 'Visual Studio 18 2026' },
        @{ Major = 17; Name = 'Visual Studio 17 2022' }
    )) {
        $matching = @($installs | Where-Object {
            $_.isComplete -and ([version]$_.installationVersion).Major -eq $candidate.Major
        })
        if ($matching.Count -gt 0 -and $supported -contains $candidate.Name) { $candidate.Name }
    })
    if ($Requested) {
        if ($available -notcontains $Requested) {
            throw "Requested generator '$Requested' requires a complete matching Visual Studio C++ installation and compatible CMake. This Windows project requires a Visual Studio generator."
        }
        return $Requested
    }
    if ($Cached -and $available -contains $Cached) { return $Cached }
    if ($available.Count -gt 0) { return $available[0] }
    throw 'No compatible VS 2022/2026 C++ installation found. Finish Visual Studio setup with Desktop development with C++ and a Windows SDK. VS 2026 requires CMake 4.2 or newer.'
}

Assert-Cmake

Push-Location $RepoRoot
try {
    $cachePath = Join-Path $BuildDir 'CMakeCache.txt'
    $cacheExists = Test-Path -LiteralPath $cachePath
    $cachedGenerator = ''
    if ($cacheExists) {
        $match = Select-String -LiteralPath $cachePath -Pattern '^CMAKE_GENERATOR:INTERNAL=(.*)$'
        if ($match) { $cachedGenerator = $match.Matches[0].Groups[1].Value }
    }
    $selectedGenerator = Select-VisualStudioGenerator $Generator $cachedGenerator
    Write-Host "==> Using $selectedGenerator (x64)" -ForegroundColor Cyan

    if ($cacheExists -and $cachedGenerator -ne $selectedGenerator) {
        # CMake cannot switch generators in place. Preserve all previous output.
        $resolvedBuild = [IO.Path]::GetFullPath($BuildDir)
        $expectedBuild = [IO.Path]::GetFullPath((Join-Path $RepoRoot 'build_x64'))
        if ($resolvedBuild -ne $expectedBuild -or
            ((Get-Item -LiteralPath $BuildDir).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            throw "Refusing to move unexpected build directory: $BuildDir"
        }
        $backup = "$BuildDir.before-generator-change-$(Get-Date -Format 'yyyyMMdd-HHmmss-fff')"
        Move-Item -LiteralPath $resolvedBuild -Destination $backup
        Write-Host "==> Preserved previous build folder: $backup"
        $cacheExists = $false
    }

    if ($Reconfigure -or -not $cacheExists) {
        Write-Host '==> Configuring (downloads Qt6/obs-deps on first run)...' -ForegroundColor Cyan
        # No feature flags needed: the SRT/RIST output and the browser source
        # these used to switch off are no longer in the tree.
        $cfgArgs = @('-S', $RepoRoot, '-B', $BuildDir)
        if (-not $cacheExists) {
            $cfgArgs += @('-G', $selectedGenerator, '-A', 'x64')
        }
        & cmake @cfgArgs
        if ($LASTEXITCODE -ne 0) { throw "CMake configure failed ($LASTEXITCODE)" }
    }
    else {
        Write-Host "==> Reusing existing configuration in $BuildDir (pass -Reconfigure to redo)." -ForegroundColor DarkGray
    }

    Write-Host "==> Building target '$Target' ($Configuration)..." -ForegroundColor Cyan
    # Build the tree directly (not via the build preset) so we don't depend on the
    # pinned-generator configure preset.
    $buildArgs = @('--build', $BuildDir, '--config', $Configuration, '--parallel')
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
    Write-Host '    (It finds its plugins and data from its own folder, however it is started.)'

    if ($Run) {
        if (-not (Test-Path -LiteralPath $exe)) { throw "Executable not found: $exe" }
        Start-Process -FilePath $exe -WorkingDirectory (Split-Path $exe)
    }

    if ($Package) {
        Write-Host ''
        Write-Host '==> Packaging a self-contained distributable...' -ForegroundColor Cyan

        $runDir = Join-Path $BuildDir "rundir/$Configuration"
        if (-not (Test-Path (Join-Path $runDir 'bin/64bit/harpia.exe'))) {
            throw "rundir not found or incomplete: $runDir"
        }

        $dist = Join-Path $BuildDir 'dist/Harpia'
        if (Test-Path $dist) { Remove-Item -Recurse -Force $dist }
        New-Item -ItemType Directory -Force -Path $dist | Out-Null

        # The rundir already has the correct OBS layout: bin/64bit (exe + libobs +
        # Qt/FFmpeg DLLs + the obs-ffmpeg-mux and encoder test helpers + platforms/
        # + the Visual C++ runtime), obs-plugins/64bit, and data/. Copy it wholesale.
        # The Visual C++ runtime is put there by the build itself, from Visual
        # Studio's VC\Redist folder at a version no older than the compiler
        # (harpia/cmake/os-windows.cmake) -- not copied from this PC's System32,
        # which holds whatever version some other program installed.
        Copy-Item -Recurse -Force -Path (Join-Path $runDir '*') -Destination $dist

        # Check it before calling it done: a missing piece here is a Harpia that
        # does not start, or does not record, on someone else's PC.
        $required = @(
            @{ Path = 'bin/64bit/harpia.exe'; Why = 'the program' },
            @{ Path = 'bin/64bit/obs-ffmpeg-mux.exe'; Why = 'the recording helper: no recording starts without it' },
            @{ Path = 'data/libobs/default.effect'; Why = "the video engine's data" },
            @{ Path = 'bin/64bit/vcruntime140.dll'; Why = 'the Visual C++ runtime (not found in Visual Studio; see the configure warning)' },
            @{ Path = 'bin/64bit/vcruntime140_1.dll'; Why = 'the Visual C++ runtime' },
            @{ Path = 'bin/64bit/msvcp140.dll'; Why = 'the Visual C++ runtime' }
        )
        $expected = @(
            @{ Path = 'bin/64bit/obs-nvenc-test.exe'; Why = 'NVIDIA hardware encoding' },
            @{ Path = 'bin/64bit/obs-amf-test.exe'; Why = 'AMD hardware encoding' },
            @{ Path = 'bin/64bit/obs-qsv-test.exe'; Why = 'Intel hardware encoding (it needs the ATL component to build)' }
        )
        foreach ($item in $expected) {
            if (-not (Test-Path -LiteralPath (Join-Path $dist $item.Path))) {
                Write-Host "    WARNING: $($item.Path) is missing: no $($item.Why) in this package." -ForegroundColor Yellow
            }
        }
        $missing = @($required | Where-Object { -not (Test-Path -LiteralPath (Join-Path $dist $_.Path)) })
        if ($missing.Count -gt 0) {
            foreach ($item in $missing) {
                Write-Host "    MISSING: $($item.Path) -- $($item.Why)" -ForegroundColor Red
            }
            throw "The package in $dist is incomplete (see above)."
        }

        Write-Host "==> Distributable ready: $dist" -ForegroundColor Green
        Write-Host "    Contents: bin/64bit (harpia.exe, its DLLs, the Visual C++ runtime, the helpers), obs-plugins/64bit, data/"
        Write-Host "    Zip the '$dist' folder to share it; run bin/64bit/harpia.exe on the other machine."
    }
}
finally {
    Pop-Location
}
