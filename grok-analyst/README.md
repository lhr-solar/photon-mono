# grok-analyst (WP-H)

Loopback sidecar for Photon Analysis compute. Not ingest. Not Pit.

- Bind: `127.0.0.1:18765` (no public port; refuses `:6100/:6500/:6700`)
- HTTP: `POST /jobs`, `GET /jobs/{id}`, `POST /jobs/{id}/cancel`
- **One container per job**, destroyed on finish / timeout / cancel
- Job mounts: that job's scratch at `/job` only. No `docker.sock`, no `data/tsdb`, no `data/video`, no repo
- Limits: 1 CPU, 1 GB RAM, 64 PIDs, `--network=none` (default-deny)
- Windows path ACLs are **not** the security boundary
- Audit log: `ts,clientId,jobId,sessionId,t0,t1,signalCount,recipe,status,elapsedMs,bytesIn,bytesOut` (no secrets)
- Adapter is a stub inside the job image (`summarize_window` / `threshold_events` / `freeform`). Live Grok is later. `--trust-all-tools` is forbidden.

## Run

```bat
grok-analyst\run-grok-analyst.cmd
```

Or: `go run ./cmd/grok-analyst --bind 127.0.0.1:18765`

Build the job image first: `docker build -t grok-analyst-job:local -f Dockerfile.job .`

Compose (host publish still loopback; job image via profile): `docker compose --profile build-job build`

## Photon backend

`--analysis-compute` is **off** by default. Enable with:

```
photon-backend --analysis-compute 127.0.0.1:18765
```

If the sidecar is down, `analysis_compute_request` returns `status=failed` `code=offline`. Analysis / history / Pit are unchanged.

## Tests

```bat
go test ./...
```

Isolation tests require a running Docker engine. They prove job A cannot read client B's scratch, host TSDB, video, docker.sock, or the repo.

C++: `photon-tests` covers sidecar-down → offline and no TSDB write.
