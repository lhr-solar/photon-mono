# photon-next cleanup plan: systems layout modeled on Photon `main`

**Date:** 2026-09-19
**Status:** in progress — systems layout landed; god-file / service splits still open
**Scope:** `F:\lhr\photon-next` only. `F:\lhr\Photon` (legacy) and `F:\lhr\photon-server` (Go ingest) are read-only references.

## Progress (2026-09-19)

### DONE

| Item | Commit |
| --- | --- |
| Baseline (month of WIP before restructure) | `aac24d6` (earlier snapshot `fbd7c0a`) |
| Restructure into `photon/<system>` libraries (move-only + CMake) | `9f62824` |
| WASM frontend build + script paths after restructure | `f3911cb` |
| Scripts triage (`backend/scripts` → `tools/qa`, `tools/data`, `tools/charge_model`) | `9f62824` |
| Scratch deletion (root `_*.ps1`, `opencode_*`, agent one-offs) | `9f62824` |
| `.clang-format` + `format` / `format-check` targets | this commit (ColumnLimit **110** after measuring the tree) |
| README rewrite for systems layout + per-system READMEs | this commit |

§2 below is the **pre-`9f62824` audit** (`core/` / `backend/` / `frontend/` paths). The tree is now `photon/{base,wire,decode,store,net,hw,synth,daemon,ui}`.

### OPEN

- SessionHub service split (`session.cpp` → `Daemon` + per-concern services)
- Client / App UI split (`client.cpp`, `app.cpp`)
- `custom_view.cpp` / `tsdb.cpp` god-file splits
- `web/shell.html` JS extraction
- Per-system test executables
- `Platform` interface (web vs native)

The rest of this file is the forward plan.

## 1. Why

photon-next started as a three-tier prototype (`core/`, `backend/`, `frontend/`) and grew ~60 k LOC of feature work on top of it in one month without ever re-drawing the boundaries. Photon `main` is organized as **systems**: one directory, one CMake library, one facade struct per system, and a strict dependency DAG:

```
photon/
  engine/    Photon { GPU gpu; GUI gui; Network network; Parse parse; }  init / appLogic / renderLoop / destroy
  gpu/       GPU      render & compute
  gui/       GUI      control interface
  network/   Network  ingest streams
  parse/     Parse    decode & store  (Arena, DBC)
  synth/     Synth    analysis & synthesis
```

`engine → gui → {gpu, network, parse}`, `network → parse`, nothing points backwards. Every system has its own `CMakeLists.txt` (`file(GLOB) + add_library + target_link_libraries PUBLIC <deps>`), assets are embedded via `find/embed_assets.cmake` per asset type, and one `.clang-format` with `format` / `format-check` targets.

This plan reshapes photon-next into the same shape while keeping the split that photon-next introduced on purpose (a native daemon that owns hardware/network/storage, and a UI that only talks WebSocket).

## 2. Current state (audit)

### 2.1 Layering violations

| Symptom | Where |
| --- | --- |
| UI includes daemon headers | `frontend/…/client.hpp` → `photon/websocket.hpp`; `client.cpp`, `main.cpp` → `photon/net.hpp`; `video_decoder.cpp` → `photon/video_framing.hpp` |
| UI CMake compiles daemon sources | `frontend/CMakeLists.txt` builds `photon-ws` from `${CMAKE_SOURCE_DIR}/backend/src/{net,websocket}.cpp` |
| Tests compile sources instead of linking libraries | `tests/CMakeLists.txt` lists 15 `backend/src/*.cpp` + `frontend/src/*.cpp` files directly |
| Hardware driver in "core" | `core/…/pcan.hpp` (dynamic `PCANBasic.dll`) |
| Pure analysis logic lives in the UI tree | `frontend/src/analysis_{runtime,workbook,eval}.cpp`, `tile_cache.cpp`, `tile_store.cpp` (all compiled into `photon-tests` with no ImGui) |
| Web code in `assets/` | `assets/maps3d.html`, `assets/maps3d.js` |
| Network I/O on the render thread | `frontend/src/client.cpp` `pumpNativeWs` runs inside `Client::tick` from `App::frame`; frame pump capped at 512 frames; `App::frame` calls `client_.tick` twice per frame to survive GLB-parse stalls; 2026-09-06 note records the socket being closed when history/CAN outran the pump |
| Daemon does socket poll + all service work on one thread | `SessionHub::tick`: `net::poll(…, 5)` then accept, per-client service, history flush, sources, replays, streaming — one thread |

