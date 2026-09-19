@echo off
cd /d "%~dp0\.."
set PHOTON_PASSWORD=laptop
set PHOTON_DATA_DIR=F:\lhr\data
set PHOTON_VIDEO_INGRESS_PORT=6200
set PHOTON_ON_TRACK=F:\lhr\_logs_extract\on_track
photon-server.exe -data-dir F:\lhr\data -video-ingress 6200
