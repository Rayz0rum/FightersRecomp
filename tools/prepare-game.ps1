param()

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$xex = Join-Path $root '_unpacked\default.xex'
$rexglue = Join-Path $root 'thirdparty\rexglue-sdk\out\win-amd64\rexglue.exe'
$manifest = Join-Path $root 'stf_xbla_manifest.toml'
$cmake = Join-Path $root 'generated\rexglue.cmake'
$patch = Join-Path $root 'patches\generated-default.patch'

foreach ($path in @($xex, $rexglue, $manifest, $cmake, $patch)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Required file not found: $path"
    }
}

$backup = [IO.Path]::GetTempFileName()
Copy-Item -LiteralPath $cmake -Destination $backup -Force

Push-Location $root
try {
    & $rexglue codegen $manifest --ignore-stamp
    if ($LASTEXITCODE -ne 0) { throw "ReXGlue codegen failed: $LASTEXITCODE" }

    git apply --check --unidiff-zero -- $patch
    if ($LASTEXITCODE -ne 0) { throw 'Generated sources do not match the expected game version.' }
    git apply --unidiff-zero -- $patch
    if ($LASTEXITCODE -ne 0) { throw 'Failed to apply project changes to generated sources.' }

    Write-Host 'Generated game sources are ready. Run .\tools\build-game.cmd next.'
}
finally {
    Copy-Item -LiteralPath $backup -Destination $cmake -Force
    Remove-Item -LiteralPath $backup -Force
    Pop-Location
}
