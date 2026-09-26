# Rollback only. Live pipe is Oracle WireGuard DNAT (oracle/photon-wg-pipe.sh).
# Public TCP :6100 is photon-raw-relay on Oracle (do not -R it).
# Reverse-tunnel store fan-out only. Video is photon-video-tunnel.ps1 so
# RTP cannot stall FPGA :6100 on the same SSH TCP session.
#   :6500 TCP fan-out (MacBook store clients)

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
        -R 0.0.0.0:6500:127.0.0.1:6500 `
        openvpnas@150.136.85.207
    Start-Sleep -Seconds 5
}
