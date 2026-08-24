# photon-next

Rewrite of Longhorn Racing Solar's Photon telemetry stack: a native backend daemon owns hardware, network, files, DBC decode, arena storage, and TX safety. A Dear ImGui/ImPlot WASM page talks to that daemon over localhost WebSocket.

This repository is **milestone 2**: shared wire protocol, backend daemon, and the WASM UI. The existing native client (`F:\lhr\Photon`) and Go CANP fan-out (`F:\lhr\photon-server` on `:6500`) are not modified.

```
PCAN / DAQ TCP / log replay / synthetic
                │
                ▼
     photon-backend (this repo)
       sockets, CANP, sources,
       TX arm stub, protocol v1
                │  WebSocket 127.0.0.1:8765
                ▼
     ImGui WASM UI  frontend/  (.artifacts-web/photon.html)
```

`photon-server` stays as-is. `daq_tcp` is a TCP **client** of its CANP fan-out (default `127.0.0.1:6500`).

## Layout

| Path | Role |
| --- | --- |
| `core/` | Shared protocol library. No UI, GPU, or socket server. |
| `backend/` | Native daemon: WebSocket server, session controller, sources. |
| `frontend/` | Dear ImGui + ImPlot WASM client (Emscripten). |
| `tests/` | CTest target for encode/decode, CANP, WS accept-key. |
| `.artifacts/` | Native build directory (same convention as Photon). |
| `.artifacts-web/` | Emscripten build directory (`photon.html` / `.js` / `.wasm`). |
| `.external/` | Fetched emsdk, imgui, implot (gitignored). |

## Build

Windows host with clang, Ninja, CMake 3.25+:

```powershell
cmake -S . -B .artifacts -G Ninja -D CMAKE_C_COMPILER=clang -D CMAKE_CXX_COMPILER=clang++
cmake --build .artifacts
ctest --test-dir .artifacts --output-on-failure
```

The backend binary is `.artifacts/backend/photon-backend.exe` (Ninja multi-folder layout) or `.artifacts/photon-backend` depending on generator.

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

Or: `.\frontend\scripts\setup-emsdk.ps1`

### Build the page

```powershell
.\frontend\scripts\build.ps1
```

Equivalent manual steps:

```powershell
. .\.external\emsdk\emsdk_env.ps1
emcmake cmake -S . -B .artifacts-web -G Ninja -DPHOTON_FRONTEND=ON -DBUILD_TESTING=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build .artifacts-web
```

Outputs: `.artifacts-web/photon.html`, `.artifacts-web/photon.js`, `.artifacts-web/photon.wasm`, `.artifacts-web/photon.data` (preloaded fonts).
Do **not** mix this tree with the native `.artifacts/` configure (`PHOTON_FRONTEND` defaults OFF).

### Run the synthetic demo

Terminal 1 — native backend (build it first with the clang/Ninja commands above):

```powershell
.\.artifacts\backend\photon-backend.exe --ws-port 8765
```

Terminal 2 — static file server (do not open `photon.html` as `file://`; the browser must be on localhost for `ws://127.0.0.1:8765`):

```powershell
.\frontend\scripts\serve.ps1
```

Or: `python -m http.server 8080 --directory .artifacts-web`

Open **http://127.0.0.1:8080/photon.html**. The UI sends `hello`, `select_source synthetic`, and `subscribe_signals` for rpm/speed/soc/temp (ids 1–4). Heartbeat is every 1 s; reconnect backoff is 200 ms / 500 ms / 1 s / 2 s.

Optional: `emrun .artifacts-web/photon.html --no_browser` (emrun is on PATH after `emsdk_env.ps1`).

Headless protocol check (backend must be running):

```powershell
python .\frontend\scripts\smoke_ws.py
```

### Screens

| Tab | Status |
| --- | --- |
| Pit (default) | Vitals tiles, 32-cell heatmap, alarm strip, 3D + Live View dock tiles |
| Plots | ImPlot time-series, catalog checkboxes, live window, pause + zoom |
| Dynamics | MPH / steer / G / wheel-slip tiles + G-G and slip plots |
| Live View | WebCodecs H.264 → GLES texture |
| 3D | WebGL2 glTF car, telemetry-driven wheels / steer / suspension |
| Track Sim | Catalog + solve + speed/elevation plots |
| CAN | Live `CanMonitorBatch` table (id, dlc, hex, Hz, count), sortable |
| Network | Source picker + control-plane log |

