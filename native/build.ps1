[CmdletBinding()]
param(
    [string]$Compiler = 'C:\Dev\MinGW\bin\g++.exe',
    [switch]$Tools,
    [switch]$Verify
)

$ErrorActionPreference = 'Stop'
$native = $PSScriptRoot
$repo = Split-Path -Parent $native
$releaseDirectory = Join-Path $repo 'outputs\reynolds-boids-native'
$toolsDirectory = Join-Path $native 'build'
$mainSource = Join-Path $native 'main.cpp'
$simulationSource = Join-Path $native 'simulation.cpp'
$rendererSource = Join-Path $native 'renderer.cpp'

if (-not (Test-Path -LiteralPath $Compiler -PathType Leaf)) {
    $resolvedCompiler = Get-Command $Compiler -ErrorAction SilentlyContinue
    if ($null -eq $resolvedCompiler) {
        throw "MinGW g++ not found: $Compiler. Pass -Compiler with an existing g++. No toolchain is installed by this script."
    }
    $Compiler = $resolvedCompiler.Source
}
foreach ($source in @($mainSource, $simulationSource, $rendererSource)) {
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
        throw "Required native source is missing: $source"
    }
}

New-Item -ItemType Directory -Force -Path $releaseDirectory | Out-Null
$common = @(
    '-std=c++20', '-O3', '-DNDEBUG', '-pthread', '-static', '-static-libgcc', '-static-libstdc++',
    '-DUNICODE', '-D_UNICODE', '-D_WIN32_WINNT=0x0A00', "-I$native"
)
$windowsLibraries = @(
    '-ld3d11', '-ldxgi', '-ld3dcompiler', '-lwindowscodecs', '-lole32', '-luuid',
    '-luser32', '-lgdi32', '-lshell32', '-lcomctl32', '-lcomdlg32', '-lwtsapi32'
)

function Invoke-Gxx([string[]]$Arguments) {
    & $Compiler @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "g++ failed with exit code $LASTEXITCODE"
    }
}

$application = Join-Path $releaseDirectory 'ReynoldsBoids.exe'
$applicationArguments = @($common) + @('-mwindows', '-municode', $mainSource, $simulationSource, $rendererSource) + $windowsLibraries + @('-o', $application)
Write-Host "Building $application"
Invoke-Gxx $applicationArguments

$verifySource = Join-Path $native 'verify.cpp'
$benchmarkSource = Join-Path $native 'benchmark.cpp'
$verifyExecutable = Join-Path $toolsDirectory 'ReynoldsBoidsVerify.exe'
$benchmarkExecutable = Join-Path $toolsDirectory 'ReynoldsBoidsBenchmark.exe'

if ($Tools -or $Verify) {
    if (-not (Test-Path -LiteralPath $verifySource -PathType Leaf)) {
        throw "Verification source is missing: $verifySource"
    }
    New-Item -ItemType Directory -Force -Path $toolsDirectory | Out-Null
    $verifyArguments = @($common) + @($simulationSource, $verifySource) + @('-o', $verifyExecutable)
    Write-Host "Building validation utility $verifyExecutable"
    Invoke-Gxx $verifyArguments

    if ($Tools) {
        if (-not (Test-Path -LiteralPath $benchmarkSource -PathType Leaf)) {
            throw "Benchmark source is missing: $benchmarkSource"
        }
        $benchmarkArguments = @($common) + @($simulationSource, $benchmarkSource) + @('-o', $benchmarkExecutable)
        Write-Host "Building benchmark utility $benchmarkExecutable"
        Invoke-Gxx $benchmarkArguments
    }
}

if ($Verify) {
    Write-Host "Running behavior verification $verifyExecutable"
    & $verifyExecutable
    if ($LASTEXITCODE -ne 0) {
        throw "Behavior verification failed with exit code $LASTEXITCODE"
    }
}

Write-Host "Application output: $application"
if ($Tools) {
    Write-Host "Validation tools output: $toolsDirectory"
}
