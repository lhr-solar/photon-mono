# tools/data

Offline log analysis: VSD1 coverage (`log_coverage.py`), on-track extract, competition timeseries, critical-signal dumps.

**Owns:** read-only Python over `assets/dbc` + VSD1 / export files. No daemon, no browser.

**Must not:** protocol smoke, screenshots, or C++ (`tools/qa`, `photon/store`). Charge-model training is `tools/charge_model/`.
