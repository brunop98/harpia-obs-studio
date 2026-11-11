# SDK discovery/cache repair fixtures; no Visual Studio or real SDK is needed.
# Run with Windows PowerShell 5.1 or newer.
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$scripts = Join-Path $PSScriptRoot '../scripts'
. (Join-Path $scripts 'WindowsSdk.ps1')
$temporary = Join-Path ([IO.Path]::GetTempPath()) ('harpia-sdk-' + [Guid]::NewGuid().ToString('N'))
$failures = 0

function Check([bool] $Condition, [string] $Message) {
    if ($Condition) { Write-Host "PASS $Message" }
    else { Write-Host "FAIL $Message"; $script:failures++ }
}
function Rejects([scriptblock] $Action, [string] $Expected) {
    try { & $Action | Out-Null; return $false }
    catch { return $_.Exception.Message.Contains($Expected) }
}
function Write-FixtureFile([string] $Path) {
    [IO.Directory]::CreateDirectory((Split-Path -Parent $Path)) | Out-Null
    [IO.File]::WriteAllText($Path, 'SDK fixture')
}
function Write-Sdk([string] $Root, [string] $Version, [string] $Architecture = 'x64') {
    foreach ($relative in @(
        "Include/$Version/um/Windows.h", "Include/$Version/shared/sdkddkver.h",
        "Include/$Version/ucrt/stdio.h", "Lib/$Version/um/$Architecture/kernel32.lib",
        "Lib/$Version/um/$Architecture/user32.lib", "Lib/$Version/ucrt/$Architecture/ucrt.lib",
        "bin/$Version/x64/rc.exe", "bin/$Version/x64/mt.exe",
        "bin/$Version/arm64/rc.exe", "bin/$Version/arm64/mt.exe"
    )) { Write-FixtureFile (Join-Path $Root $relative) }
}

