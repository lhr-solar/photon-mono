---
description: A short pass through Photon after a stint.
icon: clipboard-check
---

# Test day

Do this after a stint, or while the car is live and someone asks what the chassis did.

1. [Sign in](open.md). Open **LOGS**. Stay on **Live** at the car, or open the run you were asked to review. Wait until the mode is LIVE, PLAY, or PAUSE, not BUFFERING.
2. Glance at [Pit](pit.md). Speed, motor rpm, and the pedal card should be updating. If they are `--`, you are on the wrong mode or the car is not in the log.
3. Open [Mechanical](mechanical/README.md) and leave **Driver Input** on **Distributions**. Confirm throttle, both brake pressures, and steering move when the driver moves them.
4. Step through [Chassis IMUs](mechanical/chassis-imus.md), [Suspension](mechanical/suspension.md), [Yaw](mechanical/yaw.md), and [Wheels](mechanical/wheels.md). Write down any channel that stays `--` or flat while the others move.
5. Open [Dynamics](mechanical/dynamics.md). Pick the IMU. After a representative stint, open **Peak times** and scrub LOGS to the largest `|g|` and the highest wheel speed. Watch the camera in Race context, or open [Live View](live-view.md), at those clocks.
6. If a brake trace looks wrong, press **D** on [Dashboard](dashboard.md) and read the pedal faults next to the pressures. If motor temperature is high, read coolant and pump flow on that same debug page.
7. Build a [Plots](plots.md) board only for the comparison the tabs cannot show: one unit per axis, `SUS_Data2` and the rest, or a window longer than 15 seconds.
8. [Export](export.md) **10 Hz** and **Mapped** for the stint. Add the motor power setpoint, and any extra suspension channels, with **Selected**.

## Say which speed you mean

Two speeds are on the page.

- **Speed** on Pit and on the Dynamics tiles is motor-controller vehicle velocity, shown in mph. The CSV column is m/s.
- **FL** and **FR** on Dynamics are wheel rpm converted with a 21.56 inch rolling diameter.

Quote the one you used.

## What this page will not show

- Rear wheel rpm.
- A calibrated front yaw next to the rear yaw.
- Measured motor power or torque. The power chart is the command setpoint.
- Left and right rear suspension as two traces.
- A Mechanical chart longer than 15 seconds. That is a Plots window.
