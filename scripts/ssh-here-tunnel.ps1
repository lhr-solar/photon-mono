# Reverse-tunnel photon-sshd (loopback :2222) to Oracle :2222.
# Laptop: ssh photon-macbook   (or ssh -p 2222 alper@150.136.85.207)
# This is an OpenSSH *client* to Oracle only. The MacBook shell is photon-sshd, not sshd.

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
        -R 0.0.0.0:2222:127.0.0.1:2222 `
        openvpnas@150.136.85.207
    Start-Sleep -Seconds 5
}
