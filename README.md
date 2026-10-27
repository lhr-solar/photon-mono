# Photon

Two programs. `photon-server` is the ingest server. `photon-next` is the UI and the daemon that feeds it. Set up and build the pieces a machine needs, then run one and name what the UI is reading.

```text
photon setup [server] [client] [station] [tools] [--ui desktop|web|both] [--pcan] [--build]
photon build [server] [client] [station] [tools] [--ui desktop|web|both] [--config Debug|Release]
photon test  [go] [native] [e2e] [--config Debug|Release] [--strict]
photon clean [--deps]

photon server  [ingest PORT]
photon station [ingest PORT]
photon client  [FROM] [--ui desktop|web]
```

`photon` works the same on Windows, macOS, and Linux. On Windows it is `photon.cmd` (type `photon` or `.\photon` in the repo root); on macOS and Linux it is `./photon`. Both run the Python CLI in `scripts/photon`, which needs Python 3.9 or newer: macOS has it with the Xcode Command Line Tools, Linux distributions ship it, and on Windows `photon setup` installs it with winget if it is missing.

## Setup and build

| Word | Builds | Setup installs |
| --- | --- | --- |
| `server` | `photon-server`, `photon-sshd`, `photon-index` | CMake, Ninja, clang, Go |
| `client` | `photon-backend` and the `--ui` (desktop when omitted) | CMake, Ninja, clang, Git |
| `--ui desktop` | `photon-ui` | SDL2 (Linux only; elsewhere the build fetches it) |
| `--ui web` | the page, `ui\photon.html` and `ui\models\` | emsdk 6.0.13 |
| `station` | `server` and `client --ui web` | both |
| `tools` | `photon-export`, `photon-observer` | CMake, Ninja, clang, Go |

Where setup gets them:

| OS | From |
| --- | --- |
| Windows | winget: LLVM for clang, plus the Visual Studio C++ build tools and Windows SDK it links against |
| macOS | the Xcode Command Line Tools (Apple's clang and git), and Homebrew for the rest |
| Linux | apt, dnf, pacman, or zypper, whichever is there, through `sudo`; clang 17+ and Go 1.21+ (which fetches the Go version `go.mod` asks for) |

Plain `photon build` and `photon setup` mean `server client`. `--pcan` adds the PEAK driver for `photon client pcan`, on Windows only (PCAN is Windows-only for now). `setup --build` builds the same words when setup finishes. The desktop UI on macOS and Linux has no OSM 3D map overlay or video decode yet; both are Windows-only.

```text
photon setup station --build      PC: ingest server plus the page it serves
photon setup client --build       laptop: desktop UI
photon build client --ui web      laptop: browser UI
```

### Where a build goes

A build compiles in `out\build` and then assembles runnable bundles in `out\dist`, one folder each. A bundle is complete on its own: copy the folder to another machine and run it from there. Each has a `README.txt` with its run line, commit, and build type.

| Bundle | Holds | Run with |
| --- | --- | --- |
| `out\dist\server` | `photon-server`, `photon-sshd`, `photon-index`, `assets\dbc` | `photon server` |
| `out\dist\station` | the server bundle, plus `photon-backend`, `assets\tracks`, and the page in `ui\` | `photon station` |
| `out\dist\client` | `photon-backend`, `assets\dbc`, `assets\tracks`, and the UIs that were built: `photon-ui` with `assets\fonts`, `models`, `maps3d.*`, and/or the page in `ui\` | `photon client` |
| `out\dist\tools` | `photon-export`, `photon-observer` | by hand |
| `out\dist\symbols` | Windows only: `.pdb` files from the most recent build, for the `.exe` files that have them | |

A bundle is installed into `out\dist\.staging\<bundle>`, checked for every file it needs, and only then swapped in for the old one, so a failed build never leaves a bundle that looks complete. A build stops before compiling if a program is running from a bundle it would replace.

The C++ half comes from CMake presets in `photon-next\CMakePresets.json`:

| Preset | Tree | Used for |
| --- | --- | --- |
| `headless-debug`, `headless-release` | `out\build\headless-<config>` | `server`, `tools`, and a client without the desktop UI; never fetches SDL, ImGui, or WebView2 |
| `desktop-debug`, `desktop-release` | `out\build\desktop-<config>` | any build with `--ui desktop` (all native targets) |
| `web-release` | `out\build\web-release` | the page; `photon build` loads emsdk, runs it, and stamps the page (`scripts/photon/web.py`) |

Go programs build into `out\build\go`. Bundles come from CMake install components (`server`, `client`, `desktop`, `web`, `tools`, `symbols`) with `cmake --install <tree> --component <name>`, plus the Go programs. The same presets work from a terminal in `photon-next` (`cmake --preset desktop-debug`, `cmake --build --preset desktop-debug`, `ctest --preset desktop-debug`) and from editors that read CMake presets, such as VS Code's CMake Tools.

`photon clean` removes `out\`, and the `build\` and `photon-next\.artifacts*` trees that older builds left; `--deps` also removes the fetched libraries and emsdk in `photon-next\.external`. Neither touches `data\`.

## Test

The tests live under `tests\`: `native` (the C++ tests CTest runs), `web` (the video decoder glue, under Node), `canp` (the car-side CANP sink and source), and `e2e`. The Go tests are the exception: Go only builds a package's `_test.go` files from that package's own folder, so they stay beside the code in `photon-server`.

| Word | Runs |
| --- | --- |
| `go` | `go vet` and the photon-server package tests: config, client gate, store recovery, RTP/RTCP, video store, locate, UDP fan-out, VPT, local UI |
| `native` | `cmake --build` and `ctest --preset` in each native tree built for `--config` (`photon-tests`, `photon-export-tests`), and the video decoder glue test under Node |
| `e2e` | `tests\e2e` against `out\dist`: backend log replay from `client`, a whole `station` fed the example clip (segments, byte-exact fan-out, auth, observer, live TSDB, export, the page in headless Chrome or Edge, clean shutdown), and `photon-index` import with `photon-export` |

Plain `photon test` runs all three. Build first. The station test needs ports 8765 and 8080, so it skips beside a running station. `--strict` turns those skips, and any bundle that was not built, into failures. `photon-server\cache` is unfinished and not run; `go test ./cache` in `photon-server` shows where it stands. `tests\e2e\test_canp.py` builds the car-side CANP source with gcc and runs it against `photon-server`, so it only runs on Linux.

## CI

`.github/workflows/ci.yml` runs on every push. The jobs run side by side: the server tests on Windows; the server tests and the CANP interop test on Linux; a Windows x64 job that runs `photon build station client tools --ui both --config Release`, then `photon test native` and `photon test e2e --strict` against those bundles; and one job each for Linux x64 and arm64, macOS arm64 and x64, and Windows arm64, which run `photon setup` (Linux and macOS), `photon build server client tools --config Release`, and `photon test native`. When the two server-test jobs and the Windows x64 job pass, the run's page has `photon-station-<commit>`, `photon-server-<commit>`, `photon-client-<commit>`, `photon-tools-<commit>`, and `photon-symbols-<commit>` to download: the `out\dist` folders, kept for 90 days. A failed run uploads its test logs instead.

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

`PHOTON_PASSWORD` defaults to `laptop`. `PHOTON_DATA_DIR` defaults to `data\` in this repo. Flags after the use case are passed through to `photon-server` or `photon-backend`. Add `--dry-run` to any command to print what it would run.

`Photon/` stays its own repo. That tree is the native desktop client and the embedded sharepoint.

InfluxDB 3 Core and Grafana are installed programs on the PC. They bind to `127.0.0.1`. Dashboard JSON for those panels belongs in `dashboards/`.

## Live checkouts

`F:\lhr\photon-server` and `F:\lhr\photon-next` are still the running trees. MacBook ingest has not moved.
