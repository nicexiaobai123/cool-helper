[CmdletBinding()]
param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release",

    [ValidateRange(1, 256)]
    [int]$Jobs = [Environment]::ProcessorCount,

    [switch]$Clean,
    [switch]$Reconfigure,
    [switch]$Test,
    [switch]$Package,
    [switch]$Run,
    [switch]$Help,

    [string]$PackageRoot = ""
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version 3.0

function Invoke-CMake {
    param([Parameter(Mandatory)][string[]]$Arguments)

    Write-Host ("cmake " + ($Arguments -join " ")) -ForegroundColor DarkGray
    & cmake @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "CMake command failed with exit code $LASTEXITCODE."
    }
}

function Show-Usage {
    Write-Host @"
CoolHelperHub quick build

Usage:
  .\build.ps1                         Incremental Release build
  .\build.ps1 -Configuration Debug    Incremental Debug build
  .\build.ps1 -Clean                  Clean then rebuild
  .\build.ps1 -Test                   Build and run all tests
  .\build.ps1 -Package                Create a timestamped package in out\
  .\build.ps1 -Run                    Package and launch without locking build\Release
  .\build.ps1 -Reconfigure            Force CMake configure before building

The same arguments can be passed through build.cmd, for example:
  build.cmd -Configuration Debug -Test
"@
}

if ($Help) {
    Show-Usage
    exit 0
}

$projectRoot = $PSScriptRoot
$buildDirectory = Join-Path $projectRoot "build"
$cacheFile = Join-Path $buildDirectory "CMakeCache.txt"
$executable = Join-Path $buildDirectory "$Configuration\CoolHelperHub.exe"

if ([string]::IsNullOrWhiteSpace($PackageRoot)) {
    $PackageRoot = Join-Path $projectRoot "out"
} elseif (-not [System.IO.Path]::IsPathRooted($PackageRoot)) {
    $PackageRoot = Join-Path $projectRoot $PackageRoot
}

if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    # Fall back to the CMake bundled with an installed Visual Studio.
    $bundledCmake = @(
        "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
        "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    ) | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
    if ($bundledCmake) {
        Write-Host "Using bundled cmake: $bundledCmake" -ForegroundColor DarkGray
        $env:Path = (Split-Path -Parent $bundledCmake) + ";" + $env:Path
    } else {
        throw "cmake was not found in PATH. Install Visual Studio with C++ CMake tools, or add cmake.exe to PATH."
    }
}

function Test-VSInstance {
    param([string]$Root)
    return (Test-Path -LiteralPath (Join-Path $Root "Common7\IDE\devenv.exe"))
}

$vs2022Root = "C:\Program Files\Microsoft Visual Studio\2022\Community"
$vs2019Root = "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community"
$hasVs2022 = Test-VSInstance $vs2022Root
$hasVs2019 = Test-VSInstance $vs2019Root

$stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
Push-Location $projectRoot
try {
    if ($Reconfigure -or -not (Test-Path -LiteralPath $cacheFile)) {
        if ($hasVs2022) {
            Write-Host "[1/3] Configuring Visual Studio 2022 x64..." -ForegroundColor Cyan
            Invoke-CMake -Arguments @("--preset", "vs2022-x64")
        }
        elseif ($hasVs2019) {
            # Preset schemas above v2 need cmake >= 3.21; older bundled CMake
            # versions get the equivalent explicit configure invocation.
            Write-Host "[1/3] Configuring Visual Studio 2019 x64..." -ForegroundColor Cyan
            Invoke-CMake -Arguments @(
                "-S", $projectRoot, "-B", $buildDirectory,
                "-G", "Visual Studio 16 2019", "-A", "x64",
                "-DBUILD_TESTING=ON"
            )
        }
        else {
            throw "Visual Studio 2019 or 2022 with the C++ workload was not found."
        }
    } else {
        Write-Host "[1/3] Configure cache ready (use -Reconfigure to refresh)." -ForegroundColor DarkGray
    }

    Write-Host "[2/3] Building $Configuration with $Jobs parallel job(s)..." -ForegroundColor Cyan
    $buildArguments = @("--build", $buildDirectory, "--config", $Configuration, "--parallel", $Jobs.ToString())
    if (-not $Test) {
        $buildArguments += @("--target", "CoolHelperHub")
    }
    if ($Clean) {
        $buildArguments += "--clean-first"
    }
    Invoke-CMake -Arguments $buildArguments

    if (-not (Test-Path -LiteralPath $executable)) {
        throw "Build completed but the executable was not found: $executable"
    }

    if ($Test) {
        Write-Host "[3/3] Running tests..." -ForegroundColor Cyan
        Write-Host "ctest --test-dir $buildDirectory -C $Configuration" -ForegroundColor DarkGray
        & ctest --test-dir $buildDirectory -C $Configuration --output-on-failure
        if ($LASTEXITCODE -ne 0) {
            throw "Tests failed with exit code $LASTEXITCODE."
        }
    } else {
        Write-Host "[3/3] Tests skipped (use -Test to run them)." -ForegroundColor DarkGray
    }

    $launchPath = $executable
    if ($Package -or $Run) {
        $stamp = Get-Date -Format "yyyyMMdd-HHmmss-fff"
        $packageDirectory = Join-Path $PackageRoot "CoolHelperHub-$Configuration-$stamp"
        Write-Host "Creating isolated package: $packageDirectory" -ForegroundColor Cyan
        Invoke-CMake -Arguments @(
            "--install", $buildDirectory,
            "--config", $Configuration,
            "--prefix", $packageDirectory
        )
        $launchPath = Join-Path $packageDirectory "CoolHelperHub.exe"
    }

    $stopwatch.Stop()
    Write-Host "Build succeeded in $([math]::Round($stopwatch.Elapsed.TotalSeconds, 1))s" -ForegroundColor Green
    Write-Host "Executable: $launchPath" -ForegroundColor Green

    if ($Run) {
        Write-Host "Launching isolated copy..." -ForegroundColor Cyan
        Start-Process -FilePath $launchPath -WorkingDirectory (Split-Path -Parent $launchPath)
    }
} finally {
    Pop-Location
}