### 2.2 God files

| File | Size | Concerns mixed |
| --- | --- | --- |
| `core/src/tsdb.cpp` | 102 KB | store format, query, compaction, live tail (`tsdb_live.cpp` already split) |
| `frontend/src/custom_view.cpp` | 101 KB | plot views, time axis, range select, scrub, PNG export, JSON import/export, EM_JS glue |
| `backend/src/session.cpp` | 92 KB | `SessionHub`: WS accept/poll, JSON dispatch, telemetry/CAN/video/timeline streaming, history queries + thread pool + credit flow, track-sim jobs, analysis jobs, live writer, retention, crash recovery, drive-day catalog, CSV export |
| `tools/photon-index/main.cpp` | 62 KB | CLI, indexer, live fan-out server |
| `frontend/src/screens/dashboard.cpp` | 60 KB | one screen |
| `frontend/src/tile_cache.cpp` | 56 KB | LOD tile cache + history I/O + placeholder logic |
| `backend/src/source.cpp` | 55 KB | `DecodeBuffer`, 4 sources, `SourceHub`, `ReplayView` |
| `tools/photon-export/test_export.cpp` | 55 KB | tests inside a tool dir |
| `frontend/src/gl3d/{model,scene}.cpp` | 52 + 47 KB | glTF load, batching, scene, IK, OSM bridge, EM_JS |
| `core/src/control.cpp` | 47 KB | JSON control-plane encode/decode for ~40 message types |
| `frontend/src/video_decoder.cpp` | 45 KB | WebCodecs bridge + native MF path + release pacing |
| `frontend/src/client.cpp` | 43 KB | WS connection, reconnect, hello/ack, source adoption, interest tracking, drive-day heuristics, history, video subscribe |
| `frontend/shell.html` | 70 KB | ~1 500 lines of JS: MapLibre + three.js car layer, sat tiles, tile cache, fetch remap, crash reporter, Emscripten `Module` |

### 2.3 Hygiene

- One commit (`fbd7c0a`); **90 untracked files, ~70 modified**. Nothing has been reviewed as a diff.
- `.gitignore` covers `.artifacts/` and `.artifacts-web/` but not `.artifacts-replay-fix/`, `.artifacts-tp/` (both untracked build trees at repo root).
- Root scratch: `_build_wasm_macbook.ps1`, `_hotfix_blank.ps1` (patches `photon-server/ui/photon.html` in place), `_probe_locate.py`, `_probe_ui.ps1`, `_verify_wasm_ui.ps1`.
- `backend/scripts/` has ~25 Python files of four unrelated kinds: protocol smoke checks (`check_*_ws.py`, `smoke_ws.py`), browser QA automation (`click_*.py`, `screenshot_*.py`, `shot_after_click.py`, `capture_console.py`), data science (`charge_model/`, `extract_on_track.py`, `export_comp_timeseries.py`, `log_coverage.py`), and agent-loop plumbing (`opencode_*.py`). `__pycache__/` present.
- `tools/photon-export/_mut.py`, `frontend/scripts/_dismiss-gpu.ps1`, `tests/revert_check_video_index.ps1`, `tools/photon-export/revert_check.py` are one-off agent artifacts.
- No `.clang-format`, no `format` target, no CI script. Style is consistent (4-space, `photon::ui`, trailing-underscore members) but unenforced.
- Target names mix conventions: `photon-core`, `photon-ws`, `photon-imgui`, `photon-tinygltf`, `photon-frontend`, `photon-ui`, `photon-backend`, `photon-index`, `photon-export-lib`.
- `README.md` is milestone-2 era: no mention of Dashboard, Analysis, TSDB/history, `photon-ui` native exe, `photon-index`, `photon-export`, native video/maps, client auth, history remote. Protocol table is missing ~25 control messages that `control.hpp` defines.
- `App::init` finds the analysis screen via `dynamic_cast<AnalysisScreen*>` loop; `drawShell` special-cases screen ids by `strcmp`.
- `SyntheticSource` ships in the production `photon-backend` binary although `.memory/context/2026-08-31-remove-synthetic-source.md` keeps it "for unit tests only"; it still leaks into `frontend/src/{client,app}.cpp` and `screens/network.cpp` via `SourceKind::Synthetic` checks.

