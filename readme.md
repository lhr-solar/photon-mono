# Photon Server

Photon Server is the ingest server on the **PC** (CANP, video store, fan-out, observer). It starts `photon-index --live-fanout` so the TSDB is written on this machine, and a **local UI** (`photon-backend` + `photon.html`) pointed at loopback fan-out. That local UI is how you watch the car when you are sitting at the PC. The laptop runs its own client when you want a different source (USB PCAN, another host). Oracle is a pipe *to* this process (public IP + WireGuard DNAT to `10.66.0.2`; Wireshark on `ens3` still sees the public packets).

It operates over TCP using the lightweight CANP (CAN Packet) wire protocol and fans out live streams to `photon-backend` (laptop/client) and a local `photon-index --live-fanout` process that writes the TSDB (`data/tsdb`) on this PC. Incoming H.264 from the car/pit is accepted as RTP/UDP, stored as a **separate** compressed elementary stream (never re-encoded), and served via GOP pointers (`video.vpt` / `video_locate`) rather than being muxed into the time series. Observer TUI still attaches on `:5400`.

---

## Architecture

```
                            ┌───────────────┐
                            │ Ingress Source│ (FPGA / CANP source)
                            └───────┬───────┘
                                    │ TCP :6100 (CANP)
                                    ▼
                         ┌────────────────────┐
                         │   photon-server    │  the server (on the PC)
                         │                    │
                         └───┬───────────┬────┘
            TCP :6500 (Fan-out)│           │ TCP :5400 (JSON)
           ┌───────────────────┼───────────────────┐
           ▼                   ▼                   ▼
  ┌──────────────────┐ ┌──────────────────┐ ┌─────────────────┐
  │ photon-index     │ │ PC local UI      │ │ laptop client   │
  │ --live-fanout    │ │ photon-backend   │ │ photon-backend  │
  │ 127.0.0.1:6500   │ │ 127.0.0.1:6500   │ │ --daq-host <pc> │
  │ → data/tsdb/     │ │ :8080 / :8765    │ │ or --source pcan│
  └────────┬─────────┘ └──────────────────┘ └─────────────────┘
                 │ video.vpt
                 ▼
  Car/pit camera ── UDP :6200 RTP/H.264 ──► (Oracle WG DNAT) ──► VideoStore (./data/video/)
  Public clients  ── TCP :6200 len+RTP  ──► (Oracle WG DNAT) ──► same store
                                                 │
                                                 ▼
                                          TCP :6700  video_locate / range
                                          + photon-observer :5400
```

---

## Network Ports & Firewall Requirements

| Port | Protocol | Default Bind | Purpose |
|------|----------|--------------|---------|
| `6100` | TCP | `0.0.0.0` | **Ingress**: CANP telemetry stream ingestion |
| `6500` | TCP | `0.0.0.0` | **Fan-out**: Downstream clients & telemetry consumers |
| `5400` | TCP | `0.0.0.0` | **Observer**: Real-time JSON telemetry & TUI monitor |
| `6200` | UDP + TCP | `0.0.0.0` | **Video ingress**: RTP/H.264 PT=96. Public path is Oracle DNAT over WireGuard to `10.66.0.2` (UDP stays UDP). TCP length-prefixed RTP is rollback only (`udp-rtp-to-tcp.py` + `ssh -R`). |
| `6700` | TCP | `0.0.0.0` | **Video clients**: live subscribe + historical replay / locate / range |
| `6800` | UDP | `0.0.0.0` | **Video fan-out**: live Annex-B NALs to subscribed UIs. Client sends PHAU+cmd first (NAT hole punch). Not camera ingest. |
| `8765` | TCP | `127.0.0.1` | **Local UI**: photon-backend WebSocket (this PC only) |
| `8080` | TCP | `127.0.0.1` | **Local UI**: `photon.html` |
| `18765` | TCP | `127.0.0.1` | **Grok analyst sidecar** (optional, off by default; never public, never `:6100`) |
| `2222` | TCP | `127.0.0.1` | **photon-sshd**: admin shell / SFTP. Not Windows OpenSSH. Oracle `-R :2222` points here. |

> **LAN Access Note:** By default, Photon Server binds to all network interfaces (`0.0.0.0`). Ensure that firewall rules (e.g., `ufw`, `iptables`, or AWS/cloud security groups) allow inbound TCP traffic on ports 6100, 6500, 5400, 6200, and 6700, plus inbound UDP on 6200, as needed for your network topology.

---

## Runtime Configuration

Configuration is supported via command-line flags and environment variables. Command-line flags take precedence over environment variables.

