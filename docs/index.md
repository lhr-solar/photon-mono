---
layout: default
title: Photon
description: Telemetry for the LHR solar car.
---

Photon is the telemetry page for the car. It shows decoded CAN, the onboard camera, and a 3D view of the car on track. Open a live car or a saved run. Every screen shares one playhead.

![Pit during a saved run, with Logs open]({{ '/assets/pit.png' | relative_url }})

Pit during a saved run. Logs is open on the left: the timeline, the calendar, and the runs from that day.

![Pit with Logs closed, waiting for a car]({{ '/assets/pit-idle.png' | relative_url }})

Pit with Logs closed, before a car is connected.

## Screens

Pit is the home screen. Pack state, contactors, cell taps, the camera, and the 3D view sit on one page. The other screens are Dashboard, Plots, Mechanical, PDU, Live View, and CAN. Dynamics is a tab inside Mechanical.

Logs picks the run and the playhead. Charts on Mechanical draw the last 15 seconds at that playhead. Move the playhead before you decide a channel is dead.

Plots is where a chart needs a different shape. Export a CSV from the open run when the stint needs a spreadsheet.

## Run

```text
photon build
photon server
photon ui local
photon ui daq HOST[:PORT]
photon ui oracle
photon ui pcan
photon ui log RECORDING_ID
photon station
```

`photon server` accepts CAN on port 6100 and serves the page and `/ws` on port 8080. `photon build` writes the server, the page, the PCAN adapter, and the offline exporter into `build\`.

`photon-server` owns ingest, recordings, DBC decoding, browser sessions, replay, and video. `photon-next` is the display client, as `photon.html` or `photon-ui`. Configuration is in the [server readme](https://github.com/lhr-solar/photon-mono/blob/main/photon-server/readme.md). The display client is described in the [photon-next readme](https://github.com/lhr-solar/photon-mono/blob/main/photon-next/README.md).

`Photon/` is a separate repository. That tree is the native desktop client.
