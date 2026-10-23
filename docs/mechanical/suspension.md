---
title: Suspension
---

# Suspension

**Suspension travel** plots three channels in millimeters:

| Trace | CAN signal |
| --- | --- |
| FL | `Suspension_FL.SUS_Data1` |
| FR | `Suspension_FR.SUS_Data1` |
| Rear | `Suspension_Back.SUS_Data1` |

The DBC scales `SUS_Data1` to about 0–78 mm. This tab plots `SUS_Data1` only. `SUS_Data2`, `SUS_Data3`, and `SUS_Data4` are on the same three messages and are not drawn here. Add them from [Plots](/plots.html), or include them in a [CSV](/export.html), when the test needs them.

Rear is one sensor, `Suspension_Back`. It is not a left and a right.

Compare FL and FR through a symmetric corner or a straight-line brake. A channel that never leaves one value across a run where the wheel is visibly moving is the first thing to check at the sensor and the linkage. Confirm the other two channels are moving in that same window before you call one of them stuck.
