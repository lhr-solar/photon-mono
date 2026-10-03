Write-Output "--- processes ---"
Get-Process photon-backend,photon-server,photon-index -ErrorAction SilentlyContinue |
  Format-Table Id, ProcessName, StartTime -AutoSize
Write-Output "--- listen 8765 ---"
Get-NetTCPConnection -LocalPort 8765 -State Listen -ErrorAction SilentlyContinue |
  Format-Table LocalAddress, LocalPort, OwningProcess -AutoSize
Write-Output "--- exe ---"
Get-Item F:\lhr\photon-server\photon-backend.exe -ErrorAction SilentlyContinue |
  Format-List FullName, Length, LastWriteTime
Write-Output "--- stderr tail ---"
Get-Content F:\lhr\photon-server\photon-backend-stderr.log -Tail 40 -ErrorAction SilentlyContinue
Write-Output "--- stdout tail ---"
Get-Content F:\lhr\photon-server\photon-backend-stdout.log -Tail 25 -ErrorAction SilentlyContinue
