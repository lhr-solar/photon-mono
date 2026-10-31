---
description: G-G diagram, wheel speed in mph, and session peaks.
icon: chart-scatter
---

# Dynamics

Dynamics is a tab inside Mechanical. It is the G-G page.

Race context on this tab is six tiles: Speed, Steer, Long G, Lat G, Yaw, and Throttle. Long G and Lat G follow the IMU you pick under the charts.

## Peaks

The peak row is for the run you have open and for the IMU selected in the dropdown. The choices are **IMU B**, **IMU FR**, and **IMU FL**.

| Readout | Meaning |
| --- | --- |
| FL, FR | Current wheel speed in mph, from wheel rpm and a 21.56 inch rolling diameter |
| pk FL, pk FR | Highest wheel speed seen in this run, same conversion |
| long | Most positive and most negative longitudinal g held for this run and IMU |
| lat | Same, for lateral g |
| \|g\| | Largest combined acceleration |

**Reset peaks** clears those held values. **Peak times** lists the clock time of each peak. Scrub the [LOGS](../replay.md) timeline to that clock to see the camera and the other charts at the same moment. Peaks restart when you change run or IMU.

## Charts

- **G-G** plots longitudinal g on X and lateral g on Y for the selected IMU. Rings are 0.5 g and 1 g. Points are colored by vehicle speed from `MC_VelocityMeasurement.MC_VehicleVelocity`: under 15 mph, 15–35 mph, and 35 mph and above. The view stays readable out to a few g so one spike does not shrink the rings.
- **Acceleration** is long g and lat g versus time for that IMU.
- **Wheel speed** is FL and FR in mph versus time.

`no IMU yet` or `no wheel speed yet` means this 15 second window has no samples for that chart. The antenna line at the top is a radio readout. A muted note there means that message is not in the current catalog.

{% hint style="info" %}
Wheel mph and the Speed tile can disagree. Wheel mph is rpm times the 21.56 inch diameter. Speed is the motor controller's vehicle velocity, converted to mph. Use both, and say which one you are quoting.
{% endhint %}
