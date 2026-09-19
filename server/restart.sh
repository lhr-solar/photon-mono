#!/bin/sh
set -e

echo "Building photon-server..."
go build -o photon-server ./server

if systemctl is-active --quiet photon-server.service 2>/dev/null; then
    echo "Restarting photon-server.service..."
    sudo systemctl restart photon-server.service
    sudo systemctl status photon-server.service
elif systemctl is-active --quiet daq_server.service 2>/dev/null; then
    echo "Restarting legacy daq_server.service..."
    sudo systemctl restart daq_server.service
    sudo systemctl status daq_server.service
else
    echo "Build completed: ./photon-server"
fi
