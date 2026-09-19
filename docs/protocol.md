# Protocol v1

Binary WebSocket payloads carry a **12-byte little-endian header** plus `payloadLen` bytes:

| Offset | Type | Name | Notes |
| --- | --- | --- | --- |
| 0 | `uint16` | `magic` | `0x5057` (`"PW"`) |
| 2 | `uint8` | `version` | `1` |
| 3 | `uint8` | `msgType` | see table |
| 4 | `uint32` | `sequence` | monotonically increasing per daemon |
| 8 | `uint32` | `payloadLen` | bytes after header |

## Binary message types

| `msgType` | Name | Payload |
| --- | --- | --- |
| `0x01` | `TelemetryBatch` | Repeated blocks: `uint32 signalId`, `uint16 sampleCount`, `uint16 reserved`, then `double timestamps[sampleCount]`, `double values[sampleCount]`. Timestamps are UTC milliseconds as IEEE-754 float64. |
| `0x02` | `CanMonitorBatch` | Repeated 24-byte items: `uint32 canId`, `uint8 dlc`, `uint8 flags`, `uint16 reserved`, `uint64 timestampMs`, `uint8 data[8]`. `flags` bit 0 reserved for 29-bit extended IDs. |
| `0x04` | `VideoFrame` | `uint64 timestampMs`, `uint8 flags` (bit 0 keyframe, bit 1 SPS/PPS config), `uint8 reserved[3]`, then an Annex-B H.264 NAL (including the `00 00 00 01` start code). |
| `0x05` | `TimelineStatus` | 33 bytes: `uint8 mode`, `uint64 cursorMs`, `minTimeMs`, `maxTimeMs`, `totalBatches`. Mode: `1` pause, `2` play, `3` live. |

`0x03` is reserved.

## Control plane (JSON text frames, `type` field)

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

TX arm is a stub: per-client armed flag, auto-disarm on disconnect or 3 s without a control message. Heartbeat is every 1 s; UI reconnect backoff is 200 ms / 500 ms / 1 s / 2 s.

Control-plane JSON, SHA-1, and Base64 are in-repo (`photon/base`, `photon/net`) — no third-party dependency.

## Sources

- **daq_tcp** (default): TCP client of photon-server CANP fan-out (`:6500`). Parses CANP v3, forwards frames as `CanMonitorBatch`, and decodes subscribed DBC signals into `TelemetryBatch` when a DBC is loaded. Video is a separate TCP client of photon-server `:6700`.
- **log_replay**: local VSD1 `.log` file or directory of segments (nested `logs/` / `segments/` accepted). Payloads are CANP v3; replay honors inter-frame timestamps, `timeline_control` play/pause/seek (`cursor_ms` ≠ 0), and `speed`. `.zip` is not supported — extract first. `TimelineStatus` reports the archive min/max/cursor.
- **pcan**: USB PCAN on this machine (channel / baud / listen-only).

`hello_ack.features` includes `daq_tcp`, `log_replay`, `pcan`, `dbc`, and `track_sim`. Bundled DBC: `assets/dbc/carcan_telemcan.dbc`. `signalId` is FNV-1a-32 of `MessageName.SignalName` (never 0; collisions increment).

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

The Go observer also understands v2 (21-byte packets). This daemon follows the C reader and accepts **v3 only**. Fan-out on `:6500` carries raw ingress CANP payloads (not the on-disk VSD1 store framing).

## Track Sim

Solver lives in `photon/synth` (`photon/tracksim.hpp`); no I/O in the library. The backend lists `assets/tracks/*.json` and runs solves on a worker thread. Control messages are JSON-plane additive (no new binary `msgType`):

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

`--assets-dir DIR` overrides catalog discovery (`PHOTON_ASSETS_DIR` compile definition, env, then `assets/` next to cwd / the binary).
