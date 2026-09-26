@echo off
REM One-click Photon UI after you connect Cisco AnyConnect (vpn.utexas.edu).
REM Copies the campus UI bridge into OpenVPN and opens local photon.html.
setlocal
set SRC=%~dp0campus-ui-tunnel.ps1
set DST=C:\Users\alper\OpenVPN\campus-ui-tunnel.ps1
set LOGDIR=C:\Users\alper\OpenVPN\log
if not exist "%LOGDIR%" mkdir "%LOGDIR%"
copy /Y "%SRC%" "%DST%" >nul
echo copied campus-ui-tunnel.ps1
start "photon-campus-ui" /min powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%DST%"
timeout /t 2 /nobreak >nul
start "" http://127.0.0.1:8080/photon.html
echo Photon UI: http://127.0.0.1:8080/photon.html
echo Stay on vpn.utexas.edu. Shop PC SSH is tried first; Oracle :2222 is fallback.
endlocal
