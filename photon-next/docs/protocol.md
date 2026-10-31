# Protocol v1

Video seek correlation: a `video_locate` request may include a nonzero
`query_id`. Its reply and all fetched NALs use message type `0x08`
(`VideoSeekPacket`), whose payload is a complete nested v1 `VideoLocateReply`
or `VideoFrame`. The nested header's `sequence` holds `query_id`; the outer
sequence retains the normal daemon sequence. Clients discard packets from
superseded requests. Video lookup errors echo the same `query_id` in JSON.
Requests without `query_id` retain the legacy unwrapped replies.

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
| `select_dbc` | UI → backend | `path` (filesystem) **or** bundled `id` / `dbc_id` (stem under `assets/dbc/`). Connection popover; no restart. |
| `signal_catalog` | backend → UI | `signals`, `dbc_id` (after `hello_ack` and on DBC change) |
| `subscribe_signals` | UI → backend | `signal_ids` |
| `unsubscribe_signals` | UI → backend | `signal_ids` |
| `timeline_control` | UI → backend | `mode`, `cursor_ms`, optional `speed` (replay multiplier, default 1) |
| `tx_arm_request` | UI → backend | `armed` |
| `tx_arm_status` | backend → UI | `armed`, `reason` |
| `error_alert` | backend → UI | `code`, `message` |
| `heartbeat` | UI → backend | refreshes TX watchdog |

TX arm is a stub: per-client armed flag, auto-disarm on disconnect or 3 s without a control message. Heartbeat is every 1 s; UI reconnect backoff is 200 ms / 500 ms / 1 s / 2 s.

Control-plane JSON, SHA-1, and Base64 are in-repo (`src/base`, `src/net`). No third-party dependency.

## Sources

- **daq_tcp** (default): TCP client of photon-server CANP fan-out (`:6500`). Parses CANP v3, forwards frames as `CanMonitorBatch`, and decodes subscribed DBC signals into `TelemetryBatch` when a DBC is loaded. Video is a separate TCP client of photon-server `:6700`.
- **log_replay**: local VSD1 `.log` file or directory of segments (nested `logs/` / `segments/` accepted). Payloads are CANP v3; replay honors inter-frame timestamps, `timeline_control` play/pause/seek (`cursor_ms` ≠ 0), and `speed`. `.zip` is not supported; extract first. `TimelineStatus` reports the archive min/max/cursor.
- **pcan**: USB PCAN on this machine (channel / baud / listen-only).

`hello_ack.features` includes `daq_tcp`, `log_replay`, `pcan`, `dbc`. Bundled DBC: `assets/dbc/carcan_telemcan.dbc`. `signalId` is FNV-1a-32 of `MessageName.SignalName` (never 0; collisions increment).

## DBC + VSD1

`src/decode` parses text DBC (`BO_` / `SG_`; multiplexed signals skipped) and decodes CAN frames with the same Intel/Motorola bit walk as legacy Photon. `src/store` (`vsd1`) indexes on-disk VSD1 frames (`magic 0x56534431`, 24-byte big-endian header, CANP payload). CRC is not required.

## CANP (daq_tcp)

The Go implementation is `photon-server/wire/canp.go`; the native adapter uses `src/wire/canp.cpp`. Wire is **big-endian** (network order):

| Field | Size | Notes |
| --- | --- | --- |
| magic | `uint32` | `0x43414E31` `"CAN1"` |
| version | `uint16` | `3` |
| count | `uint16` | 1-64 packets |
| seq | `uint32` | |
| timestamp | `uint64` | unix milliseconds |
| packet | 29 bytes × count | `uint32 can_id`, `uint8 dlc`, `uint8 data[8]`, `uint16 dt[8]` (per-byte δt) |

The Go observer also understands v2 (21-byte packets). This daemon follows the C reader and accepts **v3 only**. Fan-out on `:6500` carries raw ingress CANP payloads (not the on-disk VSD1 store framing).


