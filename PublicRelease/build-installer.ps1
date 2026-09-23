param(
    [string]$GameBuildDir
)

$ErrorActionPreference = 'Stop'

$project = Join-Path $PSScriptRoot 'InstallerSource\STFInstaller.csproj'
$publish = Join-Path $PSScriptRoot 'InstallerSource\bin\Release\net10.0-windows\win-x64\publish\install.exe'

$prepare = Join-Path (Split-Path -Parent $PSScriptRoot) 'tools\prepare-public-binaries.ps1'
if ($GameBuildDir) {
    & $prepare -BuildDir $GameBuildDir
} else {
    & $prepare
}
dotnet publish $project -c Release -r win-x64 --self-contained true
if (-not (Test-Path -LiteralPath $publish)) {
    throw "Installer output was not created: $publish"
}

Copy-Item -LiteralPath $publish -Destination (Join-Path $PSScriptRoot 'install.exe') -Force
Write-Host "Created: $(Join-Path $PSScriptRoot 'install.exe')"
