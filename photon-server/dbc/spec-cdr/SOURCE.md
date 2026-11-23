# CAN spec source (CAN/cdr)

HighNoon 2027 CAN spec from the firmware repo's `CAN/cdr` branch: newer than `dbc/spec`
(10 commits ahead of `dev/PLAT-can-codegen`). Not the default yet; select it with
`-dbc-dir dbc/spec-cdr/dbc` (or `PHOTON_DBC_DIR`). Treat these files as read-only here.

- Repository: `lhr-solar/2027-Firmware`
- Branch: `CAN/cdr`
- Commit: `8183896194a67ce2adf79bfc96fd57850b480d2c` (2026-11-18, "Merge pedal box signals into dashboard status")
- Files: `can/topology.yaml`, `can/dbc/*.dbc` (`bootloading.dbc` is empty and not copied), `can/dbc/README.md`.

Buses: CarCAN, DAqCAN, ChargerCAN, ElconCAN, TelemetryLink, and COTSCAN (motor controller,
MPPT A/B/C and the steering angle sensor, which were MotorCAN, MPPTCAN and SteeringCAN).
LightingCAN is gone. No ID is defined on two buses.

To update, change the spec in the firmware repo, then:

```sh
SHA=<commit>
R=repos/lhr-solar/2027-Firmware/contents/can
gh api "$R/topology.yaml?ref=$SHA" -H "Accept: application/vnd.github.raw" > topology.yaml
for f in COTSCAN CarCAN ChargerCAN DAqCAN ElconCAN TelemetryLink; do
  gh api "$R/dbc/$f.dbc?ref=$SHA" -H "Accept: application/vnd.github.raw" > "dbc/$f.dbc"
done
gh api "$R/dbc/README.md?ref=$SHA" -H "Accept: application/vnd.github.raw" > README.md
```