## 3. Target layout

```
photon-next/
├── CMakeLists.txt                 project, options, add_subdirectory(assets) + add_subdirectory(photon)
├── .clang-format                  Google-based, ColumnLimit 100, IndentWidth 4 (matches current code)
├── cmake/
│   ├── warnings.cmake             photon_apply_warnings()
│   ├── deps.cmake                 imgui / implot / tinygltf / SDL2 / WebView2 FetchContent, pinned
│   ├── emscripten.cmake           link flags, preload, shell, exported symbols
│   └── embed_assets.cmake         (port of Photon/find/embed_assets.cmake for dbc + tracks)
├── assets/
│   ├── dbc/ CMakeLists.txt        DbcHeaders / DbcAssets (embedded, like Photon)
│   ├── fonts/                     preload (WASM) / copy (native)
│   ├── models/                    runtime fetch, never embedded
│   └── tracks/ CMakeLists.txt     TrackAssets
├── photon/
│   ├── sync/      photon_sync     header-only: spmc.hpp (ported from Photon/parse/spmc.hpp), spsc_ring.hpp, published.hpp (PublishedSize), thread.hpp (jthread + stop_token helpers)
│   ├── wire/      photon_wire     protocol.hpp, control.{hpp,cpp} → control/{codec,messages}.cpp, json, endian, canp, video_framing
│   ├── decode/    photon_decode   dbc, interp, decode_buffer (moved from backend/source.cpp)
│   ├── store/     photon_store    tsdb/{store,query,compact,live}.cpp, vsd1, video_index, log_sessions
│   ├── net/       photon_net      socket (net.cpp), ws/{handshake,server,client}.cpp, sha1, base64, client_auth
│   ├── hw/        photon_hw       pcan (dynamic load), future: socketcan, serial
│   ├── synth/     photon_synth    tracksim, analysis/{compute,eval,runtime,workbook}, history/tile_cache, tile_store (pure logic, no ImGui)
│   ├── daemon/    photon_daemon   + exe photon-backend
│   │   ├── daemon.{hpp,cpp}       struct Daemon { Network network; SourceHub sources; Services…; init/tick/stop }
│   │   ├── cli.cpp, main.cpp
│   │   ├── network/               network.{hpp,cpp}: I/O jthread (poll loop over listener, WS clients, DAQ TCP, video relay, history remote) + per-connection inbound/outbound rings
│   │   ├── server/                client_session, control_router (main thread)
│   │   ├── sources/               source.hpp (ISource), daq_tcp, log_replay, pcan, source_hub, replay_view
│   │   └── services/              telemetry_stream, can_stream, timeline, video_service (+relay, udp), history_service (+pool, remote, credit), live_recorder (+retention, recover), catalog_service (drive days, sessions), tracksim_service, analysis_service, csv_export
│   └── ui/        photon_ui       + exe photon-ui (native) + exe photon-frontend (WASM, OUTPUT photon.html)
│       ├── ui.{hpp,cpp}           struct UI { Platform& platform; Arena arena; Client client; Screens screens; init/frame/shutdown }
│       ├── app/                   shell, titlebar, log_drawer, alarm_strip, screen registry
│       ├── transport/             client → connection, reconnect, hello, interest, control_handlers, history_io
│       ├── state/                 arena, timeline_state, video_state, tracksim_state, catalog, signal_map, alarms
│       ├── screens/               pit, dashboard, plots (custom_view split: views, axis, range, scrub, export), dynamics, live, scene3d, tracksim, can, network, analysis
│       ├── widgets/               widgets, gg_plot, plot_axis/color/time, clock_format, theme
│       ├── render/                gl_api, gl_loader, gl3d/{math,model,renderer,scene}
│       ├── video/                 decoder (WebCodecs + MF), hud, release pacing
│       ├── maps/                  maps3d bridge (web + WebView2)
│       └── platform/              platform.hpp interface; web/ (EM_JS glue, page_url, idb_tile_store); native/ (win32 host, crash handler, native_video, native_maps)
├── web/
│   ├── shell.html                 markup only
│   ├── js/                        module.js, crash.js, fetch_remap.js, tilecache.js, maps3d.js (from assets/), video.js
│   └── scripts/                   build.ps1, serve.ps1, serve_nocache.py, setup-emsdk.ps1, deploy-to-photon-server.ps1
├── tools/
│   ├── photon-index/              split: cli.cpp, indexer.cpp, live_fanout.cpp
│   ├── photon-export/             lib + exe; tests move to tests/
│   └── qa/                        protocol smoke + browser automation Python that is still used
├── tests/
│   ├── harness.hpp
│   ├── wire/ decode/ store/ net/ synth/ daemon/ ui/   one test exe per system, ctest LABELS
│   └── e2e/                       smoke_ws.py, check_*_ws.py, webcodecs .cjs
└── docs/
    ├── architecture.md            systems + DAG (this doc's §3/§4 once landed)
    ├── protocol.md                v1 binary + full control-plane table (moved out of README)
    ├── build.md
    └── cleanup-plan.md            this file
```

