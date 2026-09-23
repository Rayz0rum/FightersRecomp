param(
    [string]$BuildDir
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$defaultBuild = Join-Path $root 'out\build\win-amd64-release'
$build = if ($BuildDir) { (Resolve-Path -LiteralPath $BuildDir).Path } else { $defaultBuild }
$package = Join-Path $root 'out\release-package'
$public = Join-Path $root 'PublicRelease'

New-Item -ItemType Directory -Path $package -Force | Out-Null

$cache = Join-Path $build 'CMakeCache.txt'
if (-not (Test-Path -LiteralPath $cache)) {
    throw "Release build not found. Run tools\build-game.ps1 first: $build"
}

$sdkLine = Get-Content -LiteralPath $cache | Where-Object { $_ -like 'REXSDK_DIR:PATH=*' } | Select-Object -First 1
$sdkDir = if ($sdkLine) { $sdkLine.Substring('REXSDK_DIR:PATH='.Length) } else { '' }
$sdkBins = @()
if ((Test-Path -LiteralPath (Join-Path $build 'rexruntime.dll')) -and
    (Test-Path -LiteralPath (Join-Path $build 'rexgpu-xenos.dll'))) {
    $sdkBins += $build
}
if ($sdkDir -and (Test-Path -LiteralPath $sdkDir)) {
    $sdkBins += (Join-Path $sdkDir 'out\win-amd64')
    $sdkBins += (Join-Path $sdkDir 'out\install\win-amd64\bin')
}
$sdkBins += (Join-Path $root 'win-amd64\bin')
$sdkBin = $sdkBins | Where-Object {
    (Test-Path -LiteralPath (Join-Path $_ 'rexruntime.dll')) -and
    (Test-Path -LiteralPath (Join-Path $_ 'rexgpu-xenos.dll'))
} | Select-Object -First 1
if (-not $sdkBin) {
    throw 'Could not find rexruntime.dll and rexgpu-xenos.dll for this build. Check REXSDK_DIR and build the SDK first.'
}

$gpuPlugin = Join-Path $sdkBin 'rexgpu-xenos.dll'
$pluginText = [System.Text.Encoding]::ASCII.GetString([System.IO.File]::ReadAllBytes($gpuPlugin))
if ($pluginText -notmatch 'VulkanGraphicsSystem' -or $pluginText -notmatch 'D3D12GraphicsSystem') {
    throw "The selected GPU plugin does not contain both D3D12 and Vulkan backends: $gpuPlugin"
}

$game = Join-Path $package 'stfrecompiled.exe'

Copy-Item -LiteralPath (Join-Path $build 'stf_xbla.exe') -Destination $game -Force
Copy-Item -LiteralPath (Join-Path $sdkBin 'rexruntime.dll') -Destination (Join-Path $public 'rexruntime.dll') -Force
Copy-Item -LiteralPath $gpuPlugin -Destination (Join-Path $public 'rexgpu-xenos.dll') -Force

Write-Host "Prepared public binaries from $build and $sdkBin"
