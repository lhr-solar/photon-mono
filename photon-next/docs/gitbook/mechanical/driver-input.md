---
description: Throttle, brake pressure, steering, and the motor power setpoint.
icon: gauge-high
---

# Driver Input

Driver Input is the first Mechanical tab. It has two subtabs. **Distributions** is the one that opens first.

## Distributions

**Driver controls** plots four traces on one time chart:

| Trace | CAN signal | Unit |
| --- | --- | --- |
| Throttle % | `Pedal_Status.AccelPedal_Main_Pos` | % |
| Brake front psi | `Brake_Pressure_1.Brake_Pressure` | psi |
| Brake rear psi | `Brake_Pressure_2.Brake_Pressure` | psi |
| Steering deg | `LWS_Standard.LWS_Angle` | deg |

Under that chart, three histograms show how much of this 15 second window sat at each throttle, front-brake, and steering value.

{% hint style="warning" %}
The shared Driver controls axis mixes percent, psi, and degrees. Use it to see that the channels move together. Use the histograms, or a [Plots](../plots.md) chart, when you need one unit on a scale.
{% endhint %}

On a stop, throttle and both brake pressures should sit quiet, and steering should sit near the straight-ahead the wheel was calibrated to. Into a brake zone, front and rear pressure should rise together. A pressure that stays flat while the pedal moves belongs on [Dashboard](../dashboard.md), on the brake fault flags.

## Power vs throttle

**Power vs throttle** replaces the histograms with a scatter of motor power command (%) against throttle (%).

The line under the chart is the rule: motor power command is the setpoint, not measured shaft power or torque. The CAN signal is `MC_PowerCommand.MC_MotorPowerSetpoint`.

That setpoint is a Dashboard channel, so it is not included in a Mapped CSV. Add it by hand when you [export](../export.md).

## Brake pedal versus brake pressure

Throttle on this chart is pedal position in percent. The brake traces are pressure in psi, from the two pressure sensors. Pedal position for the brake is `Pedal_Status.BrakePedal_Main_Pos`, and it is not drawn here. Put that on a Plots chart next to the two pressures when you are checking the pedal against the lines.
