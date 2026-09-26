# Run as Administrator on LHRSolarOfficePC.
# Opens SSH so the laptop (SamsungS22) can finish the Photon + WireGuard pipe.
# Safe to re-run.
$ErrorActionPreference = "Stop"

$pub = "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIJlgIVwYmBmyYFHSFWaLGUVqXss3qQLg13MdNcJcO+9t alperenaydin1@gmail.com"

function Ensure-SshKey([string]$path, [bool]$adminFile) {
    $dir = Split-Path $path
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    if (-not (Test-Path $path) -or -not (Select-String -Path $path -Pattern "alperenaydin1@gmail.com" -Quiet -ErrorAction SilentlyContinue)) {
        Add-Content -Path $path -Value $pub
        Write-Output "authorized_keys updated $path"
    } else {
        Write-Output "authorized_keys already has laptop key $path"
    }
    if ($adminFile) {
        icacls $path /inheritance:r | Out-Null
        icacls $path /grant "SYSTEM:(F)" /grant "BUILTIN\Administrators:(F)" | Out-Null
    }
}

Write-Output ("host=" + $env:COMPUTERNAME + " user=" + $env:USERNAME)

$cap = Get-WindowsCapability -Online | Where-Object { $_.Name -like "OpenSSH.Server*" }
if ($cap -and $cap.State -ne "Installed") {
    Write-Output "installing OpenSSH.Server"
    Add-WindowsCapability -Online -Name $cap.Name | Out-Null
} else {
    Write-Output ("OpenSSH.Server " + $(if ($cap) { $cap.State } else { "missing-capability" }))
}

Start-Service sshd
Set-Service -Name sshd -StartupType Automatic
if (Get-Service ssh-agent -ErrorAction SilentlyContinue) {
    Set-Service -Name ssh-agent -StartupType Manual
}

Ensure-SshKey (Join-Path $env:USERPROFILE ".ssh\authorized_keys") $false
Ensure-SshKey "C:\ProgramData\ssh\administrators_authorized_keys" $true

$sshdCfg = "C:\ProgramData\ssh\sshd_config"
if (Test-Path $sshdCfg) {
    $raw = Get-Content $sshdCfg -Raw
    if ($raw -match "(?m)^Match Group administrators") {
        $raw = $raw -replace "(?m)^Match Group administrators", "# Match Group administrators"
        $raw = $raw -replace "(?m)^\s+AuthorizedKeysFile\s+__PROGRAMDATA__/ssh/administrators_authorized_keys", "# AuthorizedKeysFile __PROGRAMDATA__/ssh/administrators_authorized_keys"
        Set-Content -Path $sshdCfg -Value $raw -Encoding ascii
        Write-Output "commented Match Group administrators in sshd_config"
        Restart-Service sshd
    }
}

$rules = @(
    @{ Name = "OpenSSH-Tailscale"; Remote = "100.64.0.0/10" },
    @{ Name = "OpenSSH-ZeroTier";  Remote = "10.147.17.0/24" },
    @{ Name = "OpenSSH-Campus";    Remote = @("128.62.0.0/16", "128.83.0.0/16", "10.0.0.0/8") }
)
foreach ($r in $rules) {
    Get-NetFirewallRule -DisplayName $r.Name -ErrorAction SilentlyContinue | Remove-NetFirewallRule
    New-NetFirewallRule -DisplayName $r.Name -Direction Inbound -Action Allow -Protocol TCP `
        -LocalPort 22 -RemoteAddress $r.Remote -Profile Any | Out-Null
}
Write-Output "SSH firewall rules ok"

if (Get-Command tailscale -ErrorAction SilentlyContinue) {
    tailscale up --unattended 2>&1 | Out-Null
    Write-Output "tailscale up"
} else {
    Write-Output "tailscale not installed"
}

Write-Output "--- addresses ---"
Get-NetIPAddress -AddressFamily IPv4 |
    Where-Object { $_.IPAddress -notlike "127.*" } |
    ForEach-Object { Write-Output ($_.InterfaceAlias + " " + $_.IPAddress) }
Write-Output "DONE sshd listening. From the laptop: ssh officepc / ssh officepc-campus"
