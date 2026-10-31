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
       WASM:   photon.html (web-release preset)
       native: photon-ui.exe (desktop-* presets)
```

`photon-server` is the ingest server on the PC. Oracle is a pipe to it. Laptop clients connect to the PC fan-out (`:6500` CANP, `:6700` video). `photon-index --live-fanout` writes `data/tsdb` on the PC.

## Layout

| Path | Role |
| --- | --- |
| `photon/base` | JSON, endian |
| `photon/wire` | protocol v1, control plane, CANP, video framing |
| `photon/decode` | DBC parse/decode, interp |
| `photon/store` | TSDB, VSD1, video index, log sessions |
| `photon/net` | sockets, WebSocket, client auth |
| `photon/hw` | PCAN (native only) |
| `photon/synth` | track-sim solver, analysis |
| `photon/daemon` | `photon-backend`, `photon-index` |
| `photon/ui` | ImGui client (`photon-ui` / `photon.html`) |
| `web/` | shell + Emscripten build/serve scripts |
| `tools/` | `photon-export`, QA, log analysis |
| `../tests/native/` | CTest (`photon-tests`, `photon-export-tests`); all the repo's tests are under `../tests/` |
| `assets/` | DBC, fonts, models, tracks |

## Build

In the monorepo, `photon build` at the repo root builds this and assembles the runnable bundles in `out\dist` (see the root README). Underneath it are the presets in `CMakePresets.json`, which also work on their own from this folder. Windows host with clang, Ninja, CMake 3.25+:

```powershell
cmake --preset desktop-debug
cmake --build --preset desktop-debug
```

| Preset | Builds | Tree |
| --- | --- | --- |
| `headless-debug`, `headless-release` | `photon-backend`, `photon-index`, `photon-export`, tests; no SDL, ImGui, or WebView2 | `..\out\build\headless-<config>` |
| `desktop-debug`, `desktop-release` | all of that plus `photon-ui` | `..\out\build\desktop-<config>` |
| `web-release` | the WASM page | `..\out\build\web-release` |

The WASM page needs emsdk loaded first, so build it from the repo root with `photon build client --ui web` (`photon setup client --ui web` installs emsdk once).

In a tree, binaries are at `photon\daemon\photon-backend.exe`, `photon\daemon\photon-index.exe`, `photon-ui.exe`, and `photon.html`. Run from a tree, they read `assets\` from this checkout. Install components (`server`, `client`, `desktop`, `web`, `tools`, `symbols`) copy each program with the assets it reads, for running anywhere: `cmake --install ..\out\build\desktop-debug --component client --prefix <dir>`.

## Run

The PC console is started by photon-server. On a laptop, `photon client <pc-ip>` from the repo root runs the client bundle. From a tree (or any other source: `--source pcan`):

```powershell
..\out\build\desktop-debug\photon\daemon\photon-backend.exe --no-live-tail --daq-host <pc-ip>
.\web\scripts\serve.ps1
```

Open **http://127.0.0.1:8080/photon.html**. Do not open `photon.html` as `file://`. Same UI as a desktop exe: `..\out\build\desktop-debug\photon-ui.exe --ws ws://127.0.0.1:8765`.

`photon-backend -h` lists the remaining flags (ws port, source, password). You should not need them; the defaults are the deployed topology.

## Tests

```powershell
ctest --preset desktop-debug
```

`photon test` at the repo root runs these, the Go server tests, and the end-to-end tests against the bundles.
