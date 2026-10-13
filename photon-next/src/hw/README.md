# src/hw (`photon_hw`)

Native-only hardware drivers. Today: USB PCAN (`PCANBasic.dll`, dynamic load).

**May link:** `photon_wire`, `photon_base`.

**Must not:** UI, TSDB, WebSocket servers, or daemon session/control dispatch. Future socketcan/serial stays here; nothing else.
