# Run as Administrator on LHRSolarOfficePC after officepc-enable-ssh.ps1.
# Installs WireGuard 10.66.0.4 -> Oracle, photon-server autostart, campus UI proxy.
# Does not cut MACBOOK ingest. Oracle STORE stays 10.66.0.2 until retarget-store-officepc.sh.
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Split-Path -Parent $here
if (-not (Test-Path (Join-Path $root "photon-server.exe"))) {
    $root = "F:\lhr\photon-server"
}

$confSrc = @(
    "$env:USERPROFILE\OpenVPN\config\photon-officepc.conf",
    "C:\Users\alper\OpenVPN\config\photon-officepc.conf",
    (Join-Path $here "photon-officepc.conf")
) | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $confSrc) {
    throw "missing photon-officepc.conf (WireGuard private key). Copy it next to this script or to OpenVPN\config."
}

& (Join-Path $here "officepc-enable-ssh.ps1")

if (-not (Get-Command wireguard -ErrorAction SilentlyContinue) -and
    -not (Test-Path "C:\Program Files\WireGuard\wireguard.exe")) {
    Write-Output "installing WireGuard"
    winget install --id WireGuard.WireGuard -e --accept-package-agreements --accept-source-agreements
}

$wgExe = "C:\Program Files\WireGuard\wireguard.exe"
$confDst = "C:\Program Files\WireGuard\Data\Configurations\photon-officepc.conf.dpapi"
$plain = "C:\Program Files\WireGuard\Data\Configurations\photon-officepc.conf"
New-Item -ItemType Directory -Force -Path (Split-Path $plain) | Out-Null
Copy-Item -Force $confSrc $plain
& $wgExe /installtunnelservice $plain
Start-Sleep -Seconds 2
$alias = @("photon-officepc", "pc") | Where-Object { Get-NetAdapter -Name $_ -ErrorAction SilentlyContinue } | Select-Object -First 1
if (-not $alias) {
    $alias = (Get-NetAdapter | Where-Object { $_.InterfaceDescription -match "WireGuard|Wintun" } | Select-Object -First 1).Name
}
if ($alias) {
    Set-NetFirewallProfile -Name Public, Private, Domain -DisabledInterfaceAliases $alias
    Write-Output "excluded WG adapter $alias from Windows Firewall"
}

$remoteWg = "10.66.0.0/24"
Get-NetFirewallRule -DisplayName "Photon-WG-pipe" -ErrorAction SilentlyContinue | Remove-NetFirewallRule
Get-NetFirewallRule -DisplayName "Photon-WG-pipe-udp" -ErrorAction SilentlyContinue | Remove-NetFirewallRule
New-NetFirewallRule -DisplayName "Photon-WG-pipe" -Direction Inbound -Action Allow -Protocol TCP `
    -LocalPort 6100, 6200, 6500, 6700, 5400 -RemoteAddress $remoteWg -Profile Any | Out-Null
New-NetFirewallRule -DisplayName "Photon-WG-pipe-udp" -Direction Inbound -Action Allow -Protocol UDP `
    -LocalPort 6200 -RemoteAddress $remoteWg -Profile Any | Out-Null

# Local UI is loopback :8080. Publish :9080 on campus/VPN so viewers skip Oracle.
$campus = @("128.62.0.0/16", "128.83.0.0/16", "10.0.0.0/8", "100.64.0.0/10", "10.147.17.0/24")
Get-NetFirewallRule -DisplayName "Photon-UI-campus" -ErrorAction SilentlyContinue | Remove-NetFirewallRule
New-NetFirewallRule -DisplayName "Photon-UI-campus" -Direction Inbound -Action Allow -Protocol TCP `
    -LocalPort 9080 -RemoteAddress $campus -Profile Any | Out-Null
netsh interface portproxy delete v4tov4 listenport=9080 listenaddress=0.0.0.0 2>$null | Out-Null
netsh interface portproxy add v4tov4 listenaddress=0.0.0.0 listenport=9080 connectaddress=127.0.0.1 connectport=8080
Write-Output "campus UI http://<this-host>:9080/photon.html (UT VPN or on campus)"

$data = "F:\lhr\data"
if (-not (Test-Path "F:\")) { $data = "C:\lhr\data" }
New-Item -ItemType Directory -Force -Path $data | Out-Null

$ensure = Join-Path $root "scripts\ensure-running.ps1"
if (Test-Path $ensure) {
    $taskName = "photon-server"
    Unregister-ScheduledTask -TaskName $taskName -Confirm:$false -ErrorAction SilentlyContinue
    $action = New-ScheduledTaskAction -Execute "powershell.exe" `
        -Argument "-NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File `"$ensure`"" `
        -WorkingDirectory $root
    $startup = New-ScheduledTaskTrigger -AtStartup
    $logon = New-ScheduledTaskTrigger -AtLogOn
    $settings = New-ScheduledTaskSettingsSet -ExecutionTimeLimit ([TimeSpan]::Zero) `
        -RestartCount 3 -RestartInterval (New-TimeSpan -Minutes 1) `
        -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -StartWhenAvailable -MultipleInstances IgnoreNew
    $principal = New-ScheduledTaskPrincipal -UserId "SYSTEM" -LogonType ServiceAccount -RunLevel Highest
    Register-ScheduledTask -TaskName $taskName -Action $action -Trigger @($startup, $logon) `
        -Principal $principal -Settings $settings `
        -Description "WMI-start photon-server + photon-sshd at boot." | Out-Null
    Start-ScheduledTask -TaskName $taskName
    Write-Output "registered $taskName"
} else {
    Write-Output "photon-server tree missing at $root — copy binaries then re-run"
}

Write-Output "--- wg ---"
Get-NetIPAddress -AddressFamily IPv4 | Where-Object { $_.IPAddress -like "10.66.*" } |
    ForEach-Object { Write-Output ($_.InterfaceAlias + " " + $_.IPAddress) }
Write-Output "DONE bootstrap. FPGA dest stays 150.136.85.207. Do not retarget Oracle STORE until ping 10.66.0.4 from Oracle works."