try {
    foreach ($file in @('WindowsSdk.ps1', 'Build-Harpia.ps1')) {
        $tokens = $null
        $errors = $null
        [void][System.Management.Automation.Language.Parser]::ParseFile((Join-Path $scripts $file), [ref]$tokens, [ref]$errors)
        Check ($errors.Count -eq 0) "PowerShell parses $file"
    }
    $root = Join-Path $temporary "SDKs [x64]'s installed kits"
    Write-Sdk $root '10.0.19041.0'
    Write-Sdk $root '10.0.26100.0'
    Write-Sdk $root '10.0.30000.0'
    $found = Find-HarpiaWindowsSdk -Roots @($root)
    Check ($found.Version -eq [version]'10.0.30000.0' -and $found.Root -eq $root) 'a newer installed SDK is preferred over the preset version'
    Check ((Test-Path -LiteralPath $found.ResourceCompiler) -and (Test-Path -LiteralPath $found.ManifestTool)) 'the selected SDK includes resource and manifest tools'
    Remove-Item -LiteralPath (Join-Path $root 'Lib/10.0.30000.0/ucrt/x64/ucrt.lib')
    $found = Find-HarpiaWindowsSdk -Roots @($root)
    Check ($found.Version -eq [version]'10.0.26100.0') 'a partial newer installation does not hide a complete compatible SDK'
    $other = Join-Path $temporary 'SDK on another drive'
    Write-Sdk $other '10.0.28000.0'
    $found = Find-HarpiaWindowsSdk -Roots @($root, $other)
    Check ($found.Version -eq [version]'10.0.28000.0' -and $found.Root -eq $other) 'SDK discovery checks multiple installation roots, including custom folders'
    $oldOnly = Join-Path $temporary 'old SDK'
    Write-Sdk $oldOnly '10.0.19041.0'
    Check (Rejects { Find-HarpiaWindowsSdk -Roots @($oldOnly) } 'https://developer.microsoft.com/windows/downloads/windows-sdk/') 'an unsupported SDK produces the official download link'
    $x86Only = Join-Path $temporary 'x86 SDK'
    Write-Sdk $x86Only '10.0.30000.0' 'x86'
    Check (Rejects { Find-HarpiaWindowsSdk -Roots @($x86Only) } 'x64 libraries') 'an x86-only installation cannot pass an x64 build preflight'
    Check (Rejects { Find-HarpiaWindowsSdk -Roots @((Join-Path $temporary 'missing')) } 'Visual Studio Installer') 'missing installations explain the Installer alternative'

    $minimum = Join-Path $temporary 'minimum supported SDK'
    Write-Sdk $minimum '10.0.20348.0'
    Check ((Find-HarpiaWindowsSdk -Roots @($minimum)).Version -eq [version]'10.0.20348.0') 'the SDK minimum follows the project compiler requirement'
    $previousEnvironment = @{}
    foreach ($name in @('CMAKE_WINDOWS_KITS_10_DIR', 'WindowsSdkDir')) {
        $previousEnvironment[$name] = [Environment]::GetEnvironmentVariable($name)
    }
    try {
        $env:CMAKE_WINDOWS_KITS_10_DIR = $other
        $env:WindowsSdkDir = $minimum
        # Mock registry reads only; validate discovery with the real fixture paths.
        function Get-ItemProperty {
            [CmdletBinding()]
            param([string] $LiteralPath)
            if ($LiteralPath -match '^HKLM:') { return [pscustomobject]@{ KitsRoot10 = $root } }
        }
        $roots = @(Get-HarpiaWindowsSdkRoots)
        Check ($roots -contains $other -and $roots -contains $minimum -and $roots -contains $root) 'registry and environment SDK locations are all searched'
    }
    finally {
        Remove-Item Function:Get-ItemProperty
        foreach ($name in $previousEnvironment.Keys) {
            [Environment]::SetEnvironmentVariable($name, $previousEnvironment[$name])
        }
    }

    $build = Join-Path $temporary 'Harpia Rec [build]'
    $cachePath = Join-Path $build 'CMakeCache.txt'
    Write-FixtureFile $cachePath
    [IO.File]::WriteAllText($cachePath, @"
CMAKE_GENERATOR:INTERNAL=Visual Studio 18 2026
CMAKE_GENERATOR_PLATFORM:INTERNAL=x64,version=10.0.26100.0
CMAKE_RC_COMPILER:FILEPATH=$($found.ResourceCompiler)
"@)
    $cache = Read-HarpiaBuildCache $cachePath
    Check ($cache['CMAKE_GENERATOR_PLATFORM'] -eq 'x64,version=10.0.26100.0') 'cache reading retains the exact platform pin, with literal paths containing brackets'
    $output = Join-Path $build 'rundir/RelWithDebInfo/bin/64bit/harpia.exe'
    Write-FixtureFile $output
    $plan = Get-HarpiaWindowsConfigurePlan -SourceRoot $temporary -BuildDir $build `
        -Generator 'Visual Studio 18 2026' -Sdk $found -Cache $cache -CacheExists $true
    Check ($plan.Configure -and $plan.Fresh -and $plan.Arguments -contains '--fresh') 'an unavailable pinned SDK triggers automatic CMake cache refresh'
    Check ($plan.Arguments -contains 'x64,version=10.0.28000.0') 'fresh configuration explicitly selects the detected installed SDK'
    Check ($plan.Arguments -contains "-DCMAKE_RC_COMPILER:FILEPATH=$($found.ResourceCompiler)") 'the resource compiler comes from the selected SDK'
    Check ((Get-Content -LiteralPath $output -Raw) -eq 'SDK fixture') 'cache repair planning preserves existing build output'
    $cache['CMAKE_GENERATOR_PLATFORM'] = 'x64,version=10.0.28000.0'
    Write-FixtureFile (Join-Path $build 'CMakeFiles/generate.stamp')
    $plan = Get-HarpiaWindowsConfigurePlan -SourceRoot $temporary -BuildDir $build `
        -Generator 'Visual Studio 18 2026' -Sdk $found -Cache $cache -CacheExists $true
    Check (-not $plan.Configure -and -not $plan.Fresh) 'a compatible generated build can be reused'
    $plan = Get-HarpiaWindowsConfigurePlan -SourceRoot $temporary -BuildDir $build `
        -Generator 'Visual Studio 18 2026' -Sdk $found -Cache $cache -CacheExists $true -Reconfigure
    Check ($plan.Configure -and -not $plan.Fresh) 'an explicit reconfigure retains a compatible cache'
    Remove-Item -LiteralPath (Join-Path $build 'CMakeFiles/generate.stamp')
    $plan = Get-HarpiaWindowsConfigurePlan -SourceRoot $temporary -BuildDir $build `
        -Generator 'Visual Studio 18 2026' -Sdk $found -Cache $cache -CacheExists $true
    Check $plan.Configure 'a failed configure is retried rather than treated as a usable build'
    $plan = Get-HarpiaWindowsConfigurePlan -SourceRoot $temporary -BuildDir $build `
        -Generator 'Visual Studio 18 2026' -Sdk $found
    Check ($plan.Configure -and -not $plan.Fresh -and $plan.Arguments -contains '-G' -and $plan.Arguments -contains '-A') 'a first build sets the generator and detected SDK explicitly'
}
finally {
    if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary -Recurse -Force }
}
if ($failures) { throw "$failures SDK checks failed." }
Write-Host 'ALL PASSED'
