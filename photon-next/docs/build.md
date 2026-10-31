# Build and run reference

The default runtime is Go on port 8080 and a display client connected directly to `/ws`. See [Go recording and replay](../../photon-server/readme.md) for current launch commands, uploads, and imports. The C++ session backend has been removed. Native helpers perform USB capture and offline imports.

## Native (Windows, clang + Ninja + CMake 3.25+)

```powershell
cmake -S . -B .artifacts -G Ninja -D CMAKE_C_COMPILER=clang -D CMAKE_CXX_COMPILER=clang++
cmake --build .artifacts
ctest --test-dir .artifacts --output-on-failure
```

| Binary | Path |
| --- | --- |
| `photon-ui` | `.artifacts/photon-ui.exe` |
| `photon-pcan-adapter` | `.artifacts/server-workers/photon-pcan-adapter.exe` |
| `photon-tsdb-export` | `.artifacts/server-workers/photon-tsdb-export.exe` |
| `photon-export` | `.artifacts/tools/photon-export/photon-export.exe` |
| `photon-tests` | `.artifacts/tests/photon-tests.exe` |

`cmake --build .artifacts --target format` / `format-check` run clang-format (skipped if not installed).

## WASM page

Pinned third-party (FetchContent into `.external/`): emsdk **6.0.7**, Dear ImGui `v1.92.9-docking`, ImPlot `v0.17`, Emscripten `--use-port=sdl2` + WebGL2/GLES3.

Install emsdk once: `.\web\scripts\setup-emsdk.ps1` (or clone `emscripten-core/emsdk` into `.external/emsdk` and `emsdk install latest; emsdk activate latest`).

```powershell
.\web\scripts\build.ps1
```

Manual equivalent:

```powershell
. .\.external\emsdk\emsdk_env.ps1
emcmake cmake -S . -B .artifacts-web -G Ninja -DPHOTON_FRONTEND=ON -DBUILD_TESTING=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build .artifacts-web
```

Outputs: `.artifacts-web/photon.{html,js,wasm,data}`. Do not mix `.artifacts-web` with the native `.artifacts` configure (`PHOTON_FRONTEND` defaults OFF). `build.ps1` copies the page to `photon-server/ui/` when that sibling exists.

Serve: `.\web\scripts\serve.ps1`, open `http://127.0.0.1:8080/photon.html` (never `file://`). Headless protocol check: `python .\web\scripts\smoke_ws.py`.

## photon-ui (native desktop client)

Same ImGui sources as the WASM page. Flags: `--ws URL`, `--password P`, `--shared`/`--private`, `--screen ID`, `--quit-after-ms N` (smoke-test hook: prints `timed quit` + `shutdown ok`).

## photon-export

Reads a TSDB session, writes Parquet: `--store DIR --session ID --out FILE --signals LIST` (or `--all-signals`). Library `photon-export-lib` is used by offline tools and tests.

## Screens

Pit (default), Dashboard, Plots, Mechanical, PDU, Live View and CAN. Dynamics is inside Mechanical; Connection is in the titlebar.
