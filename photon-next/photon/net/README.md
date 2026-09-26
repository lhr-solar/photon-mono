# photon/net (`photon_net`)

Sockets, WebSocket server/client, RFC6455 handshake, SHA-1/Base64, and client auth.
`net.cpp` / `websocket.cpp` are native-only (omitted from the WASM library).

**May link:** `photon_base`, `photon_wire`.

**Must not:** DBC decode, TSDB/VSD1, PCAN, daemon session logic, or UI. Bytes on the wire only.
