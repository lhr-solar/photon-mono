<h1 align="center">Photon</h1>
<p align="center">How to move around the page.</p>
<p align="center">The top bar picks the screen. LOGS on the left picks the moment. Every screen follows that one playhead.</p>

<p align="center">
  <a href="https://lhr-solar.github.io/photon-mono/">Project page</a>
</p>

## Top bar

<p align="center">
  <img src="docs/assets/nav.png" alt="Top bar: Pit, Dashboard, Plots, Mechanical, PDU, Live View, CAN, then Live, Play, and Pause" width="960">
</p>

The names on the left are the screens. The underlined name is the one you are on. Click another name to move. Changing screens does not move the playhead.

| You want | Open |
| --- | --- |
| Pack, contactors, and whether the car is ready | Pit |
| Faults, coolant, motor limits, and the cell grid | Dashboard |
| One channel, or a longer window | Plots |
| Pedals, brakes, suspension, yaw, wheels, and the G-G | Mechanical |
| A switched 12 V channel | PDU |
| The camera and the 3D car, full size | Live View |
| Whether a message is on the bus, and its rate | CAN |

On the right are the car name, the day, and Live, Play, and Pause. The clock at the far right is the moment the whole page is showing. PLAY means a saved run is walking forward. `ready` means the page is connected.

The green row under the names is BPS, VCU, and MC. OK means that controller is talking.

## Logs

<p align="center">
  <img src="docs/assets/logs-drawer.png" alt="Logs drawer: playhead, hour minute and second bars, calendar, and the runs for that day" width="420">
</p>

The vertical LOGS tab is on the left edge. Open it before you trust an empty chart.

The clock above the bars is the playhead. The word beside it is the mode: LIVE is the car right now, PLAY is a saved run walking forward, and PAUSE is that run held still.

- Drag the hour bar, then the minute bar, then the second bar. Each drag pauses on the new time, so the camera and the charts land on the same moment.
- Live leaves the recording and returns to the car.
- Play walks the open run forward. If no run is selected yet, it starts at the beginning.
- Pause holds the playhead.
- The calendar lists days that have data. Pick the day, then click a run. The highlighted row is the run you are in.
- CSV on a run row exports that run. `vid` means the run has camera.

Charts on Mechanical are the last 15 seconds at this playhead. Plots can show a longer window. A click in a Plots chart moves the playhead and pauses. A drag inside a Plots chart pans, and does not seek.

You can change screens while a run is playing. The clock keeps moving. Pause first when you want a still frame and a chart of the same moment.

The same tour is published at [lhr-solar.github.io/photon-mono](https://lhr-solar.github.io/photon-mono/).