### 3.1 Dependency DAG (enforced by `target_link_libraries`, no `${CMAKE_SOURCE_DIR}/…/include` reach-arounds)

```
                 ┌────────── photon-backend (exe) ──────────┐
                 │                                          │
              photon_daemon                             photon-index / photon-export
                 │      │       │        │                  │
             photon_net photon_hw photon_store photon_synth │
                 │                 │          │             │
                 └──── photon_decode ◄────────┘─────────────┘
                              │
                          photon_wire

  photon-ui / photon-frontend (exe)
                 │
              photon_ui ── photon_imgui, photon_tinygltf, SDL2 | emscripten port
                 │      │
          photon_net  photon_synth   (ws client only; no daemon headers)
                 │
             photon_wire
```

Rules:
- `wire` has no dependencies. `decode` → `wire`. `store` → `decode`. `net` → `wire`. `hw` → `wire`. `synth` → `store`, `decode`.
- `ui` may link `net` (client side), `synth`, `wire`. It may **not** link `daemon`, `store`, `hw`.
- `daemon` links everything except `ui`.
- Tests link the library under test; they never list `.cpp` files from another directory.

### 3.2 Facade per system (Photon-style)

| System | Facade | Owns | Lifecycle |
| --- | --- | --- | --- |
| `daemon` | `struct Daemon` | `Server`, `SourceHub`, service objects, `Clock` | `init(cli) / tick(nowMs) / stop()` |
| `ui` | `struct UI` | `Platform`, `Arena`, `Client`, `ScreenRegistry`, `Theme` | `init() / frame() / shutdown()` |
| `synth` | free functions + `AnalysisRuntime`, `TileCache`, `tracksim::solve` | no I/O, no ImGui | none |
| `store` | `tsdb::Store`, `vsd1::Archive`, `vidx::*`, `LiveTailWriter` | files | RAII |
| `net` | `net::Socket`, `ws::Server`, `ws::Client` | sockets | RAII + `Guard` |

## 4. Phases

Each phase ends green: native configure+build, `ctest`, WASM build, `photon-ui --quit-after-ms 2500` prints `timed quit` / `shutdown ok`. Move-only commits are separate from behavior/refactor commits so `git log --follow` and review stay tractable. Use `git mv`.

### Phase 0 — Freeze and baseline (½ day) — **DONE** `aac24d6` / scratch+triage in `9f62824`