Navigation is a **top tab bar** only (no sidebar). Titlebar right: connection dot, source, timeline mode/cursor, Live/Pause transport.

## Run

```powershell
.\.artifacts\backend\photon-backend.exe --ws-port 8765 --daq-host 127.0.0.1 --daq-port 6500
```

Flags:

| Flag | Default | Meaning |
| --- | --- | --- |
| `--ws-port` | `8765` | WebSocket listen port (always bound to `127.0.0.1`) |
| `--daq-host` | `127.0.0.1` | Default CANP fan-out host for `select_source daq_tcp` |
| `--daq-port` | `6500` | Default CANP fan-out port |

Connect a WebSocket client, send a JSON `hello`, then `select_source` / `subscribe_signals`. Binary frames are photon-wire v1.

## JSON dependency

Control-plane JSON is an in-repo recursive-descent parser/stringifier (`core/src/json.cpp`): objects, arrays, strings, numbers, bools, null. No third-party JSON library and no link-time dependency. SHA-1 and Base64 used for the WebSocket handshake are also in-repo (`core/src/sha1.cpp`, `core/src/base64.cpp`).

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
| `hello_ack` | backend → UI | `protocol_version`, `server`, `features` |
| `select_source` | UI → backend | `source`: `pcan` \| `daq_tcp` `{host,port}` \| `log_replay` `{path}` \| `synthetic` |
| `select_dbc` | UI → backend | `path` (filesystem) **or** bundled `id` / `dbc_id` (stem under `assets/dbc/`) |
| `signal_catalog` | backend → UI | `signals`: `[{id, message, name, unit, min, max}]` — after `hello_ack` and on DBC change |
| `subscribe_signals` | UI → backend | `signal_ids` |
| `unsubscribe_signals` | UI → backend | `signal_ids` |
| `timeline_control` | UI → backend | `mode`, `cursor_ms`, optional `speed` (replay multiplier, default 1) |
| `tx_arm_request` | UI → backend | `armed` |
| `tx_arm_status` | backend → UI | `armed`, `reason` |
| `error_alert` | backend → UI | `code`, `message` |
| `heartbeat` | UI → backend | refreshes TX watchdog |

TX arm is a stub: per-client armed flag, auto-disarm on disconnect or 3 s without a control message.

### Sources (this milestone)

- **synthetic**: four sine/step signals (`rpm` 1, `speed` 2, `soc` 3, `temp` 4) at ~100 Hz into an in-memory ring. Streamed as `TelemetryBatch` at ~30 Hz to subscribers, `CanMonitorBatch` bursts at 10 Hz, `TimelineStatus` at 1 Hz.
- **daq_tcp**: TCP client of photon-server CANP fan-out. Parses CANP v3, forwards frames as `CanMonitorBatch`, and decodes subscribed DBC signals into `TelemetryBatch` when a DBC is loaded.
- **log_replay**: local VSD1 `.log` file or directory of segments (nested `logs/` / `segments/` accepted). Payloads are CANP v3; replay honors inter-frame timestamps, `timeline_control` play/pause/seek (`cursor_ms` ≠ 0), and `speed`. Live mode is a no-op. `.zip` is not supported — extract first. `TimelineStatus` reports the archive min/max/cursor.
- **pcan**: rejected with `error_alert` / `not_implemented`.

`hello_ack.features` includes `synthetic`, `daq_tcp`, `log_replay`, `dbc`, and `track_sim`. Bundled DBC: `assets/dbc/carcan_telemcan.dbc` (the CARCan+TelemCAN file the legacy DAQ preset loads first). `signalId` is FNV-1a-32 of `MessageName.SignalName` (never 0; collisions increment).

## DBC + VSD1

`core/dbc` parses text DBC (`BO_` / `SG_`; multiplexed signals skipped) and decodes CAN frames with the same Intel/Motorola bit walk as legacy Photon. `core/vsd1` indexes on-disk VSD1 frames (`magic 0x56534431`, 24-byte big-endian header, CANP payload). CRC is not required.

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

Native strategy helper in `core/` (`photon/tracksim.hpp`). No I/O in the library: tracks load from JSON text via the in-repo parser. Example course: `assets/tracks/hill_loop.json` (closed loop with a climb, descent, and curved return). The backend lists `assets/tracks/*.json` and runs solves on a worker thread.

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
