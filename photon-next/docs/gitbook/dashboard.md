---
description: Driver cluster, and the debug page for brakes, cooling, and limits.
icon: gauge
---

# Dashboard

Dashboard is the driver cluster: speed, brake, and the fault banner. Mechanical opens it for three jobs.

## The cluster

The battery panel shows main pack state of charge, pack voltage, and current, plus aux voltage and current. It also shows heatsink temperature, motor temperature, bus voltage, bus current, drive velocity setpoint, drive current setpoint, and power command. Motor temperature turns a warning color above 70 °C and a fault color above 90 °C.

**MOTOR LIMITS ACTIVE** lists which motor-controller limit is on: PWM, motor current, velocity, bus current, bus voltage, or motor temperature. A chassis trace that flattens while this banner is up is often the controller limiting, not the linkage.

The cell row shows the lowest cell, the highest cell, and the hottest cell when those samples exist. A slot with no voltage at or below 0.05 V is left out of the min and max.

## Debug

Press **D** while Dashboard is open and the text is not in a search box. The page switches to **DEBUG**. Press **D** again to return to the cluster.

The left column is the mechanical checklist:

- Steering angle and whether `LWS_Fault` is clear.
- Horn, hazard, push-to-talk, cruise set, regen.
- Gear and turn signals.
- Accel and brake pedal, main and redundant, with their fault flags.
- Front and rear brake pressure, with `Brake_Pressure_1_Fault` and `Brake_Pressure_2_Fault`.

The right column includes motor-controller limits, VCU status, and **COOLING**:

- Coolant temperature 1 and 2, in °C.
- Flow 1 and 2, in L/min.
- Pump duty in percent, and FAULT or OK.

Use this page when a brake-pressure trace on Mechanical looks wrong. The fault flags are here, next to pedal percent and the two pressures. Use it when motor temperature on Pit is high and you need flow and coolant next to it.

The snapshot dropdown and **Clear Snapshots** are for holding a copy of this debug view. **[LIVE DATA]** is the live or replay playhead. Leave the dropdown there unless you meant to freeze a snapshot.
