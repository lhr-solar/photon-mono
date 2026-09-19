# photon-next

Rewrite of Longhorn Racing Solar's Photon telemetry stack. A native daemon
(`photon-backend`) owns sources, DBC decode, and storage; an ImGui client —
WASM page or native `photon-ui` — speaks protocol v1 over a localhost
WebSocket.

```
sources ─► daemon ─► WebSocket 127.0.0.1:8765 ─► ImGui client
(daq_tcp / pcan / log)                          (photon.html / photon-ui)
```

## Systems (`photon/<name>`)

`base` json/endian · `wire` protocol v1/canp · `decode` dbc · `store` tsdb/vsd1/video
· `net` sockets/websocket · `hw` pcan · `synth` tracksim/analysis · `daemon`
photon-backend/photon-index · `ui` photon-ui/photon.html

## Build

```powershell
cmake -S . -B .artifacts -G Ninja -D CMAKE_C_COMPILER=clang -D CMAKE_CXX_COMPILER=clang++
cmake --build .artifacts
ctest --test-dir .artifacts --output-on-failure
```

WASM page: `.\web\scripts\build.ps1`

## Run

The PC console is started by photon-server. On a laptop:

```powershell
.\.artifacts\photon\daemon\photon-backend.exe --no-live-tail --daq-host <pc-ip>
.\web\scripts\serve.ps1     # or: .\.artifacts\photon-ui.exe --ws ws://127.0.0.1:8765
```

## Docs

- [docs/protocol.md](docs/protocol.md) — wire format, control plane, sources, track-sim
- [docs/build.md](docs/build.md) — binaries, flags, emsdk pins, screens
- per-system `README.md` under `photon/<name>`
