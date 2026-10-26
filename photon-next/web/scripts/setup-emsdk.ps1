param(
    # The emsdk release the page is built and tested with.
    [string]$Version = "6.0.13"
)

$ErrorActionPreference = "Stop"
$Root = Resolve-Path (Join-Path $PSScriptRoot "..\..")
$Emsdk = Join-Path $Root ".external\emsdk"

if (-not (Test-Path (Join-Path $Emsdk "emsdk.ps1"))) {
    New-Item -ItemType Directory -Force -Path (Join-Path $Root ".external") | Out-Null
    git clone --depth 1 https://github.com/emscripten-core/emsdk.git $Emsdk
    if ($LASTEXITCODE -ne 0) { throw "git clone emsdk failed" }
}

Push-Location $Emsdk
try {
    # A shallow clone may not list a release newer than the clone itself.
    git pull --ff-only
    .\emsdk.ps1 install $Version
    if ($LASTEXITCODE -ne 0) { throw "emsdk install $Version failed" }
    .\emsdk.ps1 activate $Version
    if ($LASTEXITCODE -ne 0) { throw "emsdk activate $Version failed" }
} finally {
    Pop-Location
}

Write-Host "emsdk $Version ready at $Emsdk"
Write-Host "In this shell:  . `"$Emsdk\emsdk_env.ps1`""
