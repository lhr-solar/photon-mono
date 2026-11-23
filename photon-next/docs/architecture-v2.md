# Photon client architecture v2

**Status:** draft, agreed 2026-11-22. Nothing here is built yet unless it says so.
Wire format: [protocol-v2.md](protocol-v2.md). Server pipeline background: the photon-server handoff
(ingest pool → decoder → aggregator → InfluxDB + live ring).

## What changes

Today every user runs a local C++ `photon-backend` that owns sources, DBC decoding and a local TSDB,
and the ImGui UI talks to it over `ws://127.0.0.1:8765` (protocol v1).

In v2 the **Go server is the only backend**. Clients connect straight to it:

```
Car ──CANP :6100──▶ photon-server ──▶ InfluxDB 3 (history)
Car ──RTP  :6200──▶      │  ├─ live ring (raw frames, last few seconds)
                         │  └─ video streamer (last GOP)
                         │
       HTTP  /     web bundle (photon.html + .wasm)
       WSS   /v2   one socket per client: control, live frames, tiles, video
       HTTPS /v2/uploads   PCAN run uploads
                         │
        ┌────────────────┴─────────────────┐
  Cloudflare listener (loopback)      LAN listener (0.0.0.0)
  checks Cf-Access-Jwt-Assertion      token in the hello message
        │                                  │
  remote browsers (Google SSO)        pit laptops, no internet needed
```

- Both listeners run the same handler; only the login check differs.
- The ImGui UI stays (WASM page and native `photon-ui.exe`); only its transport changes.
- **One socket per client.** The server's send queue always sends telemetry before video, sends video
  in pieces of at most 16 KiB, and keeps the socket send buffer small, so a keyframe never delays
  live rows by more than one piece.

## Laptop PCAN

```
PCAN adapter ─▶ photon-pcan ─CANP─▶ photon-server (laptop, local mode) ─▶ raw segments
                                     │  live view: same /v2 on localhost
                                     └─ upload run ─HTTPS─▶ main server ─▶ InfluxDB
```

- `photon-pcan` is a small native tool built from `photon/hw`. It reads the adapter and sends CANP
  to any server's :6100: the laptop's own server offline, or the main server live.
- The laptop runs the same Go server in **local mode** (InfluxDB off): it records raw segments and
  serves live view to the UI over localhost.
- **Upload:** the laptop compresses a run's raw segments and uploads them in ≤50 MB chunks (Cloudflare's
  free plan caps a request at 100 MB), resumable. The main server replays them through the decoder
  into `frame` rows under a new session. This is the same path as backfill from raw segments.
- `session` rows carry `source` (`car` / `pcan`) and `device`, so uploaded runs list next to car runs.

## Where the old backend's jobs go

| `photon-backend` today | v2 |
|---|---|
| `daq_tcp` source | Gone; the server is the source |
| `pcan` source | `photon-pcan` → a server (laptop or main) |
| `synthetic` source | Test tools send CANP to :6100 (`send-laptop-synth.py`) |
| `subscribe_signals` / `subscribe_can` | `subscribe` by message ID; the server filters raw frames |
| `history_query` / `credit` / `cancel` | `tile_request` / `tile_cancel`, tiles from InfluxDB |
| `catalog_list` (log sessions) | `runs`, from InfluxDB's `session` table |
| `log_replay`, `timeline_control` | Moving through history tiles; play/pause is a client-side cursor |
| `select_dbc` | The server owns the DBCs and sends a `catalog` |
| `video_*` | `video_subscribe` / `video_seek` on the same socket |
| CSV export | Server HTTP endpoint querying InfluxDB (later) |
| `tx_arm` / `tx_can_frame` | Not now. A generic **downlink** (server → car, any payload) is reserved in the protocol; the server will write back on the car's :6100 socket |
| `track_sim_*`, `analysis_compute_*` | Removed for now |

Retired: `photon-backend`, `photon-index --live-fanout` and the local TSDB, the Go server's raw CANP
fan-out on :6500, TCP video on :6700, and `startLiveWriter` / `startLocalUI` in photon-server.

## Inside the client (WASM / native)

| Module | v2 |
|---|---|
| Transport | v2 WebSocket client; keeps the existing reconnect backoff |
| Catalog + decode | `photon/decode` bit-walk driven by the server's `catalog` (no DBC text parsing), so client and server decode identically |
| Live store | New: per subscribed signal, a bounded ring of decoded samples about as long as the server's live ring |
| TileCache | Same `TileKey` {session, dbc hash, signal, level, index} and IndexedDB; tiles come from the server; the live store fills the open tile at the live edge |
| Plot merge | Builds `PlotArrays` from tiles + live samples, dropping duplicate timestamps |
| Video | WebCodecs in the browser; the existing decoder in the native exe |

Values arrive **raw** (live frames and tiles alike). The client scales with the catalog:
`value = raw × factor + offset`; with a negative factor, a tile's min and max swap.

**Joining live:** subscribe (the cursor starts at the oldest ring row) → request tiles for the visible
window → merge by timestamp, drop duplicates. The ring covers InfluxDB's ~0.5 s write delay, so there
is no gap and no re-query.

## Build order

1. **Server endpoint:** `/v2` WebSocket, LAN token + Cloudflare JWT login, `hello`, `catalog`.
2. **Live:** live ring, `LiveFrames`, `subscribe`, `Gap`, telemetry-first send queue.
3. **History:** `runs`, tiles from InfluxDB, a small cache of the live run's sealed tiles, query concurrency cap.
4. **Client:** v2 transport, catalog decode, live store, TileCache source swap.
5. **PCAN:** `photon-pcan`, server local mode, run upload + replay into InfluxDB, segment compression.
6. **Video** on the shared socket.
7. **Later:** CSV export, downlink, analysis.

`photon-backend` retires after steps 4 and 5.
