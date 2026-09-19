# photon/daemon (`photon_daemon`)

Native session hub: sources (daq_tcp / log_replay / pcan), WS accept, control dispatch, streams, live tail, history, track-sim jobs.
Builds `photon-backend` and `photon-index` (VSD1→TSDB / `--live-fanout`).

**May link:** every system library except `photon_ui` (plus `photon-export-lib` for CSV).

**Must not:** include `photon_ui` headers or ImGui. UI talks WebSocket; it never links this library.
