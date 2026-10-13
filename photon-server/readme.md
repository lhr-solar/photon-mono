# Photon Server

Go owns CAN ingest, durable raw recordings, DBC decoding, Influx summaries and queries, browser sessions, replay, exports, events, and video delivery. Photon Next connects directly to `/ws`. The old C++ session daemon and its process launchers have been removed.

## Code walkthrough

Follow one CAN frame through these files:

1. `server/main.go`: configuration, startup, listeners, shutdown; `readIngressConnection` reads each connection independently.
2. `wire/canp.go`: incremental CANP framing, payload validation, timestamps and flags.
3. `server/recordings.go`: source ownership, persist-before-delivery, DBC epochs, decoding workers, retries.
4. `recording/store.go`: VSD1 raw segments, crash recovery, timestamp index and bounded replay pages.
5. `cache/dbc.go`, `cache/decode.go`: immutable signal definitions, bit extraction, signed/IEEE values, mutable one-second tallies.
6. `influx/client.go`: acknowledged native writes, parameterized SQL, exact integer strings.
7. `server/browser.go`: authenticated HTTP and WebSocket connections; independent selection, cursor, speed and playback for every client.
History delivery, cancellation, numerical jobs and analysis bundles are also in `server/browser.go`.

Supporting features:

- `recording/registry.go`: validated DBC uploads, content hashes, original files and aliases.
- `server/http_ui.go`: UI assets, HTTP authentication and crash reports. This replaces `local_ui.go`; there is no proxy to another backend.
- `server/client.go`: compatibility TCP clients and their bounded RAM buffer. It does not write or delete recordings.
- `server/config.go`: runtime flags and registered default DBC selection.
- `server/events.go`: manual event marks, persisted thresholds and crossing detection.
- `server/pcan_adapter.go`: local capture adapter control and transmit arming/watchdog.
- `server/video_store.go`: compressed video storage and clock-aligned delivery.

## Run

From the repository root, `photon build`, then `photon server`. The Go server serves the page and `/ws` on HTTP port 8080. CANP ingress defaults to 6100. Configure InfluxDB 3 Core using `PHOTON_INFLUX_URL`, `PHOTON_INFLUX_DATABASE` and `PHOTON_INFLUX_TOKEN_FILE`. Credentials remain on the server. Use `photon_dev` for validation.

`POST /api/dbcs?name=car.dbc` accepts up to 4 MiB of DBC text. Upload registers it without applying it. `dbc_apply` starts a new live decoding epoch; `recording_redecode` creates a historical version for a finished raw recording. Earlier versions remain selectable.

Startup loads saved DBCs from `<data-dir>/dbcs/`. To select one for new recordings, pass `-dbc-hash <SHA-256 hash>` or set `PHOTON_DBC_HASH`; the hash is returned by the upload API and listed by `GET /api/dbcs`. An unknown hash fails startup. Without a configured hash, new recordings remain raw until a DBC is applied. The previous `-dbc` file option, `PHOTON_DBC` / `PHOTON_DBC_PATH`, and automatic bundled DBC loading have been removed. For a new installation, start without a default, upload the DBC, then restart with its hash or apply it through `dbc_apply`.

`workers` contains the USB capture adapter and offline TSDB exporter. They do not own browser connections. Track Sim and its numerical worker have been removed.

For a server review, start with the files in the walkthrough. `oracle/` and `workers/` contain deployment helpers and native tools. They support the running system and can be left out of the walkthrough.

## Checks

```powershell
go build ./...
go vet ./...
```

Test files have been removed from `photon-server`. Real Influx, physical PCAN and camera acceptance still require validation on the target environment. No production database was changed during the migration.

The optional `cmd/photon-import` and `cmd/photon-sshd` programs have been removed, along with their terminal UI and SSH implementation. Existing Go recordings and browser replay remain available. Old VSD1/TSDB import commands are no longer provided.
