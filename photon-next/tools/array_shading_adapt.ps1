# Schema 1 adapter for lhr-solar/ArrayShading.
# Photon reads only the JSON line this script prints. If ArrayShading
# changes its flags or its stdout, change this script, not Photon.
$ErrorActionPreference = 'Stop'

function Write-SchemaError([string]$Message) {
    $safe = $Message.Replace('\', '\\').Replace('"', '\"')
    Write-Output ('{"schema":1,"error":"' + $safe + '"}')
}

$exe = $env:PHOTON_ARRAY_SHADING_EXE
$stl = $env:PHOTON_ARRAY_SHADING_STL
if ([string]::IsNullOrWhiteSpace($exe) -or -not (Test-Path -LiteralPath $exe)) {
    Write-SchemaError 'Set PHOTON_ARRAY_SHADING_EXE to solar_whitted_cpp.exe'
    exit 2
}
if ([string]::IsNullOrWhiteSpace($stl) -or -not (Test-Path -LiteralPath $stl)) {
    Write-SchemaError 'Set PHOTON_ARRAY_SHADING_STL to the car STL'
    exit 2
}

$efficiency = 0.24
if (-not [string]::IsNullOrWhiteSpace($env:PHOTON_ARRAY_SHADING_EFFICIENCY)) {
    $efficiency = [double]::Parse($env:PHOTON_ARRAY_SHADING_EFFICIENCY, [Globalization.CultureInfo]::InvariantCulture)
}

$rays = 16
if (-not [string]::IsNullOrWhiteSpace($env:PHOTON_ARRAY_SHADING_RAYS)) {
    $rays = [int]$env:PHOTON_ARRAY_SHADING_RAYS
}

$out = Join-Path $env:TEMP ('array-shading-' + [guid]::NewGuid().ToString() + '.csv')
$log = & $exe --headless --stl $stl --output $out --rays $rays 2>&1 | Out-String
if ($LASTEXITCODE -ne 0) {
    Write-SchemaError ("ArrayShading exited " + $LASTEXITCODE)
    exit $LASTEXITCODE
}

$match = [regex]::Match($log, 'incident optical power:\s*([0-9eE.+\-]+)')
if (-not $match.Success) {
    Write-SchemaError 'ArrayShading stdout has no incident optical power line'
    exit 2
}

$optical = [double]::Parse($match.Groups[1].Value, [Globalization.CultureInfo]::InvariantCulture)
$electrical = $optical * $efficiency
$culture = [Globalization.CultureInfo]::InvariantCulture
Write-Output ('{"schema":1,"incident_optical_w":' + $optical.ToString($culture) + ',"electrical_w":' + $electrical.ToString($culture) + '}')
exit 0
