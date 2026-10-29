<#
.SYNOPSIS
    Compiles Reynolds Boids C++ simulation into WebAssembly with Emscripten.

.DESCRIPTION
    Builds the standalone single-file WebAssembly module for Wallpaper Engine.
    Uses -sSINGLE_FILE=1 to embed WASM in the JS glue code, avoiding separate CEF MIME/fetch requirements.
    Enforces single-threaded serial execution (-sALLOW_MEMORY_GROWTH=1, no pthread / SharedArrayBuffer).
    Passes --no-entry as this is a library with C API exports rather than a main() binary.

.PARAMETER EmscriptenPath
    Optional custom path to em++ / em++.bat executable or emsdk/emscripten directory. If not specified, automatically probes:
    1. work/toolchains/emsdk/upstream/emscripten/em++.bat
    2. work/toolchains/emsdk/upstream/emscripten/em++.exe
    3. C:/emsdk/upstream/emscripten/em++.bat
    4. C:/emsdk/upstream/emscripten/em++.exe
    5. PATH environment variable

.PARAMETER OutputDirectory
    Optional destination directory for boids-wasm.js.
    Default: If ProjectRoot contains project.json and index.html (e.g. repo root), outputs directly to ProjectRoot.
    Otherwise defaults to ProjectRoot\outputs\reynolds-boids-wasm.
#>

[CmdletBinding()]
param(
    [string]$EmscriptenPath = "",
    [string]$OutputDirectory = ""
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition
$ProjectRoot = Split-Path -Parent $ScriptDir

# Determine output directory
if ($OutputDirectory -and $OutputDirectory.Trim() -ne "") {
    $OutputDir = $OutputDirectory
} elseif ((Test-Path (Join-Path $ProjectRoot "project.json")) -and (Test-Path (Join-Path $ProjectRoot "index.html"))) {
    $OutputDir = $ProjectRoot
} else {
    $OutputDir = Join-Path $ProjectRoot "outputs\reynolds-boids-wasm"
}

Write-Host "====================================================" -ForegroundColor Cyan
Write-Host "  Reynolds Boids - WebAssembly Build Script" -ForegroundColor Cyan
Write-Host "====================================================" -ForegroundColor Cyan

# 1. Locate Emscripten compiler (em++.bat or em++)
$Emcc = $null

if ($EmscriptenPath -and (Test-Path $EmscriptenPath)) {
    if (-not (Test-Path $EmscriptenPath -PathType Container)) {
        # Direct file path supplied
        $Emcc = (Resolve-Path $EmscriptenPath).Path
    } elseif (Test-Path (Join-Path $EmscriptenPath "em++.bat")) {
        $Emcc = (Resolve-Path (Join-Path $EmscriptenPath "em++.bat")).Path
    } elseif (Test-Path (Join-Path $EmscriptenPath "em++.exe")) {
        $Emcc = (Resolve-Path (Join-Path $EmscriptenPath "em++.exe")).Path
    } elseif (Test-Path (Join-Path $EmscriptenPath "em++")) {
        $Emcc = (Resolve-Path (Join-Path $EmscriptenPath "em++")).Path
    }
}

if (-not $Emcc) {
    # Probe Candidate Paths
    $Candidates = @(
        (Join-Path $ProjectRoot "work\toolchains\emsdk\upstream\emscripten\em++.bat"),
        (Join-Path $ProjectRoot "work\toolchains\emsdk\upstream\emscripten\em++.exe"),
        (Join-Path $ProjectRoot "..\work\toolchains\emsdk\upstream\emscripten\em++.bat"),
        (Join-Path $ProjectRoot "..\work\toolchains\emsdk\upstream\emscripten\em++.exe"),
        "C:\emsdk\upstream\emscripten\em++.bat",
        "C:\emsdk\upstream\emscripten\em++.exe"
    )

    foreach ($Candidate in $Candidates) {
        if (Test-Path $Candidate) {
            $Emcc = (Resolve-Path $Candidate).Path
            break
        }
    }
}

if (-not $Emcc) {
    $CommandCheck = Get-Command "em++" -ErrorAction SilentlyContinue
    if ($CommandCheck) {
        $Emcc = $CommandCheck.Source
    }
}

if (-not $Emcc) {
    Write-Error "Could not find em++ compiler. Please specify -EmscriptenPath or run emsdk_env.bat first."
    exit 1
}

Write-Host "Found Emscripten compiler at: $Emcc" -ForegroundColor Green

# Ensure output directory exists
if (-not (Test-Path $OutputDir)) {
    New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null
}

$OutputFile = Join-Path $OutputDir "boids-wasm.js"
$SourceSim = Join-Path $ScriptDir "simulation.cpp"
$SourceBridge = Join-Path $ScriptDir "web\wasm_bridge.cpp"

if (-not (Test-Path $SourceSim)) {
    Write-Error "Source file missing: $SourceSim"
    exit 1
}
if (-not (Test-Path $SourceBridge)) {
    Write-Error "Source file missing: $SourceBridge"
    exit 1
}

Write-Host "Compiling:" -ForegroundColor Yellow
Write-Host "  - $SourceSim"
Write-Host "  - $SourceBridge"
Write-Host "Output: $OutputFile" -ForegroundColor Yellow

$ExportedFunctionsList = @(
    "_boids_init",
    "_boids_destroy",
    "_boids_reset",
    "_boids_configure",
    "_boids_step",
    "_boids_get_time",
    "_boids_get_stats",
    "_boids_prepare_render_frame",
    "_boids_get_bird_count",
    "_boids_get_bird_instances",
    "_boids_get_trail_count",
    "_boids_get_trail_instances",
    "_boids_get_bird_mesh",
    "_boids_get_bird_mesh_vertex_count",
    "_boids_get_trail_corners",
    "_boids_get_trail_corner_vertex_count",
    "_malloc",
    "_free"
)
$ExportedFunctionsJson = $ExportedFunctionsList | ConvertTo-Json -Compress

$ExportedRuntimeMethodsList = @("HEAPF32", "HEAP32", "cwrap", "ccall")
$ExportedRuntimeMethodsJson = $ExportedRuntimeMethodsList | ConvertTo-Json -Compress

$CompileArgs = @(
    "-std=c++20",
    "-O3",
    "-DNDEBUG",
    "--no-entry",
    "-sSTANDALONE_WASM=0",
    "-sSINGLE_FILE=1",
    "-sEXPORT_NAME=ReynoldsBoidsModule",
    "-sMODULARIZE=1",
    "-sEXPORTED_FUNCTIONS=$ExportedFunctionsJson",
    "-sEXPORTED_RUNTIME_METHODS=$ExportedRuntimeMethodsJson",
    "-sALLOW_MEMORY_GROWTH=1",
    "-sINITIAL_MEMORY=33554432",
    "-sFILESYSTEM=0",
    "-sDISABLE_EXCEPTION_CATCHING=1",
    "-o", $OutputFile,
    $SourceSim,
    $SourceBridge
)

Write-Host "Invoking em++..." -ForegroundColor Cyan
& $Emcc @CompileArgs

if ($LASTEXITCODE -ne 0) {
    Write-Error "Emscripten compilation failed with exit code $LASTEXITCODE."
    exit $LASTEXITCODE
}

Write-Host "Compilation SUCCESS! Output generated at:" -ForegroundColor Green
Write-Host "  $OutputFile ($((Get-Item $OutputFile).Length) bytes)"
Write-Host "====================================================" -ForegroundColor Cyan
