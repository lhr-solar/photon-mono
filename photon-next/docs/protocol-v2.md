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

### `0x20` LiveSamples
Decoded values of the signals the client subscribed to, from the live ring. The server decodes; the
client never sees CAN frames or bit layouts (user decision 2026-11-23: "client shouldn't parse on their
side").

| Type | Field |
|---|---|
| `i64` | `base_ns` |
| `u16` | `count` |
| `u16` | reserved |
| then `count` records: | |
| `u32` | `dt_us`: microseconds after `base_ns` |
| `u32` | `signal`: catalog signal index |
| `f64` | `value`: in the signal's unit (factor and offset applied) |

- A multi-sample message (several readings of one sensor per frame) gives one record per reading, each at
  its own time. A multiplexed message gives records only for the signals on the page it carried.
- A signal that does not decode (frame too short, NaN/infinite float) gives no record.

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
| level 0: `f64[n]` | `value` |
| level > 0: `f64[n]` ×3 | `min`, `max`, `mean` |

Values are in the signal's **unit**, scaled by the server like live samples (with a negative factor,
the server swaps min and max so `min <= max`).

The server streams a tile: it sends each piece as soon as it has read that many rows from InfluxDB, so
the first piece arrives before the whole tile is read, and the server holds about one piece per query.
If something fails after some pieces were sent, a `tile_error` with the same `qid` follows: drop the pieces
received for that `qid`, and any that still arrive for it. After a `tile_cancel`, a few pieces already
queued may still arrive; drop them too. A client that reads nothing for 5 s while a tile streams gets a
`tile_error` for it (the query holds one of the server's few InfluxDB slots), and can ask again.

**Levels and tiles.** `hello_ack.levels` lists the bucket width of each level in ms; today
`[0, 10, 100, 1000, 10000, 60000, 600000]` (level 0 = raw samples). A level-0 tile covers **10 s**; a tile
of level L > 0 covers `levels[L] × 1024` ms (1024 buckets). Tile `index` `i` covers `[i × span, (i+1) × span)`
in Unix ms. Read the list from `hello_ack`; do not hard-code it.

**Choosing a level.** For a plot `w` pixels wide showing `d` ms, use the **coarsest** level whose bucket is
at most `d / w` ms (one bucket per pixel or finer). Use level 0 only when even the 10 ms level is coarser
than a pixel (views shorter than ~10–20 s). Request the tiles of that level that overlap the view.

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
| `subscribe` | C → S | `signals`: catalog signal indexes. Replaces the previous set; `[]` stops live data |
| `runs_list` | C → S | — |
| `runs` | S → C | `runs`: `[{id, label, source, device, dbc_sha, start_ms, end_ms, live, rev}]`; `rev` changes when the run's history is rewritten (backfill) |
| `tile_request` | C → S | `qid`, `run`, `signal`, `level`, `index` |
| `tile_cancel` | C → S | `qid` |
| `tile_error` | S → C | `qid`, `message` |
| `video_subscribe` | C → S | `mode`: `live` / `replay` / `off`, `from_ms` (replay) |
| `video_seek` | C → S | `qid`, `t_ms` |
| `video_status` | S → C | `mode`, `t_ms`, `clock` (provenance), optional `qid` (after a seek), optional `gap` `{from_ms, to_ms, reason}`: `none` (no video recorded then; show "No video recorded HH:MM:SS–HH:MM:SS"), `archived` (stored elsewhere, not on the server), `stale` (live: camera silent since `from_ms`, `to_ms` 0). Replay keeps following the timeline through a gap; a plain status follows when video flows again |
| `ping` / `pong` | both | `t_ms`; every 1 s, socket closed after 5 s silent |
| `error` | S → C | `code`, `message` |
| `downlink` | C → S | **reserved**: a payload for the car, sent back over its :6100 socket |

### `catalog`
```json
{"type": "catalog", "sha": "…", "buses": ["CarCAN", "DAqCAN"],
 "messages": [{"id": 1234, "name": "BPSPackData", "bus": 0,
   "signals": [{"index": 17, "name": "CellSumVoltage", "unit": "V", "min": 0, "max": 655.35,
     "integer": false, "mux": -1, "period_ms": 100}]}]}
```
- A signal's `index` is its position in the catalog and is stable for one `sha`.
  `TileKey.signalId` is this index; `TileKey.dbcHash` is `sha`.
- The catalog is for display only: names, units, the DBC's min/max (a plot's default range), and
  `integer` (the scaled value is always a whole number: factor 1, offset 0, integer signal; show it
  without decimals). `mux` is the selector page the signal belongs to, or -1. `id` and `bus` are for
  showing where a message comes from.
- No bit layouts, factors or offsets: the client never decodes CAN frames and never parses DBC files.
- `period_ms`: how often the signal is normally sent (the DBC's cycle time, or a sensor group's sample
  period), 0 when unknown. It is for drawing gaps, below.

### Gaps and missing tiles
- **No data** (car off, link down, rows dropped): the points are simply absent. Do not draw a line across a
  hole: break the line when two neighbouring points are more than **3 × `period_ms`** apart (with
  `period_ms` 0, more than 3 × the median spacing of that tile), and on a level > 0 also when more than **2
  bucket widths** apart.
- **Tile not here** (not received yet, `tile_error`): unknown, not empty. Draw that span from a coarser tile
  the client already has, or show it as loading; retry after a `tile_error` with backoff (1 s, 2 s, 4 s, … up
  to 30 s).
- **Unsealed tiles** (the live edge): request them again until one arrives sealed. After a `Gap` (live data
  skipped), request the tiles of the skipped span; their rows are in InfluxDB by then.
- **Rewritten history:** a sealed tile never changes **for one `rev` of its run**. Keep `rev` in the cache
  key (`TileKey` {session, rev, dbc hash, signal, level, index}); when `runs` shows a new `rev` for a run,
  drop that run's cached tiles.

### Server rules
- At most 4–8 InfluxDB queries run at once across all clients; cancelled ones are dropped.
- Only the **live run's** sealed tiles are cached, once, shared by every client (~8–16 MB). A tile leaves
  the cache when it is older than the live view window (~10 min) or the cap is reached, and the cache
  empties when the run ends. Tiles of past runs come straight from InfluxDB. There is no video replay cache.
- A client whose live cursor falls off the ring jumps to the newest frame and gets a `Gap`.
- The live ring keeps raw frames (24 B each); each client's pusher decodes only the messages its
  subscribed signals are in, with the same decoder that writes InfluxDB.

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
