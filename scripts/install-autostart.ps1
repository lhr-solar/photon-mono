# Replace the photon-server scheduled task. The old one pointed at
# run-desktop.cmd with no trigger, so a reboot left ingest down.
$ErrorActionPreference = "Stop"
$taskName = "photon-server"
$script = "F:\lhr\photon-server\scripts\ensure-running.ps1"
if (-not (Test-Path $script)) { throw "missing $script" }

$existing = Get-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue
if ($existing) {
    Unregister-ScheduledTask -TaskName $taskName -Confirm:$false
    Write-Output "removed old $taskName"
}

$action = New-ScheduledTaskAction -Execute "powershell.exe" `
    -Argument "-NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File `"$script`"" `
    -WorkingDirectory "F:\lhr\photon-server"

$startup = New-ScheduledTaskTrigger -AtStartup
$logon = New-ScheduledTaskTrigger -AtLogOn -User "MACBOOK\alper"
$settings = New-ScheduledTaskSettingsSet `
    -ExecutionTimeLimit ([TimeSpan]::Zero) `
    -RestartCount 3 `
    -RestartInterval (New-TimeSpan -Minutes 1) `
    -AllowStartIfOnBatteries `
    -DontStopIfGoingOnBatteries `
    -StartWhenAvailable `
    -MultipleInstances IgnoreNew

$systemOk = $false
try {
    $principal = New-ScheduledTaskPrincipal -UserId "SYSTEM" -LogonType ServiceAccount -RunLevel Highest
    Register-ScheduledTask -TaskName $taskName -Action $action -Trigger @($startup, $logon) `
        -Principal $principal -Settings $settings `
        -Description "WMI-start photon-server + photon-sshd at boot; restart if they die." | Out-Null
    $systemOk = $true
    Write-Output "registered $taskName as SYSTEM AtStartup+AtLogOn"
} catch {
    Write-Output ("SYSTEM register failed: " + $_.Exception.Message)
}

if (-not $systemOk) {
    $principal = New-ScheduledTaskPrincipal -UserId "MACBOOK\alper" -LogonType Interactive -RunLevel Highest
    Register-ScheduledTask -TaskName $taskName -Action $action -Trigger @($startup, $logon) `
        -Principal $principal -Settings $settings `
        -Description "WMI-start photon-server + photon-sshd at logon/startup; restart if they die." | Out-Null
    Write-Output "registered $taskName as MACBOOK\alper AtStartup+AtLogOn"
}

Start-ScheduledTask -TaskName $taskName
Start-Sleep -Seconds 2
$t = Get-ScheduledTask -TaskName $taskName
Write-Output ("state=" + $t.State)
Write-Output ("actions=" + ($t.Actions | ForEach-Object { $_.Execute + " " + $_.Arguments }))
Write-Output ("triggers=" + @($t.Triggers | ForEach-Object { $_.CimClass.CimClassName + " enabled=" + $_.Enabled }).Count)
$t.Triggers | ForEach-Object { Write-Output ("trigger " + $_.CimClass.CimClassName + " enabled=" + $_.Enabled) }
Write-Output ("principal=" + $t.Principal.UserId + " logon=" + $t.Principal.LogonType + " runlevel=" + $t.Principal.RunLevel)
Get-Process powershell -ErrorAction SilentlyContinue |
    Where-Object { $_.CommandLine -match "ensure-running" } |
    ForEach-Object { "watchdog pid=" + $_.Id }
# CommandLine is not on Get-Process; use CIM
Get-CimInstance Win32_Process -Filter "Name='powershell.exe'" |
    Where-Object { $_.CommandLine -and $_.CommandLine -like "*ensure-running.ps1*" } |
    ForEach-Object { Write-Output ("watchdog pid=" + $_.ProcessId) }
