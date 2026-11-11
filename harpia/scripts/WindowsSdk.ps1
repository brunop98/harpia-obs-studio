# Shared Windows SDK discovery and configure planning. Compatible with Windows
# PowerShell 5.1; sourcing this file does not configure or build anything.

function Get-HarpiaWindowsSdkRoots {
    $roots = @()
    foreach ($name in @('CMAKE_WINDOWS_KITS_10_DIR', 'WindowsSdkDir')) {
        $value = [Environment]::GetEnvironmentVariable($name)
        if ($value) { $roots += $value }
    }
    foreach ($key in @(
        'HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots',
        'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows Kits\Installed Roots'
    )) {
        $entry = Get-ItemProperty -LiteralPath $key -ErrorAction SilentlyContinue
        if ($entry -and $entry.PSObject.Properties['KitsRoot10'] -and $entry.KitsRoot10) {
            $roots += $entry.KitsRoot10
        }
    }
    foreach ($programFiles in @(${env:ProgramFiles(x86)}, $env:ProgramFiles)) {
        if ($programFiles) { $roots += Join-Path $programFiles 'Windows Kits/10' }
    }
    return @($roots | Select-Object -Unique)
}

function Find-HarpiaWindowsSdk {
    param(
        [string[]] $Roots = (Get-HarpiaWindowsSdkRoots),
        # Keep in step with cmake/windows/compilerconfig.cmake. Prefer the
        # newest installed SDK, including versions newer than the OBS preset.
        [version] $MinimumVersion = '10.0.20348.0'
    )
    $hostTools = 'x64'
    if ($env:PROCESSOR_ARCHITECTURE -eq 'ARM64' -or $env:PROCESSOR_ARCHITEW6432 -eq 'ARM64') {
        $hostTools = 'arm64'
    }
    $candidates = @(foreach ($root in @($Roots | Select-Object -Unique)) {
        if (-not $root -or -not (Test-Path -LiteralPath $root -PathType Container)) { continue }
        $includeRoot = Join-Path $root 'Include'
        foreach ($directory in @(Get-ChildItem -LiteralPath $includeRoot -Directory -ErrorAction SilentlyContinue)) {
            if ($directory.Name -notmatch '^10\.0\.\d+\.\d+$') { continue }
            $version = [version]$directory.Name
            if ($version -lt $MinimumVersion) { continue }
            $number = $directory.Name
            $required = @(
                "Include/$number/um/Windows.h",
                "Include/$number/shared/sdkddkver.h",
                "Include/$number/ucrt/stdio.h",
                "Lib/$number/um/x64/kernel32.lib",
                "Lib/$number/um/x64/user32.lib",
                "Lib/$number/ucrt/x64/ucrt.lib",
                "bin/$number/$hostTools/rc.exe",
                "bin/$number/$hostTools/mt.exe"
            )
            $complete = $true
            foreach ($relative in $required) {
                if (-not (Test-Path -LiteralPath (Join-Path $root $relative) -PathType Leaf)) {
                    $complete = $false
                    break
                }
            }
            if ($complete) {
                [pscustomobject]@{
                    Version = $version
                    Root = [IO.Path]::GetFullPath($root)
                    ResourceCompiler = Join-Path $root "bin/$number/$hostTools/rc.exe"
                    ManifestTool = Join-Path $root "bin/$number/$hostTools/mt.exe"
                }
            }
        }
    })
    $found = $candidates | Sort-Object -Property Version -Descending | Select-Object -First 1
    if (-not $found) {
        throw @"
No complete Windows SDK $MinimumVersion or newer with x64 libraries and build tools was found.
Install the latest Windows SDK:
    https://developer.microsoft.com/windows/downloads/windows-sdk/
Or open Visual Studio Installer > Modify > Individual components and select a Windows 11 SDK.
Then run build-and-run-harpia.bat again.
"@
    }
    return $found
}

function Read-HarpiaBuildCache([string] $Path) {
    $cache = @{}
    if (Test-Path -LiteralPath $Path -PathType Leaf) {
        foreach ($line in Get-Content -LiteralPath $Path) {
            if ($line -match '^(CMAKE_GENERATOR|CMAKE_GENERATOR_PLATFORM|CMAKE_RC_COMPILER):[^=]*=(.*)$') {
                $cache[$Matches[1]] = $Matches[2]
            }
        }
    }
    return $cache
}

function Get-HarpiaWindowsConfigurePlan {
    param(
        [string] $SourceRoot,
        [string] $BuildDir,
        [string] $Generator,
        [object] $Sdk,
        [hashtable] $Cache = @{},
        [bool] $CacheExists = $false,
        [switch] $Reconfigure
    )
    $platform = "x64,version=$($Sdk.Version)"
    $cachedPlatform = [string]$Cache['CMAKE_GENERATOR_PLATFORM']
    $cachedRc = [string]$Cache['CMAKE_RC_COMPILER']
    $fresh = $CacheExists -and (
        $cachedPlatform -ne $platform -or
        ($cachedRc -and -not (Test-Path -LiteralPath $cachedRc -PathType Leaf))
    )
    # --fresh clears only CMake metadata, retaining the built application and
    # other output. A failed configure's cache is not a generated build tree.
    $configure = $Reconfigure -or -not $CacheExists -or $fresh -or
        -not (Test-Path -LiteralPath (Join-Path $BuildDir 'CMakeFiles/generate.stamp') -PathType Leaf)
    $arguments = @('-S', $SourceRoot, '-B', $BuildDir)
    if ($fresh) { $arguments += '--fresh' }
    if ($fresh -or -not $CacheExists) {
        $arguments += @('-G', $Generator, '-A', $platform)
    }
    $arguments += @(
        "-DCMAKE_RC_COMPILER:FILEPATH=$($Sdk.ResourceCompiler)",
        "-DCMAKE_MT:FILEPATH=$($Sdk.ManifestTool)"
    )
    return [pscustomobject]@{ Configure = [bool]$configure; Fresh = [bool]$fresh; Arguments = $arguments }
}
