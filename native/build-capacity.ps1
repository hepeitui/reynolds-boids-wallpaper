[CmdletBinding()]
param(
    [string]$Compiler = 'C:\Dev\MinGW\bin\g++.exe',
    [int]$Capacity = 32768,
    [switch]$Reference,
    [switch]$Verify
)

$ErrorActionPreference = 'Stop'
if ($Capacity -lt 1) {
    throw "Capacity must be a positive integer; received $Capacity."
}

$native = $PSScriptRoot
$repo = Split-Path -Parent $native
$binDirectory = Join-Path $repo 'work\capacity\bin'
$mainSource = Join-Path $native 'main.cpp'
$simulationSource = Join-Path $native 'simulation.cpp'
$rendererSource = Join-Path $native 'renderer.cpp'
$verifySource = Join-Path $native 'verify.cpp'

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
if ($Verify -and -not (Test-Path -LiteralPath $verifySource -PathType Leaf)) {
    throw "Verification source is missing: $verifySource"
}

New-Item -ItemType Directory -Force -Path $binDirectory | Out-Null
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

function Build-Application([int]$MaxBirds, [string]$OutputPath) {
    $capacityDefine = "-DFLOCK_MAX_BIRDS=$MaxBirds"
    $arguments = @($common) + @($capacityDefine, '-DFLOCK_CAPACITY_MEASUREMENT=1', '-mwindows', '-municode', $mainSource, $simulationSource, $rendererSource) + $windowsLibraries + @('-o', $OutputPath)
    Write-Host "Building isolated capacity application ($MaxBirds birds): $OutputPath"
    Invoke-Gxx $arguments
}

$capacityExecutable = Join-Path $binDirectory 'ReynoldsBoidsCapacity.exe'
Build-Application $Capacity $capacityExecutable

if ($Reference) {
    $referenceExecutable = Join-Path $binDirectory 'ReynoldsBoidsReference.exe'
    Build-Application 3000 $referenceExecutable
}

if ($Verify) {
    $verifyExecutable = Join-Path $binDirectory 'ReynoldsBoidsCapacityVerify.exe'
    $capacityDefine = "-DFLOCK_MAX_BIRDS=$Capacity"
    $verifyArguments = @($common) + @($capacityDefine, $simulationSource, $verifySource, '-o', $verifyExecutable)
    Write-Host "Building capacity behavior verification: $verifyExecutable"
    Invoke-Gxx $verifyArguments
    Write-Host "Running capacity behavior verification: $verifyExecutable"
    & $verifyExecutable
    if ($LASTEXITCODE -ne 0) {
        throw "Capacity behavior verification failed with exit code $LASTEXITCODE"
    }
}

Write-Host "Capacity application: $capacityExecutable"
if ($Reference) {
    Write-Host "3000-bird reference application: $(Join-Path $binDirectory 'ReynoldsBoidsReference.exe')"
}
