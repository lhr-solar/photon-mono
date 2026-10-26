# Photon

Two programs. `photon-server` is the ingest server. `photon-next` is the UI and the daemon that feeds it. Set up and build the pieces a machine needs, then run one and name what the UI is reading.

```text
photon setup [server] [client] [station] [tools] [--ui desktop|web|both] [--pcan] [--build]
photon build [server] [client] [station] [tools] [--ui desktop|web|both] [--config Debug|Release]
photon test  [go] [native] [e2e] [--config Debug|Release] [--strict]
photon package DIR
photon clean [--deps]

photon server  [ingest PORT]
photon station [ingest PORT]
photon client  [FROM] [--ui desktop|web]
```

## Setup and build

| Word | Builds | Setup installs |
| --- | --- | --- |
| `server` | `photon-server`, `photon-sshd`, `photon-index`, `assets\` | CMake, Ninja, LLVM, VS C++ build tools, Go |
| `client` | `photon-backend`, `assets\`, and the `--ui` (desktop when omitted) | CMake, Ninja, LLVM, VS C++ build tools, Git |
| `--ui desktop` | `photon-ui.exe` | |
| `--ui web` | `ui\photon.html` and `ui\models\` | Python, emsdk 6.0.13 |
| `station` | `server` and `client --ui web` | both |
| `tools` | `photon-export`, `photon-observer` | CMake, Ninja, LLVM, VS C++ build tools, Go |

Plain `photon build` and `photon setup` mean `server client`. A server build never fetches SDL, ImGui, or WebView2. `--pcan` adds the PEAK driver for `photon client pcan`. `setup --build` builds the same words when setup finishes.

```text
photon setup station --build      PC: ingest server plus the page it serves
photon setup client --build       laptop: desktop UI
photon build client --ui web      laptop: browser UI
```

Everything lands in `build\`; the server only looks next to that executable. Debug native builds use `photon-next\.artifacts`, Release uses `photon-next\.artifacts-release`. The web page always builds Release in `photon-next\.artifacts-web`. The DBC source is `photon-next\assets\dbc`. Each build checks that its outputs are in `build\` and fails if one is missing. `photon clean` removes `build\` and those build trees; `--deps` also removes the fetched libraries and emsdk in `photon-next\.external`. Neither touches `data\`.

## Test

| Word | Runs |
| --- | --- |
| `go` | `go vet` and the photon-server package tests: config, client gate, store recovery, RTP/RTCP, video store, locate, UDP fan-out, VPT, local UI |
| `native` | `ctest` in the native tree (`photon-tests`, `photon-export-tests`) and the video decoder glue test under Node |
| `e2e` | `tests\e2e` against `build\`: backend log replay, a whole station fed the example clip (segments, byte-exact fan-out, auth, observer, live TSDB, export, the page in headless Chrome or Edge, clean shutdown), and `photon-index` import with `photon-export` |

Plain `photon test` runs all three. Build first; `native` uses the tree that `--config` names. The station test needs ports 8765 and 8080, so it skips beside a running station. `--strict` turns those skips, and any piece that was not built, into failures. `photon-server\cache` is unfinished and not run; `go test ./cache` in `photon-server` shows where it stands. `tests\e2e\test_canp.py` builds the car-side CANP source with gcc and runs it against `photon-server`, so it only runs on Linux.

`photon package DIR` copies `build\` into `DIR\server`, `station`, `client-desktop`, `tools`, and `symbols`, each with a `README.txt` that says how to run it.

## CI

`.github/workflows/ci.yml` runs on every push. Three jobs run side by side: the server tests on Windows, the server tests and the CANP interop test on Linux, and a Windows job that builds everything in Release, then runs `photon test native` and `photon test e2e --strict`. When all three pass, the run's page has `photon-station-<commit>`, `photon-server-<commit>`, `photon-client-desktop-<commit>`, `photon-tools-<commit>`, and `photon-symbols-<commit>` to download, kept for 90 days. A failed run uploads its test logs instead.

## Run

`server` is ingest only. The DAQ dials port `6100`; `ingest PORT` moves that port. `station` runs the server and the web UI together, with the UI on this machine's fan-out.

`client` is the UI. FROM says what it reads:

| FROM | Reads |
| --- | --- |
| nothing | the server on this machine, `127.0.0.1:6500` |
| `HOST[:PORT]` | that server's fan-out; the port defaults to `6500` |
| `oracle` | the public Oracle pipe, `150.136.85.207:6110` |
| `pcan` | a USB PCAN on this machine |
| a path | a VSD1 log or an `on_track` folder, replayed |

Video comes from the same host on TCP `:6700` and UDP `:6800`. `--ui` picks the desktop window or the page when both are built; desktop wins otherwise.

```text
photon client                          the server on this PC
photon client 10.0.0.5                 the PC from a laptop
photon client oracle --ui web          the public pipe, in the browser
photon client examples\brainerd-30s    replay the sample clip
```

`PHOTON_PASSWORD` defaults to `laptop`. `PHOTON_DATA_DIR` defaults to `data\` in this repo. Flags after the use case are passed through to `photon-server` or `photon-backend`. Add `-DryRun` to any command to print what it would run.

`Photon/` stays its own repo. That tree is the native desktop client and the embedded sharepoint.

InfluxDB 3 Core and Grafana are installed programs on the PC. They bind to `127.0.0.1`. Dashboard JSON for those panels belongs in `dashboards/`.

## Live checkouts

`F:\lhr\photon-server` and `F:\lhr\photon-next` are still the running trees. MacBook ingest has not moved.
