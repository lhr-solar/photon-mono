---
title: Wheels
---

# Wheels

The Wheels tab shows front-left and front-right rpm for this 15 second window, plus a histogram of each. The status line is the latest FL and FR rpm.

| Trace | CAN signal |
| --- | --- |
| FL | `WheelRPM_FL.RPM` |
| FR | `WheelRPM_FR.RPM` |

The tab says **Rear wheel RPM unavailable**. Rear wheel rpm is not in the current DBC, so there is no rear trace to add.

On the [Dynamics](/mechanical/dynamics.html) tab the same rpm is also drawn as mph. That conversion uses a 21.56 inch rolling diameter. It is a wheel speed. It is separate from the motor-controller vehicle speed on the Speed tile, which comes from `MC_VelocityMeasurement.MC_VehicleVelocity`.

A straight-line split between FL and FR that grows under braking is worth a look at the sensor, the brake, and the tire. Confirm both channels are updating before you call it a grip difference. A channel stuck at one rpm while the other tracks speed is a sensor problem first.
