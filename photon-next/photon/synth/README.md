# photon/synth (`photon_synth`)

Pure analysis: the track-sim speed-profile solver. Tracks arrive as JSON text; no filesystem calls.

**May link:** `photon_base`.

**Must not:** I/O, sockets, ImGui, store, or hardware. If it opens a file or a window, it belongs in daemon/ui, not here.
