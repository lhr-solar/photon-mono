# HighNoon 2026 DBC

The car running now (HighNoon 2026, CANP v3 sender). This is the server's default
`-dbc-dir` (`dbc/highnoon2026`).

- `HighNoon.dbc` is a copy of `examples/brainerd-30s/HighNoon.dbc`. It decodes all but a
  few hundred of the 71,930 frames in that recording.
- Standard IDs with bit 12 set (0x1xxx) are can0 messages: the v3 sender marks can0 frames
  that way, and the decoder puts the bit back on can0 frames when a DBC uses it.
- `photon-next/assets/dbc/carcan_telemcan.dbc` (the client's copy) is older: it lacks 30
  messages (BPS cell voltages/temperatures, pack current, Elcon, lighting, GPS_*) and has 5
  this one lacks (Lattitude_GPS, Longitude_GPS, Speed_GPS, Valid_GPS, Array). The two
  cannot be loaded together: Display_Status (0x680) differs between them.

HighNoon 2027 specs: `dbc/spec/dbc` (firmware `dev/PLAT-can-codegen`) and
`dbc/spec-cdr/dbc` (firmware `CAN/cdr`). Pick one with `-dbc-dir` / `PHOTON_DBC_DIR`.
