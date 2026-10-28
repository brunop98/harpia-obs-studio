# Run with PowerShell 5.1 or newer; no Windows SDK or real binaries are needed.
# The fixtures exercise package completeness, not Windows DLL loading.
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$scripts = Join-Path $PSScriptRoot '../scripts'
$validator = Join-Path $scripts 'Test-HarpiaPackage.ps1'
$temporary = Join-Path ([IO.Path]::GetTempPath()) ('harpia-package-' + [Guid]::NewGuid().ToString('N'))
$fixture = Join-Path $temporary "complete package [x64]'s files"
$failures = 0

function Check([bool] $Condition, [string] $Description) {
    if ($Condition) { Write-Host "PASS $Description" }
    else {
        Write-Host "FAIL $Description"
        $script:failures++
    }
}
function Rejects([scriptblock] $Action, [string] $Expected) {
    try { & $Action; return $false }
    catch { return $_.Exception.Message.Contains($Expected) }
}
function Write-FixtureFile([string] $Relative, [string] $Contents = 'fixture') {
    $path = Join-Path $fixture $Relative
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $path) | Out-Null
    [IO.File]::WriteAllText($path, $Contents)
}
function Snapshot {
    $hashes = @(Get-ChildItem -LiteralPath $fixture -Recurse -File | Sort-Object FullName | ForEach-Object {
        $_.FullName + ':' + (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash
    })
    return $hashes -join "`n"
}

try {
    foreach ($script in @($validator, (Join-Path $scripts 'Build-Harpia.ps1'))) {
        $tokens = $null
        $errors = $null
        [void][System.Management.Automation.Language.Parser]::ParseFile($script, [ref]$tokens, [ref]$errors)
        Check ($errors.Count -eq 0) "PowerShell parses $([IO.Path]::GetFileName($script))"
    }
    foreach ($relative in @(
        'bin/64bit/harpia.exe', 'bin/64bit/obs.dll', 'bin/64bit/libobs-d3d11.dll',
        'bin/64bit/obs-ffmpeg-mux.exe', 'data/libobs/default.effect',
        'bin/64bit/Qt6Core.dll', 'bin/64bit/Qt6Gui.dll', 'bin/64bit/Qt6Widgets.dll',
        'bin/64bit/platforms/qwindows.dll', 'bin/64bit/vcruntime140.dll',
        'bin/64bit/vcruntime140_1.dll', 'bin/64bit/msvcp140.dll'
    )) { Write-FixtureFile $relative }

    $before = Snapshot
    foreach ($configuration in @('Release', 'RelWithDebInfo', 'MinSizeRel')) {
        & $validator -PackageRoot $fixture -Configuration $configuration
        Check $true "a complete $configuration package passes validation"
    }
    Check ((Snapshot) -eq $before) 'validation does not modify complete package files'

    foreach ($relative in @(
        'bin/64bit/libobs-d3d11.dll', 'bin/64bit/obs.dll', 'bin/64bit/obs-ffmpeg-mux.exe',
        'data/libobs/default.effect', 'bin/64bit/platforms/qwindows.dll', 'bin/64bit/vcruntime140_1.dll'
    )) {
        $path = Join-Path $fixture $relative
        [IO.File]::Delete($path)
        $before = Snapshot
        Check (Rejects { & $validator -PackageRoot $fixture } $relative) "missing $relative prevents distribution and names the missing file"
        Check ((Snapshot) -eq $before) "rejection leaves the incomplete source untouched ($relative)"
        Write-FixtureFile $relative
    }

    Write-FixtureFile 'bin/64bit/libobs-d3d11.dll' ''
    Check (Rejects { & $validator -PackageRoot $fixture } 'empty file') 'an empty renderer DLL is refused'
    [IO.File]::Delete((Join-Path $fixture 'bin/64bit/libobs-d3d11.dll'))
    New-Item -ItemType Directory -Path (Join-Path $fixture 'bin/64bit/libobs-d3d11.dll') | Out-Null
    Check (Rejects { & $validator -PackageRoot $fixture } 'libobs-d3d11.dll') 'a directory named like the renderer DLL does not satisfy the package check'
    Remove-Item -LiteralPath (Join-Path $fixture 'bin/64bit/libobs-d3d11.dll')
    Write-FixtureFile 'bin/64bit/libobs-d3d11.dll'

    Check (Rejects { & $validator -PackageRoot $fixture -Configuration Debug } 'non-redistributable') 'Debug packages explain why the release runtime cannot make them portable'
    Check (Rejects { & (Join-Path $scripts 'Build-Harpia.ps1') -Configuration Debug -Package } 'non-redistributable') 'the package build refuses Debug before tool detection or compilation'
    Check (Rejects { & $validator -PackageRoot (Join-Path $temporary 'missing') } 'does not exist') 'a nonexistent package root produces a clear error'
    & $validator -PackageRoot $fixture
    Check $true 'restoring missing files makes the package valid again'
}
finally {
    if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary -Recurse -Force }
}

if ($failures) { throw "$failures Windows package checks failed." }
Write-Host 'ALL PASSED'
