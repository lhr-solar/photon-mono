# photon-next

Rewrite of Longhorn Racing Solar's Photon telemetry stack: a native backend daemon owns hardware, network, files, DBC decode, arena storage, and TX safety. A Dear ImGui/ImPlot UI (WASM page or native `photon-ui.exe`) talks to that daemon over localhost WebSocket.

This repository is organized as Photon-style **system libraries** under `photon/`. The existing native client (`F:\lhr\Photon`) is not modified. The Go ingest server (`F:\lhr\photon-server`) owns Saturday storage and starts `photon-index --live-fanout` on the PC.

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
       WASM: .artifacts-web/photon.html
       native: .artifacts/photon-ui.exe
```

`photon-server` is the ingest server on the **PC**. Oracle is a pipe to it. On the PC it also starts a **local UI** (`photon-backend` + `http://127.0.0.1:8080/photon.html`) aimed at its own fan-out — that is the console when you are not on USB PCAN or another host. The **laptop** runs its own `photon-backend` for a different source (`--daq-host <pc>` or `--source pcan`). Both clients pass `--no-live-tail`; `photon-index --live-fanout` writes `data/tsdb` on the PC.

## Layout

| Path | Library / target | Role |
| --- | --- | --- |
| `photon/base` | `photon_base` | JSON, endian (shared primitives) |
| `photon/wire` | `photon_wire` | protocol v1, control plane, CANP, video framing |
| `photon/decode` | `photon_decode` | DBC parse/decode, interp |
| `photon/store` | `photon_store` | TSDB, VSD1, video index, log sessions |
| `photon/net` | `photon_net` | sockets, WebSocket, SHA-1/Base64, client auth (`net.cpp` / `websocket.cpp` native-only) |
| `photon/hw` | `photon_hw` | PCAN (native-only) |
| `photon/synth` | `photon_synth` | track-sim solver |
| `photon/daemon` | `photon_daemon` + `photon-backend` + `photon-index` | native daemon and indexer |
| `photon/ui` | sources + `photon-ui` / `photon-frontend` | ImGui client (native exe / WASM `photon.html`) |
| `photon-core` | INTERFACE umbrella | links the system libs (except `photon_ui`) |
| `web/` | — | `shell.html` + Emscripten build/serve scripts |
| `tools/` | `photon-export` (+ lib), `qa/`, `data/`, `charge_model/` | export, protocol/browser QA, log analysis |
| `tests/` | `photon-tests` | CTest; links `photon-core`, `photon_daemon`, `photon-export-lib` |
| `assets/` | — | `dbc/`, `fonts/`, `models/`, `tracks/`, `maps3d.html`+`js` |
| `.artifacts/` | — | native build directory (same convention as Photon) |
| `.artifacts-web/` | — | Emscripten build directory (`photon.html` / `.js` / `.wasm`) |
| `.external/` | — | Fetched emsdk, imgui, implot (gitignored) |

Dependency edges: `base` → (none); `synth` → `base`; `wire` → `base`, `synth`; `decode` → `base`; `store` → `decode`, `wire`, `base`, `net` (SHA-1); `net` → `base`, `wire`; `hw` → `wire`, `base`; `daemon` → everything except `ui`; `ui` → `photon-core` + imgui/tinygltf (+ `net` native).

## Build

Windows host with clang, Ninja, CMake 3.25+:

```powershell
cmake -S . -B .artifacts -G Ninja -D CMAKE_C_COMPILER=clang -D CMAKE_CXX_COMPILER=clang++
cmake --build .artifacts
ctest --test-dir .artifacts --output-on-failure
```

Ninja multi-folder layout:

| Binary | Path |
| --- | --- |
| `photon-backend` | `.artifacts/photon/daemon/photon-backend.exe` |
| `photon-index` | `.artifacts/photon/daemon/photon-index.exe` |
| `photon-ui` | `.artifacts/photon-ui.exe` |
| `photon-export` | `.artifacts/tools/photon-export/photon-export.exe` |
| `photon-tests` | `.artifacts/tests/photon-tests.exe` |

Optional: `cmake --build .artifacts --target format` / `format-check` (needs `clang-format`; skipped if missing).

## Frontend (ImGui WASM)

Host: Windows + clang/Ninja for the native backend; Emscripten 6.x for the page.

Pinned third-party (FetchContent into `.external/`):

