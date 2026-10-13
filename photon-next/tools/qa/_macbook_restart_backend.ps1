$dest = "F:\lhr\photon-mono\build"
Get-Process photon-backend -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 400
$cmd = "cmd.exe"
$args = "/c start `"photon-backend`" /min photon-backend.exe --no-live-tail --source daq_tcp --daq-host 127.0.0.1 --daq-port 6500 --video-host 127.0.0.1 --video-port 6700 --video-udp-port 6800 --video-udp-host 10.66.0.1 --tsdb-dir F:\lhr\data\tsdb --ws-port 8765 --assets-dir F:\lhr\photon-mono\build\assets --log-path F:\lhr\_logs_extract\on_track --password laptop"
$p = ([wmiclass]"Win32_Process").Create($cmd + " " + $args, $dest)
Write-Output ("wmi=" + $p.ReturnValue + " pid=" + $p.ProcessId)
Start-Sleep -Seconds 2
Get-Process photon-backend -ErrorAction SilentlyContinue | Format-List Id, StartTime
Get-NetTCPConnection -LocalPort 8765 -State Listen -ErrorAction SilentlyContinue | Format-Table OwningProcess, LocalAddress -AutoSize
