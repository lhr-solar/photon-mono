$ErrorActionPreference = "Stop"
$Root = Resolve-Path (Join-Path $PSScriptRoot "..\..")
$Web = Join-Path $Root ".artifacts-web"
$Port = 8080
if ($args.Count -ge 1) { $Port = [int]$args[0] }

if (-not (Test-Path (Join-Path $Web "photon.html"))) {
    throw "photon.html not found in $Web. Run web/scripts/build.ps1 first."
}

Write-Host "Serving $Web on http://127.0.0.1:$Port/photon.html"
Write-Host "Start the backend separately:"
Write-Host "  .\.artifacts\photon\daemon\photon-backend.exe --ws-port 8765"
python -m http.server $Port --directory $Web
