---
title: Chassis IMUs
---

# Chassis IMUs

Chassis IMUs shows three charts for the current 15 second window:

- **Longitudinal acceleration** in g: Body, FL, and FR.
- **Lateral acceleration** in g: Body, FL, and FR.
- **Body lateral g**, a histogram of the body lateral trace.

| Trace | CAN signal |
| --- | --- |
| Body long / lat | `IMU_B_Accel.Accel_X` / `Accel_Y` |
| FL long / lat | `IMU_Accel_FL.Accel_X` / `Accel_Y` |
| FR long / lat | `IMU_Accel_FR.Accel_X` / `Accel_Y` |

X is drawn as longitudinal g and Y as lateral g, using the scale in the DBC. The front-left and front-right messages do not use the same sign on every axis as the body message. A corner trace that is a mirror of the body trace can be that scale. A corner trace that stays flat while the body trace moves is a missing sample or a dead sensor.

::: tip
Check the mount before you treat a sign difference as a handling problem. The Dynamics tab lets you look at one of these three IMUs alone, with a G-G. This tab is the comparison.
:::

Gyro X and Y are on the bus and are not drawn here. Body yaw rate is the [Yaw](/mechanical/yaw.html) tab, from `IMU_Gyro_Back.Gyro_Z`.