| Dep | Pin | Notes |
| --- | --- | --- |
| emsdk | `latest` at install → **6.0.7** (`a2e0030…`) | Local clone `.external/emsdk` |
| Dear ImGui | `v1.92.9-docking` | Docking branch tag |
| ImPlot | `v0.17` | ImGui 1.92-compatible |
| Window/GL | Emscripten `--use-port=sdl2` + WebGL2 / GLES3 | Simplest current emsdk port |

### Install emsdk (once)

```powershell
git clone --depth 1 https://github.com/emscripten-core/emsdk.git .external/emsdk
cd .external/emsdk
.\emsdk install latest
.\emsdk activate latest
cd ..\..
```

Or: `.\web\scripts\setup-emsdk.ps1`

### Build the page

```powershell
.\web\scripts\build.ps1
```

Equivalent manual steps:

```powershell
. .\.external\emsdk\emsdk_env.ps1
emcmake cmake -S . -B .artifacts-web -G Ninja -DPHOTON_FRONTEND=ON -DBUILD_TESTING=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build .artifacts-web
```

Outputs: `.artifacts-web/photon.html`, `.artifacts-web/photon.js`, `.artifacts-web/photon.wasm`, `.artifacts-web/photon.data` (preloaded fonts).
Do **not** mix this tree with the native `.artifacts/` configure (`PHOTON_FRONTEND` defaults OFF).

### Run against photon-server (daq_tcp)

On the **PC**, `photon-server` starts this UI itself (`http://127.0.0.1:8080/photon.html`). You only start these two processes by hand on a **laptop** (or any other source: `--source pcan`).

```powershell
.\.artifacts\photon\daemon\photon-backend.exe --ws-port 8765 --no-live-tail --daq-host <pc-ip>
.\web\scripts\serve.ps1
```

Open **http://127.0.0.1:8080/photon.html**. Do not open `photon.html` as `file://`. The UI sends `hello`, then follows the backend's active source. Heartbeat is every 1 s; reconnect backoff is 200 ms / 500 ms / 1 s / 2 s.

Optional: `emrun .artifacts-web/photon.html --no_browser` (emrun is on PATH after `emsdk_env.ps1`).

Headless protocol check (backend must be running):

```powershell
python .\web\scripts\smoke_ws.py
```

### Screens

| Tab | Status |
| --- | --- |
| Pit (default) | Vitals tiles, 32-cell heatmap, alarm strip, 3D + Live View dock tiles |
| Dashboard | Extra pit/debug tiles |
| Plots | ImPlot time-series, catalog checkboxes, live window, pause + zoom |
| Dynamics | MPH / steer / G / wheel-slip tiles + G-G and slip plots |
| Live View | WebCodecs H.264 → GLES texture |
| 3D | WebGL2 glTF car, telemetry-driven wheels / steer / suspension |
| Track Sim | Catalog + solve + speed/elevation plots |
| CAN | Live `CanMonitorBatch` table (id, dlc, hex, Hz, count), sortable |
| Network | Source picker + control-plane log |
| Analysis | Workbook / compute overlay |

Navigation is a **top tab bar** only (no sidebar). Titlebar right: connection dot, source, timeline mode/cursor, Live/Pause transport.

## Native UI (`photon-ui`)

Same ImGui sources as the WASM page, built as `.artifacts/photon-ui.exe` when `PHOTON_NATIVE_UI` is ON (default). Speaks protocol v1 to `photon-backend` (`--ws`, `--password`, `--shared`/`--private`, `--screen`). `--quit-after-ms` is the smoke-test hook.

## Indexer (`photon-index`)

Native-only, built next to the daemon. Historical path: `--logs DIR --out DIR` indexes VSD1 into a TSDB session. Live PC ingest: `--live-fanout HOST:PORT --out DIR` writes `data/tsdb` from photon-server CANP fan-out. Also hosts `--bench` / `--retention` helpers; see `--help`.

## Export (`photon-export`)

Reads a TSDB session and writes Parquet (`--store DIR --session ID --out FILE --signals LIST` or `--all-signals`). Library is `photon-export-lib` (also linked by the daemon CSV path and tests).

## Run

```powershell
# Laptop / other source. The PC UI is started by photon-server.
.\.artifacts\photon\daemon\photon-backend.exe --no-live-tail --daq-host <pc-ip>
```

Flags:

