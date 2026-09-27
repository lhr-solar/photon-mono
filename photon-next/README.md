# photon-next

photon-next is the client stack: a native daemon (`photon-backend`) owns sources, DBC decode, and local storage; a Dear ImGui/ImPlot UI (WASM page or native `photon-ui`) talks to that daemon over a localhost WebSocket. The existing native client (`Photon`) is not modified. The Go ingest server (`photon-server`) owns Saturday storage and starts `photon-index --live-fanout` on the PC.

On the **PC**, photon-server starts this UI itself (`photon-backend` + `http://127.0.0.1:8080/photon.html`) aimed at loopback fan-out. The **laptop** runs its own `photon-backend` for a different source (`--daq-host <pc>` or `--source pcan`). Both clients pass `--no-live-tail`. Wire format is protocol v1: [docs/protocol.md](docs/protocol.md).

## Architecture

```
PCAN / DAQ TCP / log replay
                │
                ▼
     photon-backend (photon/daemon)
       sockets, CANP, sources,
       TX arm stub, protocol v1
                │  WebSocket 127.0.0.1:8765
                ▼
     ImGui UI  photon/ui
       WASM:   .artifacts-web/photon.html
       native: .artifacts/photon-ui.exe
```

`photon-server` is the ingest server on the PC. Oracle is a pipe to it. Laptop clients connect to the PC fan-out (`:6500` CANP, `:6700` video). `photon-index --live-fanout` writes `data/tsdb` on the PC.

## Layout

| Path | Role |
| --- | --- |
| `photon/core` | Protocol, DBC, logs, video index, sockets, PCAN, track-sim |
| `photon/daemon` | `photon-backend`, `photon-index` |
| `photon/ui` | ImGui client (`photon-ui` / `photon.html`) |
| `web/` | shell + Emscripten build/serve scripts |
| `tools/` | `photon-export`, QA, log analysis |
| `tests/` | CTest (`photon-tests`) |
| `assets/` | DBC, fonts, models, tracks |

## Build

Windows host with clang, Ninja, CMake 3.25+:

```powershell
cmake -S . -B .artifacts -G Ninja -D CMAKE_C_COMPILER=clang -D CMAKE_CXX_COMPILER=clang++
cmake --build .artifacts
```

WASM page (emsdk once via `.\web\scripts\setup-emsdk.ps1`):

```powershell
.\web\scripts\build.ps1
```

Binaries land at `.artifacts/photon/daemon/photon-backend.exe`, `.artifacts/photon-ui.exe`, and `.artifacts-web/photon.html`. Do not mix the native `.artifacts` tree with the WASM configure.

## Run

The PC console is started by photon-server. On a laptop (or any other source: `--source pcan`):

```powershell
.\.artifacts\photon\daemon\photon-backend.exe --no-live-tail --daq-host <pc-ip>
.\web\scripts\serve.ps1
```

Open **http://127.0.0.1:8080/photon.html**. Do not open `photon.html` as `file://`. Same UI as a desktop exe: `.artifacts\photon-ui.exe --ws ws://127.0.0.1:8765`.

`photon-backend -h` lists the remaining flags (ws port, source, password). You should not need them; the defaults are the deployed topology.

## Tests

```powershell
ctest --test-dir .artifacts --output-on-failure
```
