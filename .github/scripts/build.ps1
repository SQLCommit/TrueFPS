# Builds build\Release\truefps.dll with CMake and the Ashita SDK at -Sdk.
param([Parameter(Mandatory = $true)][string]$Sdk)
$ErrorActionPreference = 'Stop'
Push-Location (Join-Path $PSScriptRoot '../..')
try {
    $env:ASHITA4_SDK_PATH = $Sdk
    cmake -S . -B build -G 'Visual Studio 17 2022' -A Win32 -DCMAKE_BUILD_TYPE=Release | Out-Host
    if ($LASTEXITCODE) { throw "CMake could not configure the build (exit $LASTEXITCODE)." }
    cmake --build build --config Release | Out-Host
    if ($LASTEXITCODE) { throw "The build failed (exit $LASTEXITCODE)." }
} finally { Pop-Location }
