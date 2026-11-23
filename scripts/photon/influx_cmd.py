"""photon influx: run InfluxDB 3 Core the way photon-server expects it.

photon-server's DB writer flushes every 250 ms (-influx-flush), so InfluxDB's
WAL flushes every 250 ms too; together rows are queryable in about 500 ms.
InfluxDB is installed by hand (see photon help); this only starts it.
"""

from __future__ import annotations

import glob
import os
from pathlib import Path

from photon.common import EXE, fail, resolve, run_step
from photon.run_cmd import data_dir

# What photon-server's -influx-url defaults to.
DEFAULT_BIND = "127.0.0.1:8181"
WAL_FLUSH = "250ms"


def find_influxdb3():
    """influxdb3 from PHOTON_INFLUXDB3, PATH, or the newest version under
    %LOCALAPPDATA%\\influxdb3\\<version> (where it is unpacked on Windows)."""
    pinned = os.environ.get("PHOTON_INFLUXDB3", "").strip()
    if pinned:
        return pinned if Path(pinned).exists() else None
    found = resolve("influxdb3")
    if found:
        return found
    local = os.environ.get("LOCALAPPDATA", "")
    if local:
        candidates = glob.glob(os.path.join(local, "influxdb3", "*", "influxdb3" + EXE))

        def version(path):
            name = Path(path).parent.name
            return tuple(int(p) if p.isdigit() else 0 for p in name.split("."))

        if candidates:
            return max(candidates, key=version)
    return None


def main(words) -> None:
    """photon influx [--auth] [--bind HOST:PORT] [more influxdb3 serve flags]"""
    auth = False
    bind = DEFAULT_BIND
    passthrough = []
    i = 0
    while i < len(words):
        w = words[i]
        if w == "--auth":
            auth = True
        elif w == "--bind" and i + 1 < len(words):
            bind = words[i + 1]
            i += 1
        else:
            passthrough.append(w)
        i += 1

    exe = find_influxdb3()
    if exe is None:
        fail("influxdb3 not found. Unpack InfluxDB 3 Core into %LOCALAPPDATA%\\influxdb3\\<version>, "
             "put influxdb3 on PATH, or set PHOTON_INFLUXDB3 (see photon help).")

    data = data_dir() / "influxdb3"
    data.mkdir(parents=True, exist_ok=True)
    cmd = [exe, "serve",
           "--node-id", "photon",
           "--object-store", "file",
           "--data-dir", data,
           "--http-bind", bind,
           "--wal-flush-interval", WAL_FLUSH]
    if not auth:
        # Local development: no tokens. Bound to loopback by default, so only this
        # machine can write. Use --auth on a shared machine and give photon-server
        # the token with PHOTON_INFLUX_TOKEN.
        cmd.append("--without-auth")
    run_step("InfluxDB 3 Core on " + bind + " (data in " + str(data) + ")", cmd + passthrough)
