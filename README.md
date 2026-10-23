# Photon

Two programs. `photon-server` is the ingest server. `photon-next` is the UI and the daemon that feeds it. Run either one, and name what the UI is reading.

```text
photon build
photon server
photon server ingest PORT
photon ui local
photon ui daq HOST[:PORT]
photon ui oracle
photon ui pcan
photon ui log PATH
photon station
```

`server` is ingest only. The DAQ dials port `6100`. `photon server ingest PORT` moves that port. `ui local` reads the server on this machine (`127.0.0.1:6500`, video `:6700`). `ui daq` reads a CANP fan-out; the port defaults to 6500, and the public Oracle fan-out is `150.136.85.207:6110`. `ui oracle` is that public pipe. `ui pcan` reads a USB PCAN. `ui log` replays a VSD1 log. `station` runs the server and the UI together, with the UI on this machine's fan-out. `photon station ingest PORT` moves the DAQ port the same way.

`photon build` writes `photon-server.exe`, `photon-backend.exe`, `photon-index.exe`, `photon-ui.exe`, and one `assets\` tree into `build\`. The server only looks next to that executable. The DBC source is `photon-next\assets\dbc`. The page, from `photon-next\web\scripts\build.ps1`, is copied to `build\ui\photon.html`.

`PHOTON_PASSWORD` defaults to `laptop`. `PHOTON_DATA_DIR` defaults to `data\` in this repo. Flags after the use case are passed through to `photon-server` or `photon-backend`.

`Photon/` stays its own repo. That tree is the native desktop client and the embedded sharepoint.

InfluxDB 3 Core and Grafana are installed programs on the PC. They bind to `127.0.0.1`. Dashboard JSON for those panels belongs in `dashboards/`.

## Live checkouts

`F:\lhr\photon-server` and `F:\lhr\photon-next` are still the running trees. MacBook ingest has not moved.
