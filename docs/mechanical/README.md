---
title: Mechanical
---

# Mechanical

Mechanical is the chassis page. The tabs are Driver Input, Chassis IMUs, Suspension, Yaw, Wheels, and Dynamics.

Open it from the top bar after the playhead is on the moment you care about. [Logs](/logs.html) is how you put it there.

## The 15 second window

Every chart on these tabs draws the 15 seconds that end at the LOGS playhead. During LIVE that is the last 15 seconds from the car. During PLAY or PAUSE it is the 15 seconds before the clock in the LOGS drawer.

The charts do not pan on their own. Move the LOGS timeline to see another part of the run. For a longer window, or one unit on its own axis, use [Plots](/plots.html).

`No samples in selected time window` means this slice has no points. `--` on a readout means there is no latest sample. Scrub into motion, then look again.

## Race context

On a wide window a **Race context** column sits on the left: the 3D car, the camera, and a speed readout. On the Dynamics tab that column is six tiles instead of the single speed readout: Speed, Steer, Long G, Lat G, Yaw, and Throttle. Long G and Lat G follow the IMU selected on the Dynamics chart.

On a narrow window the same column is the **Race context** section at the top. Open it when you need the camera next to a trace. [Live View](/live-view.html) is the same camera and 3D car, larger.

## Move the charts

**Unlock layout**, **Undo**, and **Reset layout** sit above the charts.

1. Click **Unlock layout**.
2. Drag a chart by its header, or drag the lower-right corner.
3. Release. The board snaps to a grid and saves in this browser.
4. **Lock layout** when you are done, so a drag does not rearrange the page.

**Reset layout** restores the default arrangement. **Undo**, or Ctrl+Z, restores the previous one. The camera, the 3D view, and the value tiles stay fixed.

Each tab remembers its own board. Driver Input remembers Distributions and Power vs throttle separately.

## What each tab is for

| Tab | Use it to |
| --- | --- |
| [Driver Input](/mechanical/driver-input.html) | See throttle, both brake pressures, and steering move together |
| [Chassis IMUs](/mechanical/chassis-imus.html) | Compare body, FL, and FR acceleration |
| [Suspension](/mechanical/suspension.html) | Read `SUS_Data1` travel in millimeters |
| [Yaw](/mechanical/yaw.html) | Read body yaw rate |
| [Wheels](/mechanical/wheels.html) | Compare front-left and front-right rpm |
| [Dynamics](/mechanical/dynamics.html) | Read the G-G, wheel speed in mph, and the peak times |
