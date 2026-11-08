# CAN spec source

Copied from the CAN specification in the firmware repo. Treat these files as read-only here.

- Repository: `lhr-solar/2027-Firmware`
- Branch: `dev/PLAT-can-codegen`
- Commit: `8db8eb1ded33e6f68ccecd1e571ad6be6678a658` (2026-11-04, "Group TelemetryLink message IDs")
- Files: `can/topology.yaml` and `can/dbc/*.dbc`. `bootloading.dbc` is empty and not copied. `dbc/README.md` documents the timestamp sync and sensor sample formats.

The layout matches the firmware repo, so the `dbc:` paths in `topology.yaml` resolve from this folder.

Do not edit these files here. Change the spec in the firmware repo, then copy the new commit over and update the commit above:

```sh
SHA=<commit>
R=repos/lhr-solar/2027-Firmware/contents/can
gh api "$R/topology.yaml?ref=$SHA" -H "Accept: application/vnd.github.raw" > topology.yaml
for f in CarCAN ChargerCAN DAqCAN ElconCAN LightingCAN MPPTCAN MotorCAN SteeringCAN TelemetryLink; do
  gh api "$R/dbc/$f.dbc?ref=$SHA" -H "Accept: application/vnd.github.raw" > "dbc/$f.dbc"
done
gh api "$R/dbc/README.md?ref=$SHA" -H "Accept: application/vnd.github.raw" > dbc/README.md
```

Sample rates for multi-sample sensor messages are not in the DBCs. They are kept with the decoder.