| CLI Flag | Environment Variable | Default | Description |
|----------|----------------------|---------|-------------|
| `-host`, `-bind` | `PHOTON_HOST` | `0.0.0.0` | Bind IP/interface address |
| `-ingress-port`, `-ingress` | `PHOTON_INGRESS_PORT` | `6100` | Ingress TCP port |
| `-client-port`, `-client` | `PHOTON_CLIENT_PORT` | `6500` | Client fan-out TCP port |
| `-observer-port`, `-observer` | `PHOTON_OBSERVER_PORT` | `5400` | Observer telemetry TCP port |
| `-video-ingress-port`, `-video-ingress` | `PHOTON_VIDEO_INGRESS_PORT` | `6200` | RTP/H.264 UDP+TCP ingress port |
| `-video-client-port`, `-video-client` | `PHOTON_VIDEO_CLIENT_PORT` | `6700` | Video live/replay TCP port |
| `-video-udp-port`, `-video-udp` | `PHOTON_VIDEO_UDP_PORT` | `6800` | Live video UDP fan-out (PVU1 NALs) |
| `-data-dir`, `-storage-path` | `PHOTON_DATA_DIR` | `./data` | Path to persistent storage directory |
| `-tsdb-dir` | `PHOTON_TSDB_DIR` | `<data-dir>/tsdb` | Telemetry store root |
| `-dbc` | `PHOTON_DBC` | next to binary / `photon-next/assets/dbc` | DBC for the live TSDB writer |
| `-live-writer` | `PHOTON_LIVE_WRITER` | `photon-index` next to binary | Path to `photon-index`, or `off` |
| `-ui` | `PHOTON_UI` | `photon-backend` next to binary | Local UI backend, or `off` |
| `-password` | `PHOTON_PASSWORD` | random at start | Shared secret for fan-out, video, observer |

---

## Building from Source

