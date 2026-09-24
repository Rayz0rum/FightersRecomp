param()

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$xex = Join-Path $root '_unpacked\default.xex'
$rexglue = Join-Path $root 'thirdparty\rexglue-sdk\out\win-amd64\rexglue.exe'
$manifest = Join-Path $root 'stf_xbla_manifest.toml'
$cmake = Join-Path $root 'generated\rexglue.cmake'

# The game hooks in the manifest are tied to instruction addresses in this exact
# executable (the one unpacked by the installer).
$expectedXexSha256 = '34A1DEC60BAA20B68E320B0AAC38F481572072EBABFC6AC69EC4D0FCBBC45CDF'

foreach ($path in @($xex, $rexglue, $manifest, $cmake)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Required file not found: $path"
    }
}

if ((Get-FileHash -LiteralPath $xex -Algorithm SHA256).Hash -ne $expectedXexSha256) {
    throw 'default.xex does not match the expected game version.'
}

$backup = [IO.Path]::GetTempFileName()
Copy-Item -LiteralPath $cmake -Destination $backup -Force

Push-Location $root
try {
    & $rexglue codegen $manifest --ignore-stamp
    if ($LASTEXITCODE -ne 0) { throw "ReXGlue codegen failed: $LASTEXITCODE" }

    Write-Host 'Generated game sources are ready. Run .\tools\build-game.cmd next.'
}
finally {
    Copy-Item -LiteralPath $backup -Destination $cmake -Force
    Remove-Item -LiteralPath $backup -Force
    Pop-Location
}
