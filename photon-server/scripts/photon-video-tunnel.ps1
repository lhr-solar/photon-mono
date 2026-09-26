# Rollback only. Live video ingest is Oracle UDP DNAT to 10.66.0.2:6200.
# Reverse-tunnel video only. Keep this off the CAN SSH session so RTP cannot
# stall FPGA :6100.
#   :6200 TCP video ingest (length-prefixed RTP)
#   :6700 TCP video clients
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
        -R 0.0.0.0:6200:127.0.0.1:6200 `
        -R 0.0.0.0:6700:127.0.0.1:6700 `
        openvpnas@150.136.85.207
    Start-Sleep -Seconds 5
}