1. Fix `.gitignore`: `.artifacts*/`, `__pycache__/`, `*.pyc`, `.qa-scratch*/`. **DONE**
2. Delete root scratch (`_*.ps1`, `_probe_locate.py`) and agent one-offs (`tools/photon-export/_mut.py`, `frontend/scripts/_dismiss-gpu.ps1`, `tests/revert_check_video_index.ps1`, `tools/photon-export/revert_check.py`). Anything with lasting value (`_hotfix_blank.ps1` → deploy script; `_probe_locate.py` → `tools/qa/`) is re-homed in Phase 1, not deleted. **DONE** (`9f62824`; `_probe_locate.py` → `tools/qa/probe_video_locate.py`)
3. Commit the WIP as-is in reviewable slices: `core`, `backend`, `frontend`, `tools`, `tests`, `assets/docs`. This is the baseline every later diff is measured against. **DONE** `aac24d6`
4. Record baseline: `ctest` pass list, `photon.wasm`/`photon.js`/`photon.data` sizes, native `photon-ui` smoke, `photon-backend --help`.

### Phase 1 — Hygiene and tooling (1 day)

1. Add `.clang-format` (Google base, `ColumnLimit: 110` measured, `IndentWidth: 4`, `PointerAlignment: Left`, `SortIncludes: true`, `NamespaceIndentation: None`). Add `format` / `format-check` targets like Photon's root `CMakeLists.txt`. Run once as a standalone "format" commit. **DONE** (targets + style file this commit; do **not** `clang-format -i` the tree until a dedicated format commit)
2. Triage `backend/scripts/`: **DONE** `9f62824` (`smoke_ws.py` landed in `web/scripts/`)
   - keep → `tools/qa/`: `smoke_ws.py`, `check_pcan_ws.py`, `check_replay_ws.py`, `check_video_ws.py`, `show_*`, `capture_console.py`, `screenshot_*`, `click_*`, `shot_after_click.py`, `_probe_locate.py`
   - keep → `tools/data/`: `log_coverage.py`, `extract_on_track.py`, `export_comp_timeseries.py`, `dump_critical_signals.py`
   - decide → `tools/charge_model/` or a separate repo: `charge_model/` (ML training, has its own README)
   - delete: `opencode_*.py`, `vsd1_read_probe.cpp` (superseded by `tests/test_vsd1.cpp`)
3. Move `assets/maps3d.{html,js}` → `web/js/maps3d.js` + markup into `web/shell.html`; update `native_maps.cpp` / WebView2 host path and the WASM asset URL. **OPEN**
4. `scripts/ci.ps1`: configure+build native, `ctest --output-on-failure`, WASM build, `format-check`. This is the gate for every later phase. **OPEN**
5. Unify target naming: libraries `photon_<system>`, executables `photon-<name>`. Third-party wrappers `photon_imgui`, `photon_tinygltf`. **Partial** (`9f62824`; `photon-core` / `photon-imgui` / `photon-tinygltf` still hyphenated)

### Phase 2 — Build system and layering fix (1–2 days)

Move-only library split: **DONE** `9f62824`. Remaining: cmake modules, embed_assets, tests link-only, `photon-ws` leftover cleanup if any.

1. Create `cmake/{warnings,deps,emscripten,embed_assets}.cmake`; root `CMakeLists.txt` becomes project options + `add_subdirectory(assets)` + `add_subdirectory(photon)` + tests, mirroring Photon's root.
2. Move `backend/src/{net,websocket}.cpp` + `core/src/{ws_handshake,sha1,base64}.cpp` + `client_auth` → `photon/net/` as `photon_net`. Split `websocket.cpp` into `ws/server.cpp` (accept, upgrade, per-connection state) and `ws/client.cpp` (what `frontend/src/client.cpp` uses on native). Frontend links `photon_net`; the `photon-ws` hack in `frontend/CMakeLists.txt` goes away.
3. Move `backend/include/photon/video_framing.hpp` → `photon/wire/` (it is wire format, both sides use it).
4. Move `core/…/pcan.*` → `photon/hw/`.
5. Embed DBC + tracks via `embed_assets.cmake` (Photon does this for dbc/kernels/models). Bundled DBC lookup in `session.cpp` (`assets/dbc/*.dbc` on disk) gains an embedded fallback; `--assets-dir` stays for overrides.
6. `tests/CMakeLists.txt` links libraries only. Any test that still needs a daemon/UI `.cpp` is a sign that code belongs in `synth` or `store` — move it (this is how `analysis_*`, `tile_cache`, `tile_store` end up in `synth`).

