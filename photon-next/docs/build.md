# Build and run reference

## Native (Windows, clang + Ninja + CMake 3.25+)

```powershell
cmake -S . -B .artifacts -G Ninja -D CMAKE_C_COMPILER=clang -D CMAKE_CXX_COMPILER=clang++
cmake --build .artifacts
ctest --test-dir .artifacts --output-on-failure
```

| Binary | Path |
| --- | --- |
| `photon-backend` | `.artifacts/photon/daemon/photon-backend.exe` |
| `photon-index` | `.artifacts/photon/daemon/photon-index.exe` |
| `photon-ui` | `.artifacts/photon-ui.exe` |
| `photon-export` | `.artifacts/tools/photon-export/photon-export.exe` |
| `photon-tests` | `.artifacts/tests/photon-tests.exe` |

`cmake --build .artifacts --target format` / `format-check` run clang-format (skipped if not installed).

## WASM page

Pinned third-party (FetchContent into `.external/`): emsdk **6.0.13**, Dear ImGui `v1.92.9-docking`, ImPlot `v0.17`, Emscripten `--use-port=sdl2` + WebGL2/GLES3.

Install emsdk once: `.\web\scripts\setup-emsdk.ps1` installs and activates 6.0.13 (or clone `emscripten-core/emsdk` into `.external/emsdk` and `emsdk install 6.0.13; emsdk activate 6.0.13`).

```powershell
.\web\scripts\build.ps1
```

Manual equivalent:

```powershell
. .\.external\emsdk\emsdk_env.ps1
emcmake cmake -S . -B .artifacts-web -G Ninja -DPHOTON_FRONTEND=ON -DBUILD_TESTING=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build .artifacts-web
```

Outputs: `.artifacts-web/photon.{html,js,wasm,data}` and `.artifacts-web/models/`. Do not mix `.artifacts-web` with the native `.artifacts` configure (`PHOTON_FRONTEND` defaults OFF). In the monorepo, `photon build client --ui web` runs `build.ps1` and copies the page and `models/` to `build/ui/`.

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
