# Laptop UI bridge over UT VPN (office PC) with Oracle/MACBOOK fallback.
# Copy: C:\Users\alper\OpenVPN\campus-ui-tunnel.ps1
# Does not replace the car dest (still 150.136.85.207).
$ErrorActionPreference = "Continue"
$ssh = "$env:SystemRoot\System32\OpenSSH\ssh.exe"
if (-not (Test-Path $ssh)) { $ssh = "ssh" }
$logDir = "C:\Users\alper\OpenVPN\log"
New-Item -ItemType Directory -Force -Path $logDir | Out-Null
$log = Join-Path $logDir "campus-ui-tunnel.log"
$pidFile = Join-Path $logDir "campus-ui-tunnel.pid"
if (Test-Path $pidFile) {
    $old = 0
    [void][int]::TryParse((Get-Content $pidFile | Select-Object -First 1), [ref]$old)
    if ($old -gt 0 -and (Get-Process -Id $old -ErrorAction SilentlyContinue)) {
        Write-Output "already running pid=$old"
        exit 0
    }
}
Set-Content -Path $pidFile -Value $PID

function Write-Log([string]$msg) {
    $line = "{0} {1}" -f (Get-Date -Format "yyyy-MM-dd HH:mm:ss"), $msg
    Add-Content -Path $log -Value $line -ErrorAction SilentlyContinue
    Write-Output $line
}

function Test-Tcp([string]$hostName, [int]$port, [int]$ms = 800) {
    $c = New-Object System.Net.Sockets.TcpClient
    try {
        $iar = $c.BeginConnect($hostName, $port, $null, $null)
        if (-not $iar.AsyncWaitHandle.WaitOne($ms, $false)) {
            return $false
        }
        return $c.Connected
    } catch {
        return $false
    } finally {
        try { $c.Close() } catch {}
    }
}

function Test-Cisco {
    $a = Get-NetAdapter -Name "Ethernet 2" -ErrorAction SilentlyContinue
    return [bool]($a -and $a.Status -eq "Up")
}

function Pick-Host {
    if ((Test-Cisco) -and (Test-Tcp "128.62.19.230" 22 900)) {
        return @{ Name = "officepc-campus"; Label = "LHRSolarOfficePC via UT VPN" }
    }
    if (Test-Tcp "150.136.85.207" 22 900) {
        return @{ Name = "photon-wg"; Label = "MACBOOK via Oracle WireGuard 10.66.0.2" }
    }
    if (Test-Tcp "150.136.85.207" 2222 900) {
        return @{ Name = "photon-macbook"; Label = "MACBOOK via Oracle :2222" }
    }
    return $null
}

Write-Log "campus-ui-tunnel start"
while ($true) {
    Write-Log ("cisco=" + (Test-Cisco) + " probing officepc:22, photon-wg, oracle:2222")
    $h = Pick-Host
    if (-not $h) {
        if (-not (Test-Cisco)) {
            Write-Log "waiting: connect Cisco Secure Client to vpn.utexas.edu"
        } else {
            Write-Log "waiting: officepc :22, photon-wg, and Oracle :2222 all closed"
        }
        Start-Sleep -Seconds 5
        continue
    }
    Write-Log ("bridging UI via " + $h.Label + " -> http://127.0.0.1:8080/photon.html")
    & $ssh -N -o BatchMode=yes -o IdentitiesOnly=yes -o ExitOnForwardFailure=yes `
        -o ServerAliveInterval=30 -o ServerAliveCountMax=3 -o StrictHostKeyChecking=accept-new `
        -L 127.0.0.1:8080:127.0.0.1:8080 `
        -L 127.0.0.1:8765:127.0.0.1:8765 `
        $h.Name
    Write-Log ("ssh exited " + $LASTEXITCODE + "; retry")
    Start-Sleep -Seconds 3
}
