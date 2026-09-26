$ErrorActionPreference = "Stop"
$openvpn = "C:\Users\alper\OpenVPN"
$canSrc = "F:\lhr\photon-server\scripts\photon-tunnel.ps1"
$vidSrc = "F:\lhr\photon-server\scripts\photon-video-tunnel.ps1"
if (Test-Path $openvpn) {
    Copy-Item -Force $canSrc (Join-Path $openvpn "photon-tunnel.ps1")
    Copy-Item -Force $vidSrc (Join-Path $openvpn "photon-video-tunnel.ps1")
}
Get-CimInstance Win32_Process -Filter "Name='ssh.exe'" | ForEach-Object {
    $cl = [string]$_.CommandLine
    if ($cl -match ":6100:127\.0\.0\.1:6100" -or
        $cl -match ":6600:127\.0\.0\.1:6600" -or
        $cl -match ":6200:127\.0\.0\.1:6200" -or
        $cl -match ":6700:127\.0\.0\.1:6700") {
        Write-Output ("stop ssh pid=" + $_.ProcessId)
        Stop-Process -Id $_.ProcessId -Force
    }
}
Get-CimInstance Win32_Process | ForEach-Object {
    $cl = [string]$_.CommandLine
    if ($cl -match "photon-tunnel\.ps1" -or $cl -match "photon-video-tunnel\.ps1") {
        Write-Output ("stop loop pid=" + $_.ProcessId)
        Stop-Process -Id $_.ProcessId -Force
    }
}
Start-Sleep -Seconds 2
function Start-Loop([string]$script) {
    $launch = "powershell.exe -NoProfile -WindowStyle Hidden -File `"$script`""
    $created = Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments @{
        CommandLine      = $launch
        CurrentDirectory = "C:\Users\alper"
    }
    Write-Output ("start " + [IO.Path]::GetFileName($script) + " wmi=" + $created.ReturnValue + " pid=" + $created.ProcessId)
}
$can = if (Test-Path (Join-Path $openvpn "photon-tunnel.ps1")) { Join-Path $openvpn "photon-tunnel.ps1" } else { $canSrc }
$vid = if (Test-Path (Join-Path $openvpn "photon-video-tunnel.ps1")) { Join-Path $openvpn "photon-video-tunnel.ps1" } else { $vidSrc }
Start-Loop $can
Start-Loop $vid
Start-Sleep -Seconds 4
Get-CimInstance Win32_Process -Filter "Name='ssh.exe'" | ForEach-Object {
    $cl = [string]$_.CommandLine
    if ($cl -match ":6100:|:6200:|:6700:") {
        Write-Output $cl
    }
}
