[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',

    [ValidateSet('x64', 'Win32')]
    [string]$Platform = 'x64',

    [switch]$Clean
)

$ErrorActionPreference = 'Stop'

$projectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$solutionPath = Join-Path $projectRoot 'DwmCfgOverlayLab.sln'

if (-not (Test-Path -LiteralPath $solutionPath)) {
    throw "Solution not found: $solutionPath"
}

# Prefer VS2019 (the project currently uses the v142 toolset), then fall back
# to VS2022 if it is installed with the C++ workload.
$vsDevCmdCandidates = @(
    'C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\Common7\Tools\VsDevCmd.bat',
    'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat',
    'C:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\Tools\VsDevCmd.bat',
    'C:\Program Files\Microsoft Visual Studio\2022\Enterprise\Common7\Tools\VsDevCmd.bat',
    'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat'
)

$vsDevCmd = $vsDevCmdCandidates |
    Where-Object { Test-Path -LiteralPath $_ } |
    Select-Object -First 1

if (-not $vsDevCmd) {
    throw 'Visual Studio developer command file was not found. Install VS with the Desktop C++ workload.'
}

$architecture = if ($Platform -eq 'x64') { 'x64' } else { 'x86' }
$target = if ($Clean) { '/t:Clean,Build' } else { '/t:Build' }

$msbuildCommand = @(
    'msbuild',
    ('"{0}"' -f $solutionPath),
    $target,
    '/m',
    ('/p:Configuration={0}' -f $Configuration),
    ('/p:Platform={0}' -f $Platform),
    # VS2019 on the development machine has v142 installed; this override
    # also lets the Debug configuration build when v143 is unavailable.
    '/p:PlatformToolset=v142',
    '/nologo'
) -join ' '

$command = ('"{0}" -arch={1} && {2}' -f $vsDevCmd, $architecture, $msbuildCommand)
Write-Host "Building $Configuration|$Platform"
Write-Host "Using $vsDevCmd"

& cmd.exe /d /c $command
if ($LASTEXITCODE -ne 0) {
    throw "Build failed with exit code $LASTEXITCODE"
}

$outputPath = Join-Path $projectRoot ("DwmCfgOverlayLab\bin\$Platform\$Configuration\DwmCfgOverlayLab.dll")
$legacyOutputPath = Join-Path $projectRoot ("x64\$Configuration\DwmCfgOverlayLab.dll")

$builtDll = $null
if (Test-Path -LiteralPath $legacyOutputPath) {
    $builtDll = $legacyOutputPath
    Write-Host "Build succeeded: $legacyOutputPath" -ForegroundColor Green
} elseif (Test-Path -LiteralPath $outputPath) {
    $builtDll = $outputPath
    Write-Host "Build succeeded: $outputPath" -ForegroundColor Green
} else {
    Write-Host 'Build succeeded.' -ForegroundColor Green
}

# Deploy the x64 DLL next to CoolHelperHub's build output so the injector can
# load it without a manual copy step. A missing hub build directory is not an
# error: the hub may simply not have been built yet.
if ($Platform -eq 'x64' -and $builtDll) {
    $hubBuildOutput = Join-Path (Split-Path -Parent $projectRoot) `
        "CoolHelperHub\build\$Configuration"
    if (Test-Path -LiteralPath $hubBuildOutput) {
        Copy-Item -LiteralPath $builtDll -Destination $hubBuildOutput -Force
        Write-Host "Copied DLL to $hubBuildOutput" -ForegroundColor Cyan
        $builtPdb = [System.IO.Path]::ChangeExtension($builtDll, '.pdb')
        if (Test-Path -LiteralPath $builtPdb) {
            Copy-Item -LiteralPath $builtPdb -Destination $hubBuildOutput -Force
        }
    }
    else {
        Write-Host "CoolHelperHub build output not found at $hubBuildOutput; skipped the DLL copy." -ForegroundColor DarkYellow
    }
}
