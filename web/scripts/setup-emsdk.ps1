$ErrorActionPreference = "Stop"
$Root = Resolve-Path (Join-Path $PSScriptRoot "..\..")
$Emsdk = Join-Path $Root ".external\emsdk"

if (-not (Test-Path (Join-Path $Emsdk "emsdk.ps1"))) {
    New-Item -ItemType Directory -Force -Path (Join-Path $Root ".external") | Out-Null
    git clone --depth 1 https://github.com/emscripten-core/emsdk.git $Emsdk
}

Push-Location $Emsdk
try {
    .\emsdk.ps1 install latest
    .\emsdk.ps1 activate latest
} finally {
    Pop-Location
}

Write-Host "emsdk ready at $Emsdk"
Write-Host "In this shell:  . `"$Emsdk\emsdk_env.ps1`""
