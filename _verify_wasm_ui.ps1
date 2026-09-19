$ErrorActionPreference = "Continue"
Write-Output "=== ui files ==="
Get-ChildItem "F:\lhr\photon-server\ui\photon.*" | Format-Table Name,Length,LastWriteTime -AutoSize | Out-String
$html = Get-Content "F:\lhr\photon-server\ui\photon.html" -Raw
$m = [regex]::Match($html, "photon\.js\?v=([A-Za-z0-9._-]+)")
Write-Output ("ver=" + $m.Groups[1].Value)
Write-Output ("glfix=" + $html.Contains("glfix-20260912"))
Write-Output ("photon-gl-lost=" + $html.Contains("photon-gl-lost"))
Write-Output "=== log markers ==="
Select-String -Path "F:\lhr\photon-next\_wasm_build.log" -Pattern "Copied UI|wasm-build ok|Built F:|ninja: build stopped" | Select-Object -Last 12 | ForEach-Object { $_.Line.Trim() }
Write-Output "=== artifacts-web ==="
if (Test-Path "F:\lhr\photon-next\.artifacts-web\photon.html") {
  Get-ChildItem "F:\lhr\photon-next\.artifacts-web\photon.*" | Format-Table Name,Length,LastWriteTime -AutoSize | Out-String
}
