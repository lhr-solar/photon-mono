# src/ui

Dear ImGui + ImPlot client (same sources for both targets). Speaks protocol v1 to `photon-backend`.
Native: `photon-ui.exe`. WASM: `photon-frontend` → `.artifacts-web/photon.html` (shell in `web/`).

**May link:** `photon-core` (umbrella), imgui / implot / tinygltf, and `photon_net` on native.

**Must not:** include daemon headers (`daemon/session.hpp`, sources, CLI). No `photon_daemon`, no `photon_hw`.
See the repo [README](../../README.md) for build / serve / flags.
