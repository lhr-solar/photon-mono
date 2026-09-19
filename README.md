─γ─ photon-next

Rewrite of Longhorn Racing Solar's Photon telemetry stack. A native daemon owns
hardware, network, files, DBC decode, and storage; a Dear ImGui/ImPlot client
(WASM page or native exe) speaks protocol v1 over a localhost WebSocket.

```
      photon-server            USB PCAN         VSD1 logs
    :6500 CANP  :6700               │                │
        │          │           ┌────┴────┐      ┌────┴──────┐
   ┌────┴───┐ ┌────┴─────┐     │readThread│     │indexThread│
   │ daqTcp │ │videoRelay│     └────┬────┘      └────┬──────┘
   └────┬───┘ └────┬─────┘          │                │
        └──────────┼────────────────┼────────────────┘
                   ▼                ▼
             ┌───────────────────────────┐
             │    decodeBuffer (rings)   │  DBC → signal rings
             └────────────┬──────────────┘
                          ▼
   ┌─────────────┐  ┌───────────┐  ┌────────────────┐
   │ historyPool ├──┤sessionHub ├─►│  ws clients    │  127.0.0.1:8765
   │  (workers)  │  │(poll loop)│  │ photon.html    │
   └──────┬──────┘  └─────┬─────┘  │ photon-ui.exe  │
          │               │        └────────────────┘
          ▼               ▼
    ┌───────────┐   ┌──────────────────┐
    │ tsdb store│   │ tracksim/analysis│  worker futures
    └───────────┘   └──────────────────┘

   ┌──────────┐  ws   ┌───────┐    ┌─────────┐
   │ wsClient ├──────►│ arena ├───►│ screens │  UI frame loop
   └──────────┘       └───────┘    └─────────┘
```

─< base   >─ json · endian
─< wire   >─ protocol v1 · control plane · canp
─< decode >─ dbc · signal rings
─< store  >─ tsdb · vsd1 · video index
─< net    >─ sockets · websocket
─< hw     >─ pcan
─< synth  >─ tracksim · analysis
─< daemon >─ photon-backend · photon-index
─< ui     >─ photon-ui.exe · photon.html

─< Build >─────────────────────────────

```powershell
cmake -S . -B .artifacts -G Ninja -D CMAKE_C_COMPILER=clang -D CMAKE_CXX_COMPILER=clang++
cmake --build .artifacts
ctest --test-dir .artifacts --output-on-failure
```

WASM page:

```powershell
.\web\scripts\build.ps1
```

─< Run >───────────────────────────────

The PC console is started by photon-server itself. On a laptop:

```powershell
.\.artifacts\photon\daemon\photon-backend.exe --no-live-tail --daq-host <pc-ip>
.\web\scripts\serve.ps1      # or run .artifacts\photon-ui.exe --ws ws://127.0.0.1:8765
```

Open http://127.0.0.1:8080/photon.html (never file://).

─< Docs >──────────────────────────────

- [docs/protocol.md](docs/protocol.md) — wire format, control plane, sources, CANP, track-sim
- [docs/build.md](docs/build.md) — binaries, flags, emsdk pins, screens
- [docs/cleanup-plan.md](docs/cleanup-plan.md) — restructure history and open work
- per-system `README.md` under `photon/<system>/`
