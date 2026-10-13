# Photon

`photon-server` owns ingest, raw recordings, DBC decoding, Influx queries, browser sessions, and replay. `photon-next` is the display client. See [the Go replay service](photon-server/readme.md) for configuration, DBC uploads, and replay.

```text
photon build
photon server
photon server ingest PORT
photon ui local
photon ui daq HOST[:PORT]
photon ui oracle
photon ui pcan
photon ui log RECORDING_ID
photon station
```

`server` accepts DAQ CANP on `6100` and serves the authenticated UI and `/ws` on `8080`. `server ingest PORT` changes CAN ingress. `ui local` and `ui daq HOST[:PORT]` connect directly to Go; the client port defaults to `8080`. `ui oracle` opens the public browser page. `ui pcan` starts a local Go server and USB capture adapter; local InfluxDB is required for plots. `ui log RECORDING_ID` opens an existing recording on the local Go server. `station` runs Go and the native display together.

`photon build` writes the Go server, `photon-ui`, the PCAN adapter, the offline TSDB exporter, and one `assets\` tree into `build\`. The DBC source is `photon-next\assets\dbc`. The page, from `photon-next\web\scripts\build.ps1`, is copied to `build\ui\photon.html`.

`PHOTON_PASSWORD` defaults to `laptop` in the launcher. `PHOTON_DATA_DIR` defaults to `data\` in this repo. Flags after the use case are passed through to `photon-server` or `photon-ui`. Influx credentials stay on Go; use `photon_dev` for validation.

Select a previously registered default DBC with `photon server -dbc-hash <SHA-256 hash>` or `PHOTON_DBC_HASH`. Startup loads saved DBCs from `<data-dir>/dbcs/`; without a selected default, new recordings remain raw until a DBC is applied. See the server README for upload and selection details.

`Photon/` stays its own repo. That tree is the native desktop client and the embedded sharepoint.

InfluxDB 3 Core and Grafana are installed programs on the PC. They bind to `127.0.0.1`. Dashboard JSON for those panels belongs in `dashboards/`.

This checkout is the server and the UI. There is no separate `photon-server` or `photon-next` repo beside it.
