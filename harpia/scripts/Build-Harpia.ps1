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
    [switch] $Reconfigure,
    # CMake generator for a FRESH configure (e.g. "Visual Studio 17 2022").
    # Empty = let CMake auto-detect the installed Visual Studio. Ignored on
    # reconfigure (the existing cache's generator is reused).
    [string] $Generator = '',
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
        Write-Host 'You also need Visual Studio 2022 with the'
        Write-Host '"Desktop development with C++" workload.'
        Write-Host ''
        exit 1
    }
}

# Is there a Visual Studio C++ toolset for CMake to generate for?
#
# Without one, CMake picks "Visual Studio 17 2022" as its default generator and
# fails with "could not find any instance of Visual Studio" -- a message that
# names a product the user never asked for and does not say what to do about
# it, or that building is not required in the first place.
function Assert-VisualStudio {
    # vswhere ships with every VS installer at this fixed path. Guard the env
    # var: Join-Path on a null root throws, and ErrorActionPreference is Stop,
    # so a missing variable would abort with a worse message than the one this
    # function exists to replace.
    $pf86 = ${env:ProgramFiles(x86)}
    $vswhere = if ($pf86) {
        Join-Path $pf86 'Microsoft Visual Studio/Installer/vswhere.exe'
    } else { $null }
    $found = $false
    if ($vswhere -and (Test-Path $vswhere)) {
        # -products * so the standalone Build Tools count, not just the IDE.
        $installs = & $vswhere -latest -products * `
            -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
            -property installationPath 2>$null
        $found = [bool]$installs
    }
    if ($found) { return }

    Write-Host ''
    Write-Host 'ERROR: no Visual Studio C++ toolset was found on this machine.' -ForegroundColor Red
    Write-Host ''
    Write-Host 'CMake needs one to generate a build. Without it you get its own'
    Write-Host 'message instead, which is where "Visual Studio 17 2022" comes from:'
    Write-Host '    Generator ... could not find any instance of Visual Studio.' -ForegroundColor DarkGray
    Write-Host ''
    Write-Host 'If you only want to RUN Harpia on this PC, you do not need any of' -ForegroundColor Yellow
    Write-Host 'this -- do not build here. On a PC that can build, run:' -ForegroundColor Yellow
    Write-Host '    .\harpia\scripts\Build-Harpia.ps1 -Package'
    Write-Host 'then copy the whole build_x64\dist\Harpia folder across and run'
    Write-Host 'bin\64bit\harpia.exe from it. That folder is self-contained.'
    Write-Host ''
    Write-Host 'If you DO want to build here, install one of these (either works):'
    Write-Host '    winget install Microsoft.VisualStudio.2022.BuildTools --override "--quiet --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"'
    Write-Host '    winget install Microsoft.VisualStudio.2022.Community  (tick "Desktop development with C++")'
    Write-Host 'Then open a NEW terminal and run this script again.'
    Write-Host ''
    exit 1
}

Assert-Cmake

Push-Location $RepoRoot
try {
    $cacheExists = Test-Path (Join-Path $BuildDir 'CMakeCache.txt')

    if ($Reconfigure -or -not $cacheExists) {
        Write-Host '==> Configuring (downloads Qt6/obs-deps on first run)...' -ForegroundColor Cyan
        # NOTE: we deliberately do NOT use `--preset windows-x64` for configure —
        # that preset pins a specific Visual Studio generator + Windows SDK version
        # that may not be installed. Instead we configure build_x64 directly so the
        # generator matches whatever VS you have.
        #   - The tree is slimmed (no OBS UI/scripting), and obs-ffmpeg drops its
        #     SRT/RIST (mpegts) path; ENABLE_BROWSER=OFF skips the CEF download.
        $cfgArgs = @('-S', '.', '-B', $BuildDir,
                     '-DENABLE_NEW_MPEGTS_OUTPUT=OFF', '-DENABLE_BROWSER=OFF')
        if (-not $cacheExists) {
            # Fresh tree: target x64 and let CMake auto-detect the installed Visual
            # Studio (override with -Generator "Visual Studio 17 2022" if needed).
            #
            # Checked here rather than at the top of the script: a -Reconfigure of
            # an existing cache reuses its generator, and an explicitly requested
            # non-VS generator (Ninja) does not need the IDE's toolset located
            # this way. Only a fresh VS-generator configure does.
            if (-not $Generator -or $Generator -like 'Visual Studio*') {
                Assert-VisualStudio
            }
            $cfgArgs += @('-A', 'x64')
            if ($Generator) { $cfgArgs += @('-G', $Generator) }
        }
        # else (reconfigure): omit -G/-A so CMake reuses the existing cache's
        # generator — this is what makes -Reconfigure work regardless of the preset.
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
    Write-Host '    (Run from that folder so the OBS plugins next to it are found.)'

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
        # Qt/FFmpeg DLLs + the obs-ffmpeg-mux helper + platforms/), obs-plugins/64bit,
        # and data/. Copy it wholesale.
        Copy-Item -Recurse -Force -Path (Join-Path $runDir '*') -Destination $dist

        # Bundle the Microsoft Visual C++ runtime app-locally (next to the exe) so
        # no separate redistributable install is required on the target machine.
        $binDir = Join-Path $dist 'bin/64bit'
        $vcDlls = @('msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll', 'concrt140.dll')
        $vcCopied = 0
        foreach ($dll in $vcDlls) {
            $src = Join-Path $env:SystemRoot "System32/$dll"
            if (Test-Path $src) {
                Copy-Item -Force -Path $src -Destination $binDir
                $vcCopied++
            }
        }

        Write-Host "==> Distributable ready: $dist" -ForegroundColor Green
        Write-Host "    Contents: bin/64bit (harpia.exe + all DLLs + obs-ffmpeg-mux), obs-plugins/64bit, data/"
        if ($vcCopied -gt 0) {
            Write-Host "    Bundled the Visual C++ runtime ($vcCopied DLLs) next to the exe."
        }
        else {
            Write-Host "    NOTE: VC++ runtime DLLs were not found in System32 — install the" -ForegroundColor Yellow
            Write-Host "    'Microsoft Visual C++ 2015-2022 Redistributable (x64)' on the target machine." -ForegroundColor Yellow
        }
        Write-Host "    Zip the '$dist' folder to share it; run bin/64bit/harpia.exe on the other machine."
    }
}
finally {
    Pop-Location
}