| Flag | Default | Meaning |
| --- | --- | --- |
| `--ws-port` | `8765` | WebSocket listen port (always bound to `127.0.0.1`) |
| `--daq-host` | `127.0.0.1` | Default CANP fan-out host for `select_source daq_tcp` |
| `--daq-port` | `6500` | Default CANP fan-out port |
| `--source` | `daq_tcp` | `daq_tcp` (photon-server), `log_replay`, or `pcan` |
| `--no-live-tail` | off | Do not write a TSDB (PC `photon-index` owns the store) |
| `--password` | `PHOTON_PASSWORD` | Shared secret for photon-server fan-out / video / observer |

Connect a WebSocket client, send a JSON `hello`, then `select_source` / `subscribe_signals`. Binary frames are photon-wire v1.

## JSON dependency

Control-plane JSON is an in-repo recursive-descent parser/stringifier (`photon/base` `json.cpp`): objects, arrays, strings, numbers, bools, null. No third-party JSON library and no link-time dependency. SHA-1 and Base64 used for the WebSocket handshake are also in-repo (`photon/net` `sha1.cpp`, `base64.cpp`).

## Protocol v1

Every binary WebSocket payload is a **12-byte little-endian header** plus `payloadLen` bytes:

| Offset | Type | Name | Notes |
| --- | --- | --- | --- |
| 0 | `uint16` | `magic` | `0x5057` (`"PW"`) |
| 2 | `uint8` | `version` | `1` |
| 3 | `uint8` | `msgType` | see table |
| 4 | `uint32` | `sequence` | monotonically increasing per daemon |
| 8 | `uint32` | `payloadLen` | bytes after header |

### Binary message types

| `msgType` | Name | Payload |
| --- | --- | --- |
| `0x01` | `TelemetryBatch` | Repeated blocks: `uint32 signalId`, `uint16 sampleCount`, `uint16 reserved`, then `double timestamps[sampleCount]`, `double values[sampleCount]`. Timestamps are UTC milliseconds as IEEE-754 float64. |
| `0x02` | `CanMonitorBatch` | Repeated 24-byte items: `uint32 canId`, `uint8 dlc`, `uint8 flags`, `uint16 reserved`, `uint64 timestampMs`, `uint8 data[8]`. `flags` bit 0 reserved for 29-bit extended IDs. |
| `0x04` | `VideoFrame` | `uint64 timestampMs`, `uint8 flags` (bit 0 keyframe, bit 1 SPS/PPS config), `uint8 reserved[3]`, then an Annex-B H.264 NAL (including the `00 00 00 01` start code). |
| `0x05` | `TimelineStatus` | 33 bytes: `uint8 mode`, `uint64 cursorMs`, `minTimeMs`, `maxTimeMs`, `totalBatches`. Mode: `1` pause, `2` play, `3` live. |

`0x03` is reserved.

JSON **text** frames are the control plane (`type` field):

| `type` | Direction | Fields |
| --- | --- | --- |
| `hello` | UI → backend | `protocol_version` (must be 1), optional `client` |
| `hello_ack` | backend → UI | `protocol_version`, `server`, `features`, `dbc` `{id, path, bundled[]}` |
| `select_source` | UI → backend | `source`: `pcan` \| `daq_tcp` `{host,port}` \| `log_replay` `{path}` |
| `select_dbc` | UI → backend | `path` (filesystem) **or** bundled `id` / `dbc_id` (stem under `assets/dbc/`). Network screen; no restart. |
| `signal_catalog` | backend → UI | `signals`, `dbc_id` — after `hello_ack` and on DBC change |
| `subscribe_signals` | UI → backend | `signal_ids` |
| `unsubscribe_signals` | UI → backend | `signal_ids` |
| `timeline_control` | UI → backend | `mode`, `cursor_ms`, optional `speed` (replay multiplier, default 1) |
| `tx_arm_request` | UI → backend | `armed` |
| `tx_arm_status` | backend → UI | `armed`, `reason` |
| `error_alert` | backend → UI | `code`, `message` |
| `heartbeat` | UI → backend | refreshes TX watchdog |

TX arm is a stub: per-client armed flag, auto-disarm on disconnect or 3 s without a control message.

### Sources (this milestone)

