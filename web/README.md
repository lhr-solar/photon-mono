# web/

Emscripten shell and host scripts for the WASM UI. `shell.html` is the `--shell-file` for `photon-frontend`.
`scripts/setup-emsdk.ps1`, `build.ps1`, `serve.ps1`, `serve_nocache.py`, `smoke_ws.py`.

**Owns:** HTML shell, emsdk/build/serve glue, protocol smoke against a running backend.

**Must not:** C++ sources, daemon logic, or Python QA/data science (those live in `photon/ui`, `photon/daemon`, `tools/qa`, `tools/data`).
