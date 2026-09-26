# photon/wire (`photon_wire`)

Protocol v1 binary framing, JSON control-plane codec, CANP v3, and H.264 video frame headers.

**May link:** `photon_base`, `photon_synth` (track-sim control types).

**Must not:** I/O — no sockets, files, threads, or hardware. Encode/decode bytes and structs only.
