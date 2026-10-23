---
title: Signal names
---

# Signal names

Plots and the CSV picker search the catalog as `Message.Signal`. Type the message, the signal, or a piece of either.

## On the Mechanical tabs

| What you see | Search |
| --- | --- |
| Throttle % | `Pedal_Status.AccelPedal_Main_Pos` |
| Front brake psi | `Brake_Pressure_1.Brake_Pressure` |
| Rear brake psi | `Brake_Pressure_2.Brake_Pressure` |
| Steering deg | `LWS_Standard.LWS_Angle` |
| Motor power setpoint % | `MC_PowerCommand.MC_MotorPowerSetpoint` |
| Body long g | `IMU_B_Accel.Accel_X` |
| Body lat g | `IMU_B_Accel.Accel_Y` |
| FL long / lat g | `IMU_Accel_FL.Accel_X` / `Accel_Y` |
| FR long / lat g | `IMU_Accel_FR.Accel_X` / `Accel_Y` |
| Body yaw, deg/s | `IMU_Gyro_Back.Gyro_Z` |
| FL / FR / rear travel | `Suspension_FL.SUS_Data1`, `Suspension_FR.SUS_Data1`, `Suspension_Back.SUS_Data1` |
| FL / FR wheel rpm | `WheelRPM_FL.RPM`, `WheelRPM_FR.RPM` |
| Vehicle speed, m/s | `MC_VelocityMeasurement.MC_VehicleVelocity` |

Photon converts that vehicle speed to mph for the Speed tiles. The CSV value stays in m/s.

## Often added on Plots

| What you need | Search |
| --- | --- |
| Brake pedal % | `Pedal_Status.BrakePedal_Main_Pos` |
| Extra suspension channels | `Suspension_FL.SUS_Data2` and the same Data3 and Data4 names on FL, FR, and Back |
| Front gyro, not used by the Yaw tab | `IMU_Gyro_FL.Gyro_Z`, `IMU_Gyro_FR.Gyro_Z` |
| Motor °C | `MC_Motor_TempMeasurement.MC_MotorTemp` |
| Heatsink °C | `MC_Motor_TempMeasurement.MC_HeatsinkTemp` |
| Coolant | `Coolant_Temperature.Coolant_Temperature_1` and `_2` |
| Pump flow, L/min | `Pump_Status.FlowRate_1` and `FlowRate_2` |
| Pump duty % | `Pump_Status.Pump_DutyCycle` |
| Steering fault flag | `LWS_Standard.LWS_Fault` |

## Not on this car's charts

- Rear wheel rpm. It is not in the DBC.
- A second rear suspension corner. Rear is `Suspension_Back` only.
- Measured motor power. `MC_MotorPowerSetpoint` is the command.

Pack voltage, cell taps, and MPPT power are real signals. They belong to [Pit](/pit.html) and [Dashboard](/dashboard.html), and they are valid to export beside a chassis channel when the question is whether the car pulled power there.
