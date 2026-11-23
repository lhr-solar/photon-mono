# HighNoon 2026 DBC

The car running now (HighNoon 2026, CANP v3 sender). This is the server's default
`-dbc-dir` (`dbc/highnoon2026`).

`HighNoon.dbc` (130 messages) merges the two 2026 DBCs we had (2026-11-23):

- Base: `examples/brainerd-30s/HighNoon.dbc` (127 messages: BPS cell voltages and
  temperatures, pack current, Elcon, lighting, ...).
- Every message `photon-next/assets/dbc/carcan_telemcan.dbc` (the client's copy) also
  defines uses the client's layout: the message names the UI uses (`Lattitude_GPS`,
  `Longitude_GPS`), calibrated scales and units (suspension mm, ride height cm,
  temperature/humidity 0.01), and the IMU axes as mounted (Z is the axis that reads
  gravity in the Brainerd recording).
- Except where the Brainerd recording shows the client copy is wrong, which keep
  HighNoon's layout: `Display_Status` 0x680 (sent as 4 bytes: counter, cameras, frame rate,
  CPU temperature), `WheelRPM_FR`/`WheelRPM_FL` 0x14A0/0x14D0 (sent with DLC 8), and
  `Pedal_Accel_ADC` 0x754 (the client copy has no signals).
- Added from the client copy: `Speed_GPS` 0x10C2, `Valid_GPS` 0x10C3, `Array` 0x1600.

On the Brainerd recording it decodes 72,279 rows from 71,930 frames (HighNoon.dbc alone
72,224; the client copy 70,522).

Standard IDs with bit 12 set (0x1xxx) are can0 messages: the v3 sender marks can0 frames
that way, and the decoder puts the bit back on can0 frames when a DBC uses it.

HighNoon 2027 specs: `dbc/spec/dbc` (firmware `dev/PLAT-can-codegen`) and
`dbc/spec-cdr/dbc` (firmware `CAN/cdr`). Pick one with `-dbc-dir` / `PHOTON_DBC_DIR`.
