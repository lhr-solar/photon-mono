# src/store (`photon_store`)

On-disk telemetry: TSDB (+ live tail), VSD1 log index, video index (VPT), and log-session catalogs.

**May link:** `photon_decode`, `photon_wire`, `photon_base`, `photon_net` (SHA-1 hashing only).

**Must not:** listen/connect sockets, WebSocket servers, PCAN, or UI. Hashing via `net` is fine; no `net.cpp` / `websocket.cpp` usage.
