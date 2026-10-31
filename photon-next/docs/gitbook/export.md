---
description: Download a run as CSV at 1 Hz, 10 Hz, or raw.
icon: file-csv
---

# Export a CSV

CSV comes from the run, not from the chart you happen to be looking at.

1. Open [LOGS](replay.md).
2. Pick the day and the run.
3. Click **CSV** on that run.
4. Choose a rate and a signal set.
5. Download.

The popup shows the run id and how long the run is.

## Rate

| Rate | What you get |
| --- | --- |
| 1 Hz | About one sample a second. Enough for a lap overview. |
| 10 Hz | About one sample every 100 ms. The usual export for a handling plot. |
| Raw | Every stored sample. A full run is large. The tooltip warns that a full run is about a gigabyte. |

Start with 10 Hz. Use Raw only for the channel you are debugging, over the run you need.

## Signals

| Choice | What you get |
| --- | --- |
| Mapped | Channels already used by Pit, Dynamics, and the 3D view. |
| Selected | You pick names. A search box is above the list. **Mapped** fills the list with the Mapped set. **Clear** empties it. |
| All | Every signal stored in the run. |

Mapped includes throttle, steering, both brake pressures, the three IMUs, body yaw, both front wheel speeds, the three suspension `SUS_Data1` channels, and vehicle speed. It also includes the Pit tiles: pack, motor rpm, motor temperature, and the MPPT currents Photon uses for array power.

{% hint style="warning" %}
Motor power command is a Dashboard channel. It is not in the Mapped set. For the power-versus-throttle scatter, choose **Selected** and add `MC_PowerCommand.MC_MotorPowerSetpoint`.
{% endhint %}

`SUS_Data2`, `SUS_Data3`, and `SUS_Data4` are also outside the Mapped set. Add them under **Selected** when the test used those channels.

Search strings that find the mechanical channels are listed on [Signal names](signals.md).
