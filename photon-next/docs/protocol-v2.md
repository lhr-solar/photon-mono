# Protocol v2

**Status:** draft, 2026-11-22. Replaces [protocol v1](protocol.md) when the Go server becomes the only
backend ([architecture-v2.md](architecture-v2.md)).

One WebSocket per client at `/v2`. Text frames carry JSON control messages; binary frames carry data.
All binary integers are **little-endian**. Times are Unix time: `_ns` nanoseconds, `_ms` milliseconds.

## Binary frame header (12 bytes)

Same shape as v1, so the framing code carries over.

| Offset | Type | Name | Notes |
|---|---|---|---|
| 0 | `u16` | `magic` | `0x3250` (`"P2"` on the wire) |
| 2 | `u8` | `version` | `2` |
| 3 | `u8` | `type` | see below |
| 4 | `u32` | `seq` | increases by one per binary frame on this socket |
| 8 | `u32` | `len` | payload bytes after the header |

## Binary messages (server → client)

### `0x20` LiveFrames
Raw CAN frames from the live ring, only the message IDs the client subscribed to.

| Type | Field |
|---|---|
| `i64` | `base_ns` |
| `u16` | `count` |
| `u16` | reserved |
| then `count` records: | |
| `u32` | `dt_us`: microseconds after `base_ns` |
| `u32` | `can_id`: wire ID, bit 31 set for 29-bit IDs |
| `u8` | `bus`: index into `catalog.buses`, or `0xFF` unknown |
| `u8` | `len`: data bytes, 0–64 |
| `u8[len]` | `data` |

### `0x21` Gap
The client fell behind the live ring; frames between the two times were skipped.

| `i64` `from_ns` | `i64` `to_ns` |
|---|---|

### `0x22` Tile
One tile, or one piece of it (tiles are sent in pieces of at most 64 KiB, interleaved with live data).

| Type | Field |
|---|---|
| `u32` | `qid`: the request this answers |
| `u32` | `signal`: catalog signal index |
| `u8` | `level`: 0 = raw samples, else a bucket level from `hello_ack.levels` |
| `u8` | `flags`: bit 0 sealed (never changes), bit 1 last piece |
| `u16` | reserved |
| `i64` | `index`: tile index at this level |
| `u32` | `offset`: index of this piece's first sample in the tile |
| `u32` | `n`: samples in this piece |
| `f64[n]` | `t_ms` |
| level 0: `f64[n]` | `raw` |
| level > 0: `f64[n]` ×3 | `min`, `max`, `mean` |

Values are in **raw units**; the client scales them with the catalog.

### `0x23` VideoChunk
One piece of an Annex-B H.264 frame, at most 16 KiB. Sent only when nothing telemetry is queued.

| Type | Field |
|---|---|
| `u64` | `t_ms` |
| `u32` | `frame`: frame sequence number |
| `u8` | `flags`: bit 0 keyframe, bit 1 SPS/PPS, bit 2 first piece, bit 3 last piece, bit 4 replay |
| `u8` | `stream`: camera index |
| `u16` | reserved |
| bytes | payload piece |

A frame with a missing piece is dropped; the decoder waits for the next keyframe. Nothing is resent.

`0x24`–`0x2F` are reserved (`0x24` for the downlink acknowledgement).

## Control messages (JSON text frames, `type` field)

| `type` | Direction | Fields |
|---|---|---|
| `hello` | C → S | `protocol` (2), `client`, `token` (LAN listener only; browsers cannot set WebSocket headers) |
| `hello_ack` | S → C | `protocol`, `server`, `user`, `server_time_ns`, `catalog_sha`, `live_run`, `levels` (bucket ms per level), `features` |
| `catalog` | S → C | sent after `hello_ack` and whenever the DBCs change; when decoding turns off, an empty one (`sha` `""`, no messages); see below |
| `subscribe` | C → S | `messages`: wire IDs. Replaces the previous set; `[]` stops live data |
| `runs_list` | C → S | — |
| `runs` | S → C | `runs`: `[{id, label, source, device, dbc_sha, start_ms, end_ms, live}]` |
| `tile_request` | C → S | `qid`, `run`, `signal`, `level`, `index` |
| `tile_cancel` | C → S | `qid` |
| `tile_error` | S → C | `qid`, `message` |
| `video_subscribe` | C → S | `mode`: `live` / `replay` / `off`, `from_ms` (replay) |
| `video_seek` | C → S | `qid`, `t_ms` |
| `video_status` | S → C | `mode`, `t_ms`, `clock` (provenance) |
| `ping` / `pong` | both | `t_ms`; every 1 s, socket closed after 5 s silent |
| `error` | S → C | `code`, `message` |
| `downlink` | C → S | **reserved**: a payload for the car, sent back over its :6100 socket |

### `catalog`
```json
{"type": "catalog", "sha": "…", "buses": ["CarCAN", "DAqCAN"],
 "messages": [{"id": 1234, "name": "BPSPackData", "bus": 0, "length": 8, "selector": -1,
   "signals": [{"index": 17, "name": "CellSumVoltage", "start": 0, "bits": 16,
     "little_endian": true, "signed": false, "kind": "int", "factor": 0.01, "offset": 0,
     "unit": "V", "mux": -1}]}]}
```
- A signal's `index` is its position in the catalog and is stable for one `sha`.
  `TileKey.signalId` is this index; `TileKey.dbcHash` is `sha`.
- `kind`: `int`, `f32` or `f64`. `mux`: the selector page this signal belongs to, or -1.
- A message's `selector` is the position of its mux selector **within that message's `signals` list**
  (not a catalog `index`), or -1 when the message is not multiplexed.
- The client decodes frames with these layouts; it does not parse DBC files.

### Server rules
- At most 4–8 InfluxDB queries run at once across all clients; cancelled ones are dropped.
- Only the **live run's** sealed tiles are cached, once, shared by every client (~8–16 MB). A tile leaves
  the cache when it is older than the live view window (~10 min) or the cap is reached, and the cache
  empties when the run ends. Tiles of past runs come straight from InfluxDB. There is no video replay cache.
- A client whose live cursor falls off the ring jumps to the newest frame and gets a `Gap`.

## Login
- **Cloudflare listener:** the request must carry `Cf-Access-Jwt-Assertion`. The server checks its
  RS256 signature against the team's Access certs and its `aud` against the application tag; the
  user is the token's `email`.
- **LAN listener:** `hello.token` must match the configured token (`PHOTON_PASSWORD` today).

## Run upload (HTTPS, same login)
1. `POST /v2/uploads` `{device, label, bytes, sha256}` → `{upload_id}`
2. `PUT /v2/uploads/{upload_id}?offset=N` with ≤50 MB of the compressed run; repeat. Resume by asking
   `GET /v2/uploads/{upload_id}` for the received offset.
3. `POST /v2/uploads/{upload_id}/finish` → the server checks `sha256`, replays the segments through
   the decoder into InfluxDB under a new session (`source: "pcan"`), and returns the session ID.
