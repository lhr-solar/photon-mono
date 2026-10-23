---
title: Pit
---

# Pit

Pit is the default tab. It is the "is the car alive" page. Mechanical uses it to confirm the run is real before reading a chassis chart, and to see motor temperature and brake pressure as single numbers.

The top two rows are:

| Row | Tiles |
| --- | --- |
| First | Pack SOC, pack voltage, pack current, motor rpm |
| Second | Speed in mph, motor temperature, inverter temperature, array power |

Under those tiles:

- **BPS** reads OK or FAULT.
- Contactors: HV+, HV-, array precharge, array, motor precharge, motor.
- **Charge**, **Regen**, and **Ready**. Ready is the motor-ready flag.
- **MPPT A**, **MPPT B**, and **MPPT C**, each as watts from input voltage times input current.
- Ignition: Off, Array, Motor.
- **Aux** voltage and current.
- **Pedals**: throttle in percent, front brake pressure, rear brake pressure.

The bottom of the page is the 3D car, the camera, and the 32-tap cell strip. A cell that shows `-- V` has no voltage sample, or the sample is at or below 0.05 V. Temperature can still show on that cell. Cell detail is an electrical readout. Use it as context when a mechanical limit coincides with a battery limit.

::: tip
Speed here is motor-controller vehicle velocity in mph. It is the same source as the Dynamics Speed tile, and it is not the wheel-rpm speed.
:::

If Pit is `--` across the board while LOGS says LIVE, the car is not in this page's data. Go back to [Logs](/logs.html) and confirm you are not paused on an old run.
