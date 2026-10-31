# web/

Emscripten shell and host scripts for the WASM UI. `shell.html` is the `--shell-file` for `photon-frontend`.
`scripts/serve.ps1`, `serve_nocache.py`, `smoke_ws.py`. emsdk and the page build are in the repo's `photon` command (`scripts/photon/web.py`).

**Owns:** HTML shell, emsdk/build/serve glue, protocol smoke against a running backend.

**Must not:** C++ sources, daemon logic, or Python QA/data science (those live in `photon/ui`, `photon/daemon`, `tools/qa`, `tools/data`).
