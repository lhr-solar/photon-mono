# photon-next

Photon Next is the Dear ImGui/ImPlot display client, built as a browser page or native desktop executable. It connects directly to the Go server at `/ws` (HTTP port 8080). Go owns CAN ingest, raw recordings, DBC decoding, Influx history, replay cursors, and video delivery. Video decoding remains in the client.

## Architecture

```text
DAQ TCP / local USB PCAN adapter
                |
                v
     photon-server (Go)
       raw CAN + timestamp index
       DBC registry + Influx history
       independent browser replay
                | WebSocket /ws
                v
     ImGui UI src/ui
       WASM: photon.html
       native: photon-ui
```

Local USB capture uses `photon-pcan-adapter` with a local Go server and local InfluxDB. The previous session daemon has been removed. See [Go server](../photon-server/readme.md) for uploads, imports, and configuration.

## Layout

Headers and implementations sit together in `src/<module>/`. Includes name the
module, for example `#include "store/tsdb.hpp"`. Keep subfolders for related
features such as UI screens, 3D rendering, and daemon sources.

| Path | Role |
| --- | --- |
| `src/base` | JSON, endian |
| `src/wire` | protocol v1, control plane, CANP, video framing |
| `src/decode` | Legacy migration/test decoder; excluded from client links |
| `src/store` | Offline migration/test readers; excluded from client links |
| `src/net` | sockets, WebSocket, client auth |
| `src/hw` | PCAN implementation linked only by the adapter/tests |
| `src/ui` | ImGui client (`photon-ui` / `photon.html`) |
| `web/` | shell + Emscripten build/serve scripts |
| `tools/` | `photon-export`, QA, log analysis |
| `tests/` | CTest (`photon-tests`) |
| `assets/` | DBC, fonts, models |

## Workspace and charts

How the mechanical team should use the page is a GitBook in [docs/gitbook](docs/gitbook/README.md). Git Sync should use the project directory `photon-next/docs/gitbook`.

The top-level screens are Pit (default), Dashboard, Plots, Mechanical, PDU,
Live View and CAN. Dynamics is a tab in Mechanical. Live View
contains the 3D and camera panes. Connection settings are available from the
connection status in the titlebar; drive days and runs live in Logs. Shared
consoles retain their source and DBC restrictions. Older `--screen` / URL screen
IDs redirect: `dynamics` to Mechanical/Dynamics, `scene3d` to Live View,
`analysis` to Plots, and `network` to the Connection popover.

Plots keeps saved view tabs visible and puts its controls in **Plot tools**.
Chart layouts in Plots, Mechanical/Dynamics, and PDU use the same
editor. Unlock the layout, drag a chart header, or resize its lower-right corner.
The panel follows the pointer continuously; its outline shows the snapped
24-column / 24-pixel grid. Overlapping charts move down. Escape cancels the
whole gesture, Undo restores the preceding layout, and Reset restores a useful
starting arrangement. Layouts save locally on release. Videos, 3D panes, and
value cards remain fixed. Charts are at least 288 pixels tall and use a
single-column scrolling arrangement when their saved widths would become too
small. This narrow presentation does not replace the desktop layout.

In Plots, dragging a chart body pans the shared time window without seeking or
moving the playhead. Clicking sets the playhead without recentering. Horizontal
wheel pans, vertical wheel zooms time, Alt-wheel zooms values, and Shift-drag
selects a range. Automatic Y axes retain their current range during a pan and
expand for new peaks; they refit when the gesture ends. Manual Y limits remain
in effect. History requests during a pan are coalesced to at most ten updates
per second per visible chart. Custom view exports use schema version 2; version
1 imports preserve their geometry by converting the old grid units.

This is a local Photon UI workflow. It does not require a Nominal API key.
The separate Photon Server Nominal streaming integration remains independent.

## Build

Windows host with clang, Ninja, CMake 3.25+:

```powershell
cmake -S . -B .artifacts -G Ninja -D CMAKE_C_COMPILER=clang -D CMAKE_CXX_COMPILER=clang++
cmake --build .artifacts
```

WASM page (emsdk once via `.\web\scripts\setup-emsdk.ps1`):

```powershell
.\web\scripts\build.ps1
```

Binaries land at `.artifacts/photon-ui.exe` and `.artifacts-web/photon.html`. Native server helpers are in `.artifacts/server-workers`. Do not mix native and WASM build directories.

## Run

From the repository root, run `photon server`, then `photon ui local`. The browser page is served by Go at **http://127.0.0.1:8080/photon.html**. Native clients connect to `ws://127.0.0.1:8080/ws`. Use `photon ui pcan` for local USB capture with Go and InfluxDB.

## Tests

```powershell
ctest --test-dir .artifacts --output-on-failure
```

Native visual QA can render a hidden window into a BMP without desktop input:

```powershell
$env:PHOTON_UI_STORE = "$PWD/.artifacts/qa-store"
.\.artifacts\photon-ui.exe --screen mechanical --capture .artifacts/mechanical.bmp --width 1440 --height 900 --frames 60
```

The interaction test also reports CPU frame time for a 24-panel board with
2,048 samples per chart and normal viewport clipping. This measurement excludes
GPU work, video decoding, and history I/O; it is not an end-to-end 60 FPS claim.
