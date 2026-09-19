# Keep photon-server + photon-sshd alive across boot and crashes.
# Scheduled task: photon-server (AtStartup SYSTEM + AtLogOn). Do not use
# run-desktop.cmd — that process dies with the logon job / SSH session.
$ErrorActionPreference = "Continue"
$dest = "F:\lhr\photon-server"
$log = Join-Path $dest "scripts\ensure-running.log"

function Write-Ensure([string]$msg) {
    $line = "{0} {1}" -f (Get-Date -Format "yyyy-MM-dd HH:mm:ss"), $msg
    try { Add-Content -Path $log -Value $line -ErrorAction SilentlyContinue } catch {}
}

function Wait-Repo {
    $deadline = (Get-Date).AddMinutes(10)
    while ((Get-Date) -lt $deadline) {
        if (Test-Path (Join-Path $dest "photon-server.exe")) { return $true }
        Start-Sleep -Seconds 5
    }
    return $false
}

function Start-PhotonServer {
    if (Get-Process photon-server -ErrorAction SilentlyContinue) { return }
    $cmd = "cmd.exe"
    $args = "/c set PHOTON_PASSWORD=laptop&& set PHOTON_DATA_DIR=F:\lhr\data&& set PHOTON_VIDEO_INGRESS_PORT=6200&& set PHOTON_ON_TRACK=F:\lhr\_logs_extract\on_track&& photon-server.exe -data-dir F:\lhr\data -video-ingress 6200 1>>photon-server-stdout.log 2>>photon-server-stderr.log"
    $p = ([wmiclass]"Win32_Process").Create($cmd + " " + $args, $dest)
    Write-Ensure ("photon-server wmi=" + $p.ReturnValue + " pid=" + $p.ProcessId)
}

function Start-PhotonSshd {
    if (Get-Process photon-sshd -ErrorAction SilentlyContinue) { return }
    $exe = Join-Path $dest "photon-sshd.exe"
    if (-not (Test-Path $exe)) { return }
    $out = Join-Path $dest "scripts\sshd-boot.log"
    $err = Join-Path $dest "scripts\sshd-boot.err"
    $launch = "cmd.exe /c set PHOTON_PASSWORD=laptop&& `"$exe`" -bind 127.0.0.1:2222 -keys `"$dest\sshd\authorized_keys`" 1>>`"$out`" 2>>`"$err`""
    $created = Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments @{
        CommandLine = $launch
        CurrentDirectory = $dest
    }
    Write-Ensure ("photon-sshd wmi=" + $created.ReturnValue + " pid=" + $created.ProcessId)
}

if (-not (Wait-Repo)) {
    Write-Ensure "F:\lhr\photon-server.exe missing; giving up this pass"
    exit 1
}

Write-Ensure "watchdog start"
while ($true) {
    try { Start-PhotonServer } catch { Write-Ensure ("server start err " + $_) }
    try { Start-PhotonSshd } catch { Write-Ensure ("sshd start err " + $_) }
    Start-Sleep -Seconds 10
}
