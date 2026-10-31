# Photon Server

Photon Server is the ingest server on the **PC** (CANP, video store, fan-out, observer). It starts `photon-index --live-fanout` so the TSDB is written on this machine, and a **local UI** (`photon-backend` + `photon.html`) pointed at loopback fan-out. That local UI is how you watch the car when you are sitting at the PC. The laptop runs its own client when you want a different source (USB PCAN, another host). Oracle is a pipe *to* this process (public IP + WireGuard DNAT).

Telemetry rides the lightweight CANP wire protocol over TCP. Incoming H.264 from the car/pit is accepted as RTP/UDP, stored as a **separate** compressed elementary stream (never re-encoded), and served via GOP pointers. See [docs/video-protocol.md](docs/video-protocol.md) for the wire format.

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
                                                 │
                                                 ▼
                                          TCP :6700  video_locate / range
                                          + photon-observer :5400
```

## Ports

| Port | Protocol | Bind | Purpose |
|------|----------|------|---------|
| `6100` | TCP | `0.0.0.0` | Ingress: CANP telemetry |
| `6500` | TCP | `0.0.0.0` | Fan-out: downstream clients |
| `5400` | TCP | `0.0.0.0` | Observer: JSON telemetry / TUI |
| `6200` | UDP+TCP | `0.0.0.0` | Video ingress: RTP/H.264 |
| `6700` | TCP | `0.0.0.0` | Video clients: live / replay / locate |
| `6800` | UDP | `0.0.0.0` | Video live fan-out (PVU1 NALs) |
| `8765` / `8080` | TCP | `127.0.0.1` | Local UI (photon-backend WS / photon.html) |
| `2222` | TCP | `127.0.0.1` | photon-sshd admin shell / SFTP |

## Build

```bash
go build -o photon-server ./server
go build -o photon-observer ./observer
```

## Run

Production is the PC scheduled task: `scripts/install-autostart.ps1` registers a watchdog that keeps `photon-server` and `photon-sshd` running (`scripts/ensure-running.ps1`).

By hand:

```bash
./photon-server            # defaults are correct; set PHOTON_PASSWORD or one is generated
./photon-observer          # TUI against 127.0.0.1:5400
```

`photon-server -h` lists the remaining flags (ports, data dir, DBC path). You should not need them; the defaults are the deployed topology.

## Storage

Two stores on this PC, one timeline. `photon-server` starts `photon-index --live-fanout` so the TSDB is written here; the laptop must not write a second store (`--no-live-tail`).

```
data/
├── tsdb/                     # telemetry store (photon-index --live-fanout)
│   ├── catalog.json
│   └── <session>/video.vpt   # GOP pointers into the video store
└── video/
    ├── video.vpt             # live pointer table
    └── segments/             # compressed H.264 elementary stream (.h264 + .idx)
```

On startup the server recovers existing segments, rebuilds missing video indexes from the `.h264` streams, and resumes appending. `SIGINT`/`SIGTERM` drain and flush cleanly. Saturday check: `data/tsdb/catalog.json` grows on the PC.

## Tests

```bash
python ../tests/go/run.py test ./server ./observer ./sshd ./cmd/...
```

The test files live in `tests/go/` at the repository root, grouped by package. The runner uses a temporary Go overlay so tests retain access to package internals without changing the production source tree. From the repository root, `photon test go` runs vet and these tests. Bare `go test` does not load the relocated tests. To include the unfinished cache package, run `python ../tests/go/run.py test ./...` (its failures are currently expected).
