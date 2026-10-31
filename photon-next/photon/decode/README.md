# photon/decode (`photon_decode`)

DBC text parse, CAN frame encode/decode, sample interpolation, and per-frame times for a CANP batch (`frame_clock.hpp`).

**May link:** `photon_base`, `photon_wire` (the CANP batch type).

**Must not:** sockets, TSDB/VSD1, protocol servers, hardware, or UI. Bit-walk and catalogs only — no store and no wire I/O.