### Prerequisites
- [Go](https://go.dev/) (version 1.24 or later)
- (Optional) GCC for C CANP test clients

### Build Binaries

```bash
# Build server binary
go build -o photon-server ./server

# Build observer TUI binary
go build -o photon-observer ./observer
```

---

## Local & LAN Execution

### Running the Server

```bash
# Run with defaults (bind 0.0.0.0, CAN 6100/6500, observer 5400, video 6200/6700, data in ./data)
./photon-server

# Run with custom ports, data directory, and TSDB writer
./photon-server -host 0.0.0.0 -data-dir /var/lib/photon/data -dbc ../photon-next/assets/dbc/carcan_telemcan.dbc -live-writer ../photon-next/.artifacts/backend/photon-index.exe

# Run using environment variables
export PHOTON_HOST=0.0.0.0
export PHOTON_DATA_DIR=/mnt/storage/photon
./photon-server
```

### Running the Observer TUI

The Observer provides a real-time terminal dashboard showing ingress throughput, queue latency, memory usage, connected client lag, and CAN packet decoding.

```bash
# Connect to local server (defaults to 127.0.0.1:5400)
./photon-observer

# Connect to a remote self-hosted server on LAN
./photon-observer 192.168.1.100:5400

# Connect using the -addr flag
./photon-observer -addr 192.168.1.100:5400

# Connect using environment variable
export PHOTON_OBSERVER_ADDR=192.168.1.100:5400
./photon-observer
```

---

## Storage & Persistence

Two stores on this PC, one timeline. `photon-server` starts `photon-index --live-fanout 127.0.0.1:<client-port>` so the TSDB is written here. The laptop must not write a second store (`--no-live-tail`). Leftover VSD1 files under `segments/` are not queried.

```
data/
├── tsdb/                     # telemetry store (photon-index --live-fanout)
│   ├── catalog.json
│   └── <session>/
│       └── video.vpt         # GOP pointers into the video store
└── video/
    ├── video.vpt             # live pointer table
    └── segments/             # compressed H.264 elementary stream
        ├── 000000000001.h264
        ├── 000000000001.idx
        └── ...
```

Place `photon-index` next to this binary (or pass `-live-writer`). Saturday check: `data/tsdb/catalog.json` grows on the PC.

- **Crash-Resilience:** On startup, the server automatically inspects and recovers existing segments from `segments/` and `video/segments/`, restores sequence indexes, and resumes appending to the active log file. Video indexes are rebuilt from the `.h264` elementary stream if an `.idx` is missing or truncated.
- **Compaction:** Inactive segments are periodically compacted based on client progress and disk watermarks.
- **Graceful Shutdown:** On `SIGINT` (Ctrl+C) or `SIGTERM`, the server drains in-flight ingestion packets, flushes pending segment writes to disk, unblocks downstream readers, and cleanly closes all open file descriptors.

---

## Video

The car/pit camera sends **H.264 over RTP/UDP** (payload type 96), matching the legacy Photon client's depacketization (single NAL, FU-A, optional STAP-A, Annex-B start codes). The server **never re-encodes**: it depacketizes RTP into Annex-B NAL units and appends the already-compressed elementary stream to disk.

### Ports

| Role | Default | Transport |
|------|---------|-----------|
| Ingress | `:6200` | UDP RTP/H.264, plus TCP length-prefixed RTP for the Oracle pipe |
| Clients | `:6700` | TCP framed NALs (live or replay / locate / range) |
| Live fan-out | `:6800` | UDP framed NAL fragments (`PVU1`). Subscribe with PHAU + cmd `1`. |

### Storage layout

Each video segment is a pair of files under `data/video/segments/`:

| File | Contents |
|------|----------|
| `NNNNNNNNNNNN.h264` | Raw Annex-B elementary stream (`00 00 00 01` + NAL). Independently decodable: SPS/PPS are copied to the start of each new segment and cached in the index header. |
| `NNNNNNNNNNNN.idx` | Compact binary index (magic `VIDX` / `0x56494458`). Fixed 2048-byte header (wall-clock start/end unix ms, SPS, PPS) followed by 24-byte NAL records: `timestampMs u64`, `offset u64`, `size u32`, `flags u8`, `nalType u8`, reserved 2 bytes. |

Segments rotate at **64 MiB** or **60 s**, preferring the next IDR so a file does not split mid-GOP (hard cap 128 MiB / 120 s). Keyframe byte offsets and timestamps live in the in-memory index (rebuilt from `.idx` on open).

### Client framing (server → client)

Every NAL is one frame. All integers are **big-endian**.

```
offset  size  field
0       4     payloadLen     uint32   length of the NAL payload only
4       8     timestampMs    uint64   wall-clock unix milliseconds
12      1     flags          uint8    bit0 = keyframe (IDR), bit1 = config (SPS/PPS)
13      N     payload        bytes    Annex-B NAL including 00 00 00 01
```

Total size on the wire: `13 + payloadLen`.

### Subscribe header (client → server)

Immediately after TCP connect, the client writes **9 bytes**:

```
offset  size  field
0       1     mode           uint8    0 = live, 1 = replay
1       8     startTimeMs    uint64   replay start (unix ms); ignored for live
```

### Live vs replay

**Live (`mode = 0`):** the server sends the current SPS and PPS (config flags), then the most recent IDR, then follows the live tail. Slow clients are not buffered without bound: a write timeout (~250 ms) or lag of more than ~45 NALs drops the client back to the latest keyframe (plus fresh SPS/PPS).

**Replay (`mode = 1`):** the server finds the last IDR with `timestampMs <= startTimeMs` (or the first IDR if `T` is earlier than any keyframe), sends SPS/PPS, then streams stored NALs from that keyframe to the end of recorded data as fast as the client reads. The client paces display using `timestampMs`. Replay does not automatically switch to live; reconnect with `mode = 0` to follow the tail.

### Example client (Go)

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

---

## Self-Host Deployment

### Option A: Docker & Docker Compose (Recommended)

1. Start the container with persistent storage:

```bash
docker compose up -d
```

2. View server logs:

```bash
docker compose logs -f
```

3. Stop the container gracefully:

```bash
docker compose down
```

### Option B: Linux Systemd Service

1. Copy the built binary to `/usr/local/bin`:

```bash
sudo cp photon-server /usr/local/bin/
```

2. Create system user and data directory:

```bash
sudo useradd -r -s /bin/false photon
sudo mkdir -p /var/lib/photon/data/segments
sudo chown -R photon:photon /var/lib/photon
```

3. Install the systemd unit file:

```bash
sudo cp photon-server.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now photon-server.service
```

4. Check service status and logs:

```bash
sudo systemctl status photon-server.service
journalctl -u photon-server.service -f
```

---

## CANP Protocol & C Test Utilities

C-based source and sink test utilities are available in the `canp/` directory:

```bash
# Ingest synthetic CAN packets into the server (defaults to 127.0.0.1:6100)
cd canp
./run_source_test.sh

# Or specify a remote target host:
PHOTON_SERVER_IP=192.168.1.100 ./run_source_test.sh

# Read streamed CAN batches from the server fan-out port (defaults to 127.0.0.1:6500)
./run_sink_test.sh
```

---

## Running Tests

```bash
go test -v ./...
```
