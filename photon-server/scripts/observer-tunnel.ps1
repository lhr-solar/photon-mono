# Rollback only. Live observer is Oracle DNAT to 10.66.0.2:5400.
# Public Oracle :5400 -> MACBOOK observer. Dest is WG 10.66.0.2 so ClientGate
# does not share the 127.0.0.1 6-slot cap with fan-out/video.
$ErrorActionPreference = "Stop"
$key = Join-Path $env:USERPROFILE ".ssh\oci-openvpn-2026-08-31-alt.key"
$ssh = "$env:SystemRoot\System32\OpenSSH\ssh.exe"
if (-not (Test-Path $ssh)) { $ssh = "ssh" }

while ($true) {
    & $ssh -i $key -N `
        -o IdentitiesOnly=yes `
        -o BatchMode=yes `
        -o ExitOnForwardFailure=yes `
        -o ServerAliveInterval=30 `
        -o ServerAliveCountMax=3 `
        -o StrictHostKeyChecking=accept-new `
        -R 0.0.0.0:5400:10.66.0.2:5400 `
        openvpnas@150.136.85.207
    Start-Sleep -Seconds 5
}
