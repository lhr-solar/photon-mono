#!/bin/sh
go build main.go client.go metadata.go store.go
sudo systemctl restart daq_server.service
sudo systemctl status daq_server.service