### Phase 3 — Core split into `wire` / `decode` / `store` / `synth` (1–2 days)

Move-only into systems: **DONE** `9f62824`. Targeted god-file splits below: **OPEN** (`tsdb.cpp`, `control.cpp`, `DecodeBuffer`, analysis/tile_* still in `ui`).

- `control.cpp` (47 KB) → `wire/control/messages.hpp` (structs), `wire/control/encode.cpp`, `wire/control/decode.cpp`; one table of `type` strings.
- `tsdb.cpp` (102 KB) → `store/tsdb/{format,store,query,compact}.cpp` + existing `live.cpp`; keep `tsdb_internal.hpp` as the only private header.
- `DecodeBuffer` + `SignalRing` (backend `source.hpp`) → `decode/decode_buffer.{hpp,cpp}`.
- `frontend/src/analysis_{runtime,workbook,eval}.cpp`, `tile_cache.cpp`, `tile_store.cpp` → `synth/analysis/`, `synth/history/`. `HistoryIO` interface moves with them; `idb_tile_store.cpp` stays in `ui/platform/web` as the web implementation.

### Phase 4 — Daemon decomposition (3–4 days) — **OPEN** (SessionHub service split)

`SessionHub` (92 KB, ~90 private methods) becomes a `Daemon` facade with one object per concern. Each extraction is one commit; `SessionHub` shrinks until it is renamed `Daemon`.

| Extract | From `SessionHub` members / methods |
| --- | --- |
| `server/listener.cpp`, `server/client_session.{hpp,cpp}` | `listener_`, `clients_`, `acceptNew`, `serviceClient`, `dropClosed`, `noteQueued/Drained`, `OutboundTag` |
| `server/control_router.cpp` | `handleText` → `std::unordered_map<std::string_view, Handler>`; handlers live on services |
| `services/telemetry_stream.cpp` | `streamTelemetry`, `sendTelemetryBlocks`, `refreshDecodeWanted`, `broadcastSnapshots`, `pushSnapshot`, congestion |
| `services/can_stream.cpp` | `streamCan`, `lastCanBurstMs_` |
| `services/timeline_service.cpp` | `streamTimeline`, per-client `ReplayView` bind/reset/tick, `bindClientReplay`, `attachClientLog` |
| `services/video_service.cpp` | `videoRelay_`, `streamVideo`, `streamVideoStatus`, GOP ring / sent-seq tracking on `ClientSession` |
| `services/history_service.cpp` | `HistoryPool`, `historyJobs_`, `historyStores_`, credit flow, `HistoryRemote`, `flushHistory*`, `openHistoryStore`, `HistoryEncodeState` |
| `services/live_recorder.cpp` | `liveWriter_`, `feedLive`, `start/stopLiveWriter`, `recoverCrashedLive`, `tickRetention`, `publishLiveCatalog` |
| `services/catalog_service.cpp` | `driveDays_`, `refreshDriveDays`, `catalogMessage`, `historyCatalogMessage`, `broadcastCatalog` |
| `services/tracksim_service.cpp` | `trackSimJobs_`, `enqueueTrackSim`, `pollTrackSimJobs`, `catalog_` |
| `services/analysis_service.cpp` | `analysisJobs_`, `handleAnalysisCompute/Cancel`, `pollAnalysisJobs`, sidecar addr |
| `services/tx_arm.cpp` | `txArmed`, `checkTxWatchdog`, `disarm` |
| `services/csv_export.cpp` | already separate; `startCsvExport` moves off the hub |

