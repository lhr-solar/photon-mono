---
description: Body yaw rate from the rear gyro.
icon: compass
---

# Yaw

**Body yaw** is `IMU_Gyro_Back.Gyro_Z` in deg/s.

The line under the tab is the current limit: a front-versus-rear yaw comparison needs calibrated front yaw signals, and those signals are not on this chart. Front gyro Z exists on `IMU_Gyro_FL` and `IMU_Gyro_FR`, and this tab does not plot them.

Use the body rate next to Race context or [Live View](../live-view.md). Turn-in and exit should line up with the camera and with the steering trace on [Driver Input](driver-input.md). Read the sign the same way you read the IMU scales on [Chassis IMUs](chassis-imus.md): the DBC scale is what the chart shows.

To put a front gyro on a chart, add `IMU_Gyro_FL.Gyro_Z` or `IMU_Gyro_FR.Gyro_Z` from [Plots](../plots.md). Treat that trace as the sensor output, not as a calibrated front yaw, until the install is checked.
