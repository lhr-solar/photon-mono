# Photon ground station

One repo for the shop PC. That machine runs the Go server and the view together.

- `photon-server` is the Go server: CAN ingest, the CAN log, the video store, the loopback UI host, the ops scripts, and the Oracle relay.
- `photon-next` is the view and the daemon it talks to: plots, video, 3D, the Grafana host, and the web shell.
- `grok-analyst` is the analysis sidecar. It binds to loopback only.

`Photon/` stays its own repo. That tree is the native desktop client and the embedded sharepoint.

InfluxDB 3 Core and Grafana are installed programs on the PC. They bind to `127.0.0.1`. Dashboard JSON for those panels belongs in `dashboards/`.

Each half still has its own `assets/dbc` copy. The builds look up those paths today.

## Build

From `photon-server`:

```powershell
go test ./analyst ./observer
```

From `photon-next`, the native daemon and the WASM page (details in `photon-next/README.md`):

```powershell
cmake -S . -B .artifacts -G Ninja -D CMAKE_C_COMPILER=clang -D CMAKE_CXX_COMPILER=clang++
cmake --build .artifacts
.\web\scripts\build.ps1
```

From `grok-analyst`:

```powershell
go test ./...
```

The server looks for `photon.html` in `photon-next/.artifacts-web`, one directory above the `photon-server` binary. This layout keeps that relative path.

## Live checkouts

`F:\lhr\photon-server` and `F:\lhr\photon-next` are still the running trees. MacBook ingest has not moved.