`source.cpp` (55 KB) → `sources/{daq_tcp,log_replay,pcan}.cpp`, `sources/source_hub.cpp`, `sources/replay_view.cpp`. `SyntheticSource` moves to `tests/daemon/fake_source.cpp` (test-only per the 2026-08-31 note) and the UI's `SourceKind::Synthetic` branches go away. `photon-index/main.cpp` (62 KB) → `cli.cpp`, `indexer.cpp`, `live_fanout.cpp` linking `photon_store`.

Acceptance: `tests/daemon/` covers router dispatch, credit flow, tx watchdog, live recorder recovery with the existing `test_session_history`, `test_log_switch`, `test_replay_views`, `test_live_fanout` cases relocated, unchanged in assertions.

### Phase 5 — UI decomposition (4–5 days) — **OPEN** (Client/App split, `custom_view` split, Platform interface)

1. **`Platform` interface** (`ui/platform/platform.hpp`): `nowMs`, `assetPath`, `pageParam`, `wsUrl`, `openUrl`, `copyToClipboard`, `persistKV`, `videoSupported`, `mapsHost`. Two implementations: `platform/web/*.cpp` (all `EM_JS`/`EM_ASM`/`extern "C"` glue consolidated here — today spread across `idb_tile_store`, `gl3d/scene`, `video_decoder`, `custom_view`, `page_url`, `screens/analysis`, `theme`, `log_drawer`, `app`) and `platform/native/*.cpp` (`platform.cpp`, `gl_loader`, `native_maps`, `native_video`, crash handler from `main.cpp`). `UI` takes a `Platform&`; screens never `#ifdef __EMSCRIPTEN__`.
2. **`transport/`**: split `Client` (43 KB) into `Connection` (socket, reconnect backoff, heartbeat), `Hello` (ack, features, source adoption), `Interest` (subscribe/unsubscribe/CAN/video wanted sets, `begin/commitInterest`), `ControlHandlers` (one function per inbound `type`), `HistoryTransport` (`requestHistory`/`cancelHistory`, catalog). Drive-day heuristics (`maybeAdoptDriveDay`, `bestCatalogForYmd`) move to `state/catalog`.
3. **`state/`**: `ClientArena` → `Arena` (rings, CAN rows, cells) + `TimelineState` (mode, cursor, preview/follow cursors, pinned playback) + `VideoState` + `TrackSimState` + `ConnectionInfo` (source/host/port/dbc/features). Public fields on `ClientArena` become accessors.
4. **`app/`**: `App` → `UI` facade; `ScreenRegistry` replaces the `dynamic_cast` lookup and `strcmp` id checks (`Screen` gains `wantsMaps()`, `attach(UI&)`). Titlebar and shell extracted from `app.cpp`.
5. **`screens/plots/`**: `custom_view.cpp` (101 KB) → `views.cpp` (layout + persistence), `plot_panel.cpp`, `range_select.cpp`, `scrub.cpp`, `export.cpp` (PNG/CSV), `import_export_json.cpp`. `dashboard.cpp` (60 KB) → `dashboard.cpp` + `dashboard_tiles.cpp` + `dashboard_debug.cpp`.
6. **`render/`**: `gl3d/` + `gl_api` + `gl_loader`; `scene.cpp` loses its OSM/maps bridge (→ `maps/`).
7. **`video/`**: `video_decoder.cpp` → `decoder.hpp` interface, `web_codecs_decoder.cpp`, `mf_decoder.cpp` (native), `release.cpp` (pacing), `hud.cpp`.
8. `photon_ui` becomes a library; `photon-ui` (native) and `photon-frontend` (WASM) are thin `main.cpp`s that construct `UI` with the right `Platform`.

### Phase 6 — Web shell (1 day) — **OPEN** (`shell.html` JS extraction; `frontend/scripts` already → `web/scripts` in `9f62824` / `f3911cb`)

