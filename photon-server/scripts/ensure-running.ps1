# Keep photon-server + photon-sshd alive. Never open a console window.
$ErrorActionPreference = "Continue"
$legacyDir = "F:\lhr\photon-server"
$log = Join-Path $legacyDir "scripts\ensure-running.log"
$runs = "F:\lhr\photon-mono\build\photon-server-can2.exe"
$plain = "F:\lhr\photon-mono\build\photon-server.exe"
$legacy = Join-Path $legacyDir "photon-server.exe"
function Write-Ensure([string]$msg) {
    $line = "{0} {1}" -f (Get-Date -Format "yyyy-MM-dd HH:mm:ss"), $msg
    try { Add-Content -Path $log -Value $line -ErrorAction SilentlyContinue } catch {}
}
function Test-PhotonServerUp {
    $running = Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.ProcessName -like "photon-server*" }
    if ($running) { return $true }
    return [bool](Get-NetTCPConnection -LocalPort 8080 -State Listen -ErrorAction SilentlyContinue)
}
function Start-Hidden([string]$command, [string]$workdir) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = "cmd.exe"
    $psi.Arguments = "/c " + $command.Substring($command.IndexOf("/c") + 3)
    if ($command -notlike "cmd.exe*") { $psi.FileName = $command; $psi.Arguments = "" }
    $psi.WorkingDirectory = $workdir
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $proc = [System.Diagnostics.Process]::Start($psi)
    return @{ ReturnValue = 0; ProcessId = $proc.Id }
}
function Start-PhotonServer {
    if (Test-PhotonServerUp) { return }
    $exe = @($runs, $plain, $legacy) | Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $exe) { Write-Ensure "no photon-server exe"; return }
    $wd = Split-Path $exe
    $cmd = "cmd.exe /c set PHOTON_PASSWORD=laptop&& set PHOTON_DATA_DIR=F:\lhr\data&& set PHOTON_VIDEO_INGRESS_PORT=6200&& `"$exe`" -data-dir F:\lhr\data -video-ingress 6200 1>>`"$legacyDir\photon-server-stdout.log`" 2>>`"$legacyDir\photon-server-stderr.log`""
    $p = Start-Hidden $cmd $wd
    Write-Ensure ("photon-server hidden=" + $p.ReturnValue + " pid=" + $p.ProcessId + " exe=" + $exe)
}
function Start-PhotonSshd {
    if (Get-Process photon-sshd -ErrorAction SilentlyContinue) { return }
    $exe = Join-Path $legacyDir "photon-sshd.exe"
    if (-not (Test-Path $exe)) { return }
    $out = Join-Path $legacyDir "scripts\sshd-boot.log"
    $err = Join-Path $legacyDir "scripts\sshd-boot.err"
    $launch = "cmd.exe /c set PHOTON_PASSWORD=laptop&& `"$exe`" -bind 127.0.0.1:2222 -keys `"$legacyDir\sshd\authorized_keys`" 1>>`"$out`" 2>>`"$err`""
    $created = Start-Hidden $launch $legacyDir
    Write-Ensure ("photon-sshd hidden=" + $created.ReturnValue + " pid=" + $created.ProcessId)
}
Write-Ensure "watchdog start"
while ($true) {
    try { Start-PhotonServer } catch { Write-Ensure ("server start err " + $_) }
    try { Start-PhotonSshd } catch { Write-Ensure ("sshd start err " + $_) }
    Start-Sleep -Seconds 15
}







