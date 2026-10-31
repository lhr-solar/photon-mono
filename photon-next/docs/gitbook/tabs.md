---
description: Which top-bar tab to open for a mechanical question.
icon: table-columns
---

# Tabs

The top bar is the same on a live car and on a replay. The playhead does not reset when you change tabs.

| You want | Open |
| --- | --- |
| Pedals, steering, brake pressure, suspension, yaw, wheel speed, G-G | [Mechanical](mechanical/README.md) |
| Pack, motor temperature, contactors, whether the car is ready | [Pit](pit.md) |
| Brake pedal faults, coolant, pump flow, motor limits, the cell grid | [Dashboard](dashboard.md) |
| Any CAN channel, a longer window, one unit per axis | [Plots](plots.md) |
| Camera and the 3D car, full size | [Live View](live-view.md) |
| A switched 12 V channel on the PDU | [PDU](pdu-and-can.md) |
| Whether a message is on the bus, and its raw rate | [CAN](pdu-and-can.md) |
| A spreadsheet of the run | [Export a CSV](export.md), from the LOGS run row |

## How the tabs share time

- **LOGS** owns the playhead. Live, Play, Pause, and the timeline bars are in that drawer. See [Live and replay](replay.md).
- **Mechanical** draws a fixed 15 second window that ends at the playhead.
- **Plots** can show a much longer window. A click in a plot seeks. A drag pans.
- **Pit**, **Dashboard**, and **Live View** follow the same playhead, including the camera.

Change tabs while a run is playing and the clock keeps moving. Pause first if you are comparing a still frame with a chart.