1. `frontend/shell.html` → `web/shell.html` markup + `web/js/{module,crash,fetch_remap,tilecache,maps3d,video}.js` loaded by `<script src>` (or `--pre-js` for `Module`). Version stamp (`photon.js?v=<git-sha>`) generated by CMake, not hand-edited.
2. `web/scripts/deploy-to-photon-server.ps1` replaces `_hotfix_blank.ps1` + `_verify_wasm_ui.ps1`: copies `photon.{html,js,wasm,data}` + `models/` to `photon-server/ui/`, prints the version it shipped.
3. WASM build becomes a CMake preset (`CMakePresets.json`: `native-debug`, `native-release`, `web-release`) so `frontend/scripts/build.ps1` is a one-liner.

### Phase 7 — Tests and CI (1 day) — **OPEN** (per-system test executables)

1. One test executable per system under `tests/<system>/`, shared `harness.hpp`, `add_test` with `LABELS`. `tools/photon-export/test_export.cpp` → `tests/tools/`.
2. `ctest -L wire|store|daemon|ui` runs in `scripts/ci.ps1`; e2e Python (`smoke_ws.py`, `check_*_ws.py`, `test_video_webcodecs.cjs`) under `tests/e2e/` with a `--backend` flag, not auto-run.
3. Add missing unit coverage created by the split: control router dispatch table, `Interest` set diffing, `TimelineState` cursor precedence (`displayCursorMs` / `videoCursorMs` / `effectiveCursorMs`).

### Phase 8 — Docs (½ day)

1. `README.md` rewritten in Photon's style: a systems glyph table (`wire / decode / store / net / hw / synth / daemon / ui / web`), build presets, run matrix (PC console vs laptop vs browser). Protocol tables move to `docs/protocol.md` and are completed from `control.hpp`. **Partial** (root README + systems table this commit; `docs/protocol.md` still **OPEN**)
2. `docs/architecture.md`: §3 of this file + the daemon tick order and the UI frame order. **OPEN**
3. Per-system `README.md` (5–10 lines: what it owns, what it may link). **DONE** this commit
4. Update `.memory/context/2026-08-18-photon-next-rewrite-state.md` layout section or supersede it. **OPEN**

## 5. Order and effort

| Phase | Effort | Blocks |
| --- | --- | --- |
| 0 Freeze | ½ d | everything |
| 1 Hygiene | 1 d | — |
| 2 Build + layering | 1–2 d | 3, 4, 5 |
| 3 Core split | 1–2 d | 4, 5 |
| 4 Daemon | 3–4 d | — |
| 5 UI | 4–5 d | 6 |
| 6 Web shell | 1 d | — |
| 7 Tests/CI | 1 d | can start after 2 |
| 8 Docs | ½ d | last |

Roughly three weeks of focused work; 4 and 5 can run in parallel after 3.

## 6. Non-goals

- No protocol v1 changes. Binary header, msgTypes, and control `type` strings stay byte-identical; `photon-server` and the deployed WASM page keep working across every phase.
- No behavior changes to plots, video pacing, history credit flow, or live recorder. Those have `.memory/context/2026-09-*` bug histories; a refactor commit that touches them must cite the note it preserves.
- No new dependencies. In-repo JSON/SHA-1/Base64 stay. No gtest.
- No changes to `F:\lhr\Photon` or `F:\lhr\photon-server`.
- Not adopting Photon's Vulkan `gpu` layer, Tracy, or hot-reload. WebGL2/GLES3 via SDL2 is the render path for both targets.

## 7. Decisions to confirm before Phase 2

1. **Directory root:** `photon/<system>/` (this plan, mirrors Photon) vs. keeping `core/backend/frontend` as top-level with systems underneath. Recommendation: `photon/<system>/`.
2. **Style:** keep photon-next's 4-space / trailing-underscore style and enforce it, vs. adopting Photon's 2-space Google style. Recommendation: keep 4-space (avoids a 60 k-line reformat mixed into the review).
3. **`charge_model/`:** move to `tools/charge_model/` or a separate repo. Recommendation: separate repo; it has its own README, training loop and no C++ consumers.
4. **Embedded DBC:** embed bundled DBCs like Photon (`DbcHeaders/DbcAssets`) or keep loading from `assets/dbc/` on disk. Recommendation: embed with on-disk override.
