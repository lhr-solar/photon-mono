$ErrorActionPreference = "Stop"
$logStamp = Get-Date -Format o
Write-Output "wasm-build $logStamp"
$pyHome = "C:\Users\alper\AppData\Local\Programs\Python\Python314"
$ninjaHome = "C:\Users\alper\AppData\Roaming\Python\Python314\Scripts"
$env:PATH = "$pyHome;$pyHome\Scripts;$ninjaHome;C:\Program Files\CMake\bin;C:\Program Files\Git\cmd;" + $env:PATH
Write-Output ("python=" + (Get-Command python).Source)
python --version
Set-Location "F:\lhr\photon-next"
$emsdk = "F:\lhr\photon-next\.external\emsdk"
if (-not (Test-Path (Join-Path $emsdk "upstream\emscripten\emcmake.bat")) -and
    -not (Test-Path (Join-Path $emsdk "upstream\emscripten\emcmake"))) {
    Write-Output "emsdk install latest"
    Push-Location $emsdk
    try {
        .\emsdk.ps1 install latest
        .\emsdk.ps1 activate latest
    } finally {
        Pop-Location
    }
}
Write-Output "running frontend/scripts/build.ps1"
& "F:\lhr\photon-next\frontend\scripts\build.ps1"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Write-Output "wasm-build ok"
