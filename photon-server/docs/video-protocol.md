# Video store and wire protocol

The car/pit camera sends **H.264 over RTP/UDP** (payload type 96), matching the legacy Photon client's depacketization (single NAL, FU-A, optional STAP-A, Annex-B start codes). The server **never re-encodes**: it depacketizes RTP into Annex-B NAL units and appends the already-compressed elementary stream to disk.

## Ports

| Role | Default | Transport |
|------|---------|-----------|
| Ingress | `:6200` | UDP RTP/H.264, plus TCP length-prefixed RTP for the Oracle pipe |
| Clients | `:6700` | TCP framed NALs (live or replay / locate / range) |
| Live fan-out | `:6800` | UDP framed NAL fragments (`PVU1`). Subscribe with PHAU + cmd `1`. |

## Storage layout

Each video segment is a pair of files under `data/video/segments/`:

| File | Contents |
|------|----------|
| `NNNNNNNNNNNN.h264` | Raw Annex-B elementary stream (`00 00 00 01` + NAL). Independently decodable: SPS/PPS are copied to the start of each new segment and cached in the index header. |
| `NNNNNNNNNNNN.idx` | Compact binary index (magic `VIDX` / `0x56494458`). Fixed 2048-byte header (wall-clock start/end unix ms, SPS, PPS) followed by 24-byte NAL records: `timestampMs u64`, `offset u64`, `size u32`, `flags u8`, `nalType u8`, reserved 2 bytes. |

Segments rotate at **64 MiB** or **60 s**, preferring the next IDR so a file does not split mid-GOP (hard cap 128 MiB / 120 s). Keyframe byte offsets and timestamps live in the in-memory index (rebuilt from `.idx` on open). If an `.idx` is missing or truncated, it is rebuilt from the `.h264` stream on startup.

## Client framing (server → client)

Every NAL is one frame. All integers are **big-endian**.

```
offset  size  field
0       4     payloadLen     uint32   length of the NAL payload only
4       8     timestampMs    uint64   wall-clock unix milliseconds
12      1     flags          uint8    bit0 = keyframe (IDR), bit1 = config (SPS/PPS)
13      N     payload        bytes    Annex-B NAL including 00 00 00 01
```

Total size on the wire: `13 + payloadLen`.

## Subscribe header (client → server)

Immediately after TCP connect, the client writes **9 bytes**:

```
offset  size  field
0       1     mode           uint8    0 = live, 1 = replay
1       8     startTimeMs    uint64   replay start (unix ms); ignored for live
```

## Live vs replay

**Live (`mode = 0`):** the server sends the current SPS and PPS (config flags), then the most recent IDR, then follows the live tail. Slow clients are not buffered without bound: a write timeout (~250 ms) or lag of more than ~45 NALs drops the client back to the latest keyframe (plus fresh SPS/PPS).

**Replay (`mode = 1`):** the server finds the last IDR with `timestampMs <= startTimeMs` (or the first IDR if `T` is earlier than any keyframe), sends SPS/PPS, then streams stored NALs from that keyframe to the end of recorded data as fast as the client reads. The client paces display using `timestampMs`. Replay does not automatically switch to live; reconnect with `mode = 0` to follow the tail.

## Example client (Go)

```go
conn, err := net.Dial("tcp", "127.0.0.1:6700")
if err != nil { log.Fatal(err) }
defer conn.Close()

// Live:
var hdr [9]byte // mode=0, start=0
if _, err := conn.Write(hdr[:]); err != nil { log.Fatal(err) }

// Replay from unix ms T instead:
// hdr[0] = 1
// binary.BigEndian.PutUint64(hdr[1:9], T)

for {
    var prefix [13]byte
    if _, err := io.ReadFull(conn, prefix[:]); err != nil { break }
    n := binary.BigEndian.Uint32(prefix[0:4])
    ts := binary.BigEndian.Uint64(prefix[4:12])
    flags := prefix[12]
    payload := make([]byte, n)
    if _, err := io.ReadFull(conn, payload); err != nil { break }
    _ = ts
    _ = flags // bit0 IDR, bit1 SPS/PPS
    // feed payload to an H.264 decoder (Annex-B)
}
```

Observer JSON includes a `video` object: `listening`, `bytes_stored`, `segment_count`, `live_clients`, `last_receive_ms`.
