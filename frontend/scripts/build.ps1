$ErrorActionPreference = "Stop"
$Root = Resolve-Path (Join-Path $PSScriptRoot "..\..")
$Emsdk = Join-Path $Root ".external\emsdk"
$Build = Join-Path $Root ".artifacts-web"

if (-not (Test-Path (Join-Path $Emsdk "emsdk_env.ps1"))) {
    & (Join-Path $PSScriptRoot "setup-emsdk.ps1")
}

. (Join-Path $Emsdk "emsdk_env.ps1")

if (-not (Get-Command emcmake -ErrorAction SilentlyContinue)) {
    throw "emcmake not on PATH after emsdk_env.ps1"
}

emcmake cmake -S $Root -B $Build -G Ninja -DPHOTON_FRONTEND=ON -DBUILD_TESTING=OFF -DCMAKE_BUILD_TYPE=Release
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
cmake --build $Build
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "Built $Build\photon.html"