- **daq_tcp** (default): TCP client of photon-server CANP fan-out (`:6500`). Parses CANP v3, forwards frames as `CanMonitorBatch`, and decodes subscribed DBC signals into `TelemetryBatch` when a DBC is loaded. Video is a separate TCP client of photon-server `:6700`.
- **log_replay**: local VSD1 `.log` file or directory of segments (nested `logs/` / `segments/` accepted). Payloads are CANP v3; replay honors inter-frame timestamps, `timeline_control` play/pause/seek (`cursor_ms` ≠ 0), and `speed`. Live mode is a no-op. `.zip` is not supported — extract first. `TimelineStatus` reports the archive min/max/cursor.
- **pcan**: USB PCAN on this machine (channel / baud / listen-only).

`hello_ack.features` includes `daq_tcp`, `log_replay`, `pcan`, `dbc`, and `track_sim`. Bundled DBC: `assets/dbc/carcan_telemcan.dbc` (the CARCan+TelemCAN file the legacy DAQ preset loads first). `signalId` is FNV-1a-32 of `MessageName.SignalName` (never 0; collisions increment).

## DBC + VSD1

`photon/decode` parses text DBC (`BO_` / `SG_`; multiplexed signals skipped) and decodes CAN frames with the same Intel/Motorola bit walk as legacy Photon. `photon/store` (`vsd1`) indexes on-disk VSD1 frames (`magic 0x56534431`, 24-byte big-endian header, CANP payload). CRC is not required.

## CANP (daq_tcp)

Mirrored from `photon-server/canp/canp.h` + `canp.c` (version 3). Wire is **big-endian** (network order):

| Field | Size | Notes |
| --- | --- | --- |
| magic | `uint32` | `0x43414E31` `"CAN1"` |
| version | `uint16` | `3` |
| count | `uint16` | 1–64 packets |
| seq | `uint32` | |
| timestamp | `uint64` | unix milliseconds |
| packet | 29 bytes × count | `uint32 can_id`, `uint8 dlc`, `uint8 data[8]`, `uint16 dt[8]` (per-byte δt) |

The Go observer also understands v2 (21-byte packets). This daemon follows the C reader and accepts **v3 only**. Fan-out on `:6500` writes raw ingress CANP payloads (not the on-disk VSD1 store framing).

## Track Sim (speed-profile solver)

Native strategy helper in `photon/synth` (`photon/tracksim.hpp`). No I/O in the library: tracks load from JSON text via the in-repo parser. Example course: `assets/tracks/hill_loop.json` (closed loop with a climb, descent, and curved return). The backend lists `assets/tracks/*.json` and runs solves on a worker thread.

`hello_ack.features` includes `track_sim`. New control messages stay on the JSON plane (protocol v1, additive). No new binary `msgType`.

| `type` | Direction | Fields |
| --- | --- | --- |
| `track_sim_list` | UI → backend | (none) |
| `track_sim_list` | backend → UI | `tracks`: `[{id, name, length_m, segment_count}]` |
| `track_sim_request` | UI → backend | `track_id` **or** inline `track`, `time_budget_s`, optional `initial_soc`, optional `car` overrides |
| `track_sim_result` | backend → UI | `feasible`, `total_time_s`, `energy_wh`, `end_soc`, `dE_dT`, `segments`: `[{index, length_m, grade, speed_mps, t_cum_s, wh_cum}]` |

### Math

Quasi-steady-state per segment (constant `v`, `a = 0` on the segment). Wheel power:

`P_whl = (½ ρ CdA v² + Crr m g cosθ + m g sinθ + m a) v`

with `sinθ = grade = Δh / path length`. Battery traction: `P_whl / η_drive` when propelling; regen is `min(−P_whl η_regen, regen_power_w)` and is capped. Net battery power subtracts constant `solar_w`. Discrete speed changes (including start-from-rest) add `½ m Δ(v²)` through the same efficiencies.

Given time budget `T`, minimize net battery energy subject to `Σ L_i / v_i ≤ T` and per-segment speed / motor-power / curvature limits. Dualize time with a constant cost-of-time `λ` (watts): each segment independently minimizes `(P_batt(v) + λ) / v`. Bisection on `λ` matches `T`. `λ = 0` is the unconstrained energy-optimal (slow) profile; large `λ` rides the speed limits. If even all-`v_max` exceeds `T`, `feasible` is false and the limit profile is returned. Sensitivity `dE_dT` at a binding solution is `−λ` (reported in Wh/s). A clamped uniform-speed profile (`v = L_tot / T`) is evaluated as a baseline and used if the search fails.

Optional `--assets-dir DIR` overrides catalog discovery (`PHOTON_ASSETS_DIR` compile definition, env, then `assets/` next to cwd / the binary).
