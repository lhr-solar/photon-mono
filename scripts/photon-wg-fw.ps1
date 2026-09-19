# Allow Oracle WireGuard (10.66.0.1) to hit photon-server data ports on MACBOOK.
# Wintun/WG adapter "pc" is excluded from Windows Firewall — port rules on that
# NIC do not match (firewall off: open; allow-any rules: still drop).
# Loopback UI :8080 and photon-sshd :2222 stay on ssh -R; they are not opened here.
$ErrorActionPreference = "Stop"
$ifAlias = "pc"
Set-NetFirewallProfile -Name Public, Private, Domain -DisabledInterfaceAliases $ifAlias
$remote = "10.66.0.0/24"
$tcp = 6100, 6200, 6500, 6700, 5400
$name = "Photon-WG-pipe"

Get-NetFirewallRule -DisplayName $name -ErrorAction SilentlyContinue | Remove-NetFirewallRule
Get-NetFirewallRule -DisplayName "${name}-udp" -ErrorAction SilentlyContinue | Remove-NetFirewallRule
New-NetFirewallRule -DisplayName $name -Direction Inbound -Action Allow -Protocol TCP `
    -LocalPort $tcp -RemoteAddress $remote -Profile Any | Out-Null
New-NetFirewallRule -DisplayName "${name}-udp" -Direction Inbound -Action Allow -Protocol UDP `
    -LocalPort 6200 -RemoteAddress $remote -Profile Any | Out-Null
Write-Output "ok excluded $ifAlias from firewall; $name tcp=$($tcp -join ',') udp=6200 from $remote"
