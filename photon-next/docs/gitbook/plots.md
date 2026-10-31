---
description: Build a chart with the channels and the time window you choose.
icon: chart-line
---

# Plots

Plots is the free-form board. Use it when a Mechanical tab is the wrong shape: one unit per axis, more than 15 seconds, or a channel the tabs do not draw.

Open **Plots** in the top bar. The playhead is still the one in [LOGS](replay.md).

## Add a signal

1. Click **Add signal**. If no plot is selected, Photon adds a line plot and opens the picker.
2. You can also double-click a plot's header to open the same picker.
3. Set the plot title.
4. Choose **Line** or **G-G**.
5. Set **Window**. The default is 15 seconds. The slider runs from 2 seconds to 24 hours. Plots in the same view share that window.
6. Pick the signals in the list. Search by message or signal name. Names are in [Signal names](signals.md).

A line plot can hold more than one signal. Turn on **Dual Y** when the two signals use different units, such as steering degrees and brake psi. Turn on **Legend** when you need the names on the chart.

**G-G** wants an X signal and then a Y signal. The picker says "Assign X then Y." You can also pick **IMU B**, **IMU FR**, or **IMU FL** and Photon fills longitudinal and lateral acceleration for that IMU. A G-G that was filled from an IMU also has that dropdown on the plot header.

**Auto Y** fits the value axis to the points in view. Turn it off when you want a fixed scale.

**Freeze** holds this time window. Play and Live follow the cursor again when Freeze is off. The picker tooltip is the rule: click or drag a plot to scrub, and Play or Live follows the cursor unless Freeze is on.

**Export PNG** saves the plot you have open in the picker. If you need a picture of another day, change **Drive day** in that same picker first.

## Move around a chart

These gestures work on a line plot. Hold Ctrl. A wheel with no Ctrl scrolls the page.

| Gesture | Result |
| --- | --- |
| Drag left or right | Pans the time window. The playhead stays where it is. |
| Drag up or down | Pans the value axis and turns off Auto Y. |
| Click, without dragging | Sets the playhead. On a recording, the run pauses there. |
| Double-click the plot | Restores Auto Y and live follow. |
| Shift-drag | Marks a range. The plot shows count, min, max, and mean for that range. |
| Ctrl + wheel | Zooms time, around the time under the pointer. |
| Ctrl + Shift + wheel, or Ctrl + a sideways wheel | Pans time. |
| Ctrl + Alt + wheel | Zooms the value axis. |

The circular-arrow button beside **Add signal** is **Reset view**. It restores the 15 second following window and automatic value ranges.

View tabs along the top switch saved boards. The board saves in this browser. Another laptop does not have your layout until you build it there.

{% hint style="info" %}
Mechanical charts cannot do this. They stay on 15 seconds at the playhead. Build the comparison here, then use the playhead so Live View shows the same moment.
{% endhint %}

## Arrangements mechanical actually needs

- Brake psi alone, then throttle percent alone, then steering degrees alone. Three line plots, one unit each.
- `Pedal_Status.BrakePedal_Main_Pos` next to `Brake_Pressure_1.Brake_Pressure` and `Brake_Pressure_2.Brake_Pressure`, with Dual Y.
- `Suspension_FL.SUS_Data2` (and Data3, Data4) beside `SUS_Data1`.
- Coolant temperature and pump flow beside motor temperature, over a whole stint. Set the window to the length of the run.
- A G-G of a pair you choose, instead of the Dynamics IMU dropdown.

## What stays fixed

Plot tiles start locked, so dragging a header does not rearrange them. Mechanical, Dynamics, and PDU have **Unlock layout**. Plots does not show that button. Use the gestures above to move time and scale. Add another plot with **Add signal** when you need another chart.
