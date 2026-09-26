# tools/qa

Protocol smoke and browser QA: `check_*_ws.py`, `show_*`, `screenshot_*`, `click_*`, `capture_console.py`.

**Owns:** talking to a running `photon-backend` / `photon.html` to assert protocol and UI behavior.

**Must not:** production C++, TSDB writers, or log-analysis notebooks (`tools/data`, `tools/charge_model`). Not linked by CMake.
