# photon/ui

Dear ImGui + ImPlot client (same sources for both targets). Speaks protocol v1 to `photon-backend`.
Native: `photon-ui.exe` (`desktop-*` presets, `desktop` install component). WASM: `photon-frontend` → `photon.html` in the `web-release` tree (shell in `web/`, `web` install component).

**May link:** `photon-core` (umbrella), imgui / implot / tinygltf, and `photon_net` on native.

**Must not:** include daemon headers (`photon/session.hpp`, sources, CLI). No `photon_daemon`, no `photon_hw`.
See the repo [README](../../README.md) for build / serve / flags.
