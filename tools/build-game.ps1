param(
    [string]$SdkDir,
    [switch]$Clean
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$preset = 'win-amd64-release'
$build = Join-Path $root 'out\build\win-amd64-release'

if (-not $SdkDir) {
    $SdkDir = Join-Path $root 'thirdparty\rexglue-sdk'
}

if (-not (Test-Path -LiteralPath (Join-Path $SdkDir 'CMakeLists.txt'))) {
    throw "ReXGlue SDK source was not found: $SdkDir"
}
$llvmBin = Join-Path $env:ProgramFiles 'LLVM\bin'
$clangC = Join-Path $llvmBin 'clang.exe'
$clangCxx = Join-Path $llvmBin 'clang++.exe'
if (-not (Test-Path -LiteralPath $clangCxx)) {
    $clang = Get-Command clang++ -ErrorAction SilentlyContinue
    if (-not $clang) { throw 'Clang 18+ was not found. Install LLVM or add clang++ to PATH.' }
    $clangCxx = $clang.Source
    $clangC = (Get-Command clang -ErrorAction Stop).Source
}
$ninja = Get-Command ninja -ErrorAction SilentlyContinue
$ninjaPath = if ($ninja) { $ninja.Source } else {
    Join-Path $env:VSINSTALLDIR 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe'
}
if (-not (Test-Path -LiteralPath $ninjaPath)) {
    throw 'Ninja was not found. Install it with Visual Studio C++ tools or add it to PATH.'
}
& cmake --preset $preset "-DREXSDK_DIR=$SdkDir" "-DCMAKE_C_COMPILER=$clangC" `
    "-DCMAKE_CXX_COMPILER=$clangCxx" "-DCMAKE_MAKE_PROGRAM=$ninjaPath" `
    '-DCMAKE_C_FLAGS=-mssse3' '-DCMAKE_CXX_FLAGS=-mssse3' `
    -DREXGLUE_USE_D3D12=ON -DREXGLUE_USE_VULKAN=ON
if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed.' }

if ($Clean) {
    & cmake --build --preset $preset --clean-first --parallel 4
} else {
    & cmake --build --preset $preset --parallel 4
}
if ($LASTEXITCODE -ne 0) { throw 'Game build failed.' }

# Copy the DLLs and game data so the EXE can run from the build folder.
$cache = Join-Path $build 'CMakeCache.txt'
$sdkLine = Get-Content -LiteralPath $cache | Where-Object { $_ -like 'REXSDK_DIR:PATH=*' } | Select-Object -First 1
$builtSdk = if ($sdkLine) { $sdkLine.Substring('REXSDK_DIR:PATH='.Length) } else { '' }
$sdkBins = @()
if ($builtSdk) {
    $sdkBins += (Join-Path $builtSdk 'out\win-amd64')
    $sdkBins += (Join-Path $builtSdk 'out\install\win-amd64\bin')
    $sdkBins += (Join-Path $builtSdk 'bin')
}
$sdkBin = $sdkBins | Where-Object {
    (Test-Path -LiteralPath (Join-Path $_ 'rexruntime.dll')) -and
    (Test-Path -LiteralPath (Join-Path $_ 'rexgpu-xenos.dll'))
} | Select-Object -First 1
if (-not $sdkBin) { throw 'Built ReXGlue runtime and GPU plugin were not found in the configured SDK.' }
Copy-Item -LiteralPath (Join-Path $sdkBin 'rexruntime.dll') -Destination $build -Force
Copy-Item -LiteralPath (Join-Path $sdkBin 'rexgpu-xenos.dll') -Destination $build -Force
$stagedGameData = Join-Path $build '_unpacked'
New-Item -ItemType Directory -Force -Path $stagedGameData | Out-Null
Copy-Item -Path (Join-Path $root '_unpacked\*') -Destination $stagedGameData -Recurse -Force

Write-Host "Game build ready: $build\stf_xbla.exe"
