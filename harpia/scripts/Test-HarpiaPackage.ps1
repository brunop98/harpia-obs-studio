<#
.SYNOPSIS
    Check the Windows runtime files before sharing a Harpia folder or ZIP.
.DESCRIPTION
    Read-only: checks the same layout used by Build-Harpia.ps1 -Package and
    zip-harpia.bat. This detects missing files, not whether Windows can load
    every DLL on the destination PC; startup reports loader failures separately.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string] $PackageRoot,
    [ValidateSet('Debug', 'RelWithDebInfo', 'Release', 'MinSizeRel')]
    [string] $Configuration = 'RelWithDebInfo'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

if ($Configuration -eq 'Debug') {
    throw 'Debug builds require the non-redistributable Visual Studio debug runtime. Share a Release or RelWithDebInfo build instead.'
}
if (-not (Test-Path -LiteralPath $PackageRoot -PathType Container)) {
    throw "Harpia package folder does not exist: $PackageRoot"
}

$required = @(
    @{ Path = 'bin/64bit/harpia.exe'; Why = 'the program' },
    @{ Path = 'bin/64bit/obs.dll'; Why = 'the OBS engine' },
    @{ Path = 'bin/64bit/libobs-d3d11.dll'; Why = 'the Direct3D graphics module: recording cannot start without it' },
    @{ Path = 'bin/64bit/obs-ffmpeg-mux.exe'; Why = 'the recording helper' },
    @{ Path = 'data/libobs/default.effect'; Why = "the video engine's data" },
    @{ Path = 'bin/64bit/Qt6Core.dll'; Why = 'the Qt runtime' },
    @{ Path = 'bin/64bit/Qt6Gui.dll'; Why = 'the Qt graphics runtime' },
    @{ Path = 'bin/64bit/Qt6Widgets.dll'; Why = 'the Qt window runtime' },
    @{ Path = 'bin/64bit/platforms/qwindows.dll'; Why = 'the Qt Windows platform plugin' },
    @{ Path = 'bin/64bit/vcruntime140.dll'; Why = 'the Visual C++ runtime' },
    @{ Path = 'bin/64bit/vcruntime140_1.dll'; Why = 'the Visual C++ runtime' },
    @{ Path = 'bin/64bit/msvcp140.dll'; Why = 'the Visual C++ runtime' }
)
$missing = @(foreach ($item in $required) {
    $path = Join-Path $PackageRoot $item.Path
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        "$($item.Path) -- $($item.Why)"
    }
    elseif ((Get-Item -LiteralPath $path).Length -eq 0) {
        "$($item.Path) -- empty file ($($item.Why))"
    }
})
if ($missing.Count -gt 0) {
    throw ("The Harpia package is incomplete:`n  " + ($missing -join "`n  ") +
        "`nRebuild harpia-recorder, then package the complete runtime folder again.")
}

Write-Host "    Runtime file check passed: $PackageRoot" -ForegroundColor DarkGreen
