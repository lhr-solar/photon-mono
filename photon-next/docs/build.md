# Build and run reference

## Native (Windows, clang + Ninja + CMake 3.25+)

Builds go through the presets in `CMakePresets.json`. `headless-*` leaves out `photon-ui` and never fetches SDL2, ImGui, ImPlot, or WebView2; `desktop-*` builds everything.

```powershell
cmake --preset desktop-debug
cmake --build --preset desktop-debug
ctest --preset desktop-debug
```

| Binary | Path in `..\out\build\<preset>` |
| --- | --- |
| `photon-backend` | `photon/daemon/photon-backend.exe` |
| `photon-index` | `photon/daemon/photon-index.exe` |
| `photon-ui` | `photon-ui.exe` (desktop presets) |
| `photon-export` | `tools/photon-export/photon-export.exe` |
| `photon-tests` | `tests/photon-tests.exe` |

`cmake --build --preset desktop-debug --target format` / `format-check` run clang-format (skipped if not installed).

## Install components

Each program installs with the assets it reads at runtime, so an installed copy runs anywhere. The repo's `photon build` installs these into the bundles under `out\dist`.

| Component | Installs |
| --- | --- |
| `server` | `photon-index`, `assets/dbc` |
| `client` | `photon-backend`, `assets/dbc`, `assets/tracks` |
| `desktop` | `photon-ui`, `assets/fonts`, `assets/models`, `assets/maps3d.html`, `assets/maps3d.js` |
| `web` | `ui/photon.{html,js,wasm,data}`, `ui/models` (from the `web-release` tree) |
| `tools` | `photon-export` |
| `symbols` | the `.pdb` beside each of the above, where the link made one |

```powershell
cmake --install ..\out\build\desktop-debug --component client --prefix C:\photon-client
```

## WASM page

Pinned third-party (FetchContent into `.external/`): emsdk **6.0.13**, Dear ImGui `v1.92.9-docking`, ImPlot `v0.17`, Emscripten `--use-port=sdl2` + WebGL2/GLES3.

Install emsdk once: `.\web\scripts\setup-emsdk.ps1` installs and activates 6.0.13 (or clone `emscripten-core/emsdk` into `.external/emsdk` and `emsdk install 6.0.13; emsdk activate 6.0.13`).

```powershell
.\web\scripts\build.ps1
```

Manual equivalent (the preset finds the Emscripten toolchain through `EMSDK`, which `emsdk_env.ps1` sets):

```powershell
. .\.external\emsdk\emsdk_env.ps1
cmake --preset web-release
cmake --build --preset web-release
```

`build.ps1` also stamps one cache token into the html and js and checks the JS glue against the wasm, so prefer it. Outputs: `..\out\build\web-release\photon.{html,js,wasm,data}` and `models\`. In the monorepo, `photon build client --ui web` runs `build.ps1` and installs the `web` component into the client bundle's `ui\`.

Serve: `.\web\scripts\serve.ps1`, open `http://127.0.0.1:8080/photon.html` (never `file://`). Headless protocol check: `python .\web\scripts\smoke_ws.py`.

## photon-backend flags

| Flag | Default | Meaning |
| --- | --- | --- |
| `--ws-port` | `8765` | WebSocket listen port (always bound to `127.0.0.1`) |
| `--daq-host` | `127.0.0.1` | Default CANP fan-out host for `select_source daq_tcp` |
| `--daq-port` | `6500` | Default CANP fan-out port |
| `--source` | `daq_tcp` | `daq_tcp` (photon-server), `log_replay`, or `pcan` |
| `--no-live-tail` | off | Do not write a TSDB (PC `photon-index` owns the store) |
| `--password` | `PHOTON_PASSWORD` | Shared secret for photon-server fan-out / video / observer |

## photon-ui (native desktop client)

Same ImGui sources as the WASM page. Flags: `--ws URL`, `--password P`, `--shared`/`--private`, `--screen ID`, `--quit-after-ms N` (smoke-test hook: prints `timed quit` + `shutdown ok`).

## photon-index

Historical: `--logs DIR --out DIR` indexes VSD1 into a TSDB session. Live PC ingest: `--live-fanout HOST:PORT --out DIR` writes `data/tsdb` from photon-server CANP fan-out. Also `--bench` / `--retention`; see `--help`.

## photon-export

Reads a TSDB session, writes Parquet: `--store DIR --session ID --out FILE --signals LIST` (or `--all-signals`). Library `photon-export-lib` is also linked by the daemon CSV path and tests.

## Screens

Pit (default) · Dashboard · Plots · Dynamics · Live View (WebCodecs/MF H.264) · 3D (glTF car) · Track Sim · CAN · Network · Analysis. Navigation is a top tab bar; titlebar right holds connection dot, source, timeline, Live/Pause.
