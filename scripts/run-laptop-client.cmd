@echo off
cd /d "%~dp0\.."
set PHOTON_PASSWORD=laptop
set UIDIR=%~dp0..\..\photon-next\.artifacts-web
if not exist "%UIDIR%\photon.html" set UIDIR=%~dp0..\..\photon-next
start "photon-ui" /min python -m http.server 8080 --bind 127.0.0.1 --directory "%UIDIR%"
photon-backend.exe --no-live-tail --source daq_tcp --daq-host 150.136.85.207 --daq-port 6110 --video-host 150.136.85.207 --video-port 6700 --video-udp-port 6800 --ws-port 8765 --password laptop --assets-dir "%cd%\assets"
