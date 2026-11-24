"""photon influx: run InfluxDB 3 the way photon-server expects it.

InfluxDB 3 Enterprise (free At-Home licence; the team is a non-profit) is used
when it is installed, else Core. Enterprise merges its data files (compaction);
Core never does.

photon-server's DB writer flushes every 250 ms (-influx-flush), so InfluxDB's
WAL flushes every 250 ms too; together rows are queryable in about 500 ms.
InfluxDB is installed by hand (see photon help); this only starts it.

Memory is capped for a small server (about 1 GB with photon-server on the same
machine): a history query that needs more than the query pool fails with an
error instead of growing until the machine runs out. --mem-default leaves
InfluxDB's own defaults (percentages of the machine's RAM).
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

# Enterprise: the cluster ID prefixes its catalog path. It must differ from the node ID.
NODE_ID = "photon"
CLUSTER_ID = "photon-cluster"
# The At-Home licence allows at most 2 cores.
ENTERPRISE_CORES = "2"

# Memory caps for a ~1 GB machine shared with photon-server (see the module doc).
MEMORY_CAPS = [
    "--exec-mem-pool-size", "192MB",  # query execution; over it a query fails
    "--file-cache-size", "64MB",      # in-memory data file cache
]
# Core's Parquet engine: write buffer before a forced snapshot (Enterprise's
# PachaTree engine ignores this flag and warns about it).
CORE_MEMORY_CAPS = [
    "--force-snapshot-mem-size", "128MB",
]
# Enterprise (PachaTree engine). Its defaults scale with the machine: the replica
# buffer takes 50% of RAM (up to 16 GB), a snapshot bucket 250 MB, compaction 50%
# of RAM, and at least 50 queries may run at once (photon-server sends at most 6).
ENTERPRISE_MEMORY_CAPS = [
    "--replica-max-buffer-size", "128MB",
    "--snapshot-size", "64MB",
    "--compactor-input-size-budget", "128MB",
    "--compactor-max-concurrent-merges", "1",
    "--max-concurrent-queries", "16",  # its advisory minimum; photon-server sends at most 6
]


def _newest(pattern):
    def version(path):
        name = Path(path).parent.name
        return tuple(int(p) if p.isdigit() else 0 for p in name.split("."))

    found = glob.glob(pattern)
    return max(found, key=version) if found else None


def find_influxdb3():
    """(path, edition) of influxdb3: PHOTON_INFLUXDB3, then the newest Enterprise
    under %LOCALAPPDATA%\\influxdb3-enterprise\\<version>, then influxdb3 on PATH,
    then the newest Core under %LOCALAPPDATA%\\influxdb3\\<version>. The edition is
    read from `influxdb3 --version` for the first and third."""
    pinned = os.environ.get("PHOTON_INFLUXDB3", "").strip()
    if pinned:
        return (pinned, _edition(pinned)) if Path(pinned).exists() else (None, None)
    local = os.environ.get("LOCALAPPDATA", "")
    if local:
        ent = _newest(os.path.join(local, "influxdb3-enterprise", "*", "influxdb3" + EXE))
        if ent:
            return ent, "enterprise"
    found = resolve("influxdb3")
    if found:
        return found, _edition(found)
    if local:
        core = _newest(os.path.join(local, "influxdb3", "*", "influxdb3" + EXE))
        if core:
            return core, "core"
    return None, None


def _edition(exe):
    import subprocess

    try:
        out = subprocess.run([exe, "--version"], capture_output=True, text=True, timeout=10).stdout
    except (OSError, subprocess.SubprocessError):
        return "core"
    return "enterprise" if "Enterprise" in out else "core"


def main(words) -> None:
    """photon influx [--auth] [--bind HOST:PORT] [--core] [--mem-default] [more influxdb3 serve flags]

    Enterprise needs the At-Home licence on first start: set PHOTON_INFLUX_LICENSE_EMAIL
    (the address that receives the verification email; click its button once). After
    that the licence is kept in the data folder and the address is not needed."""
    auth = False
    bind = DEFAULT_BIND
    force_core = False
    caps = True
    passthrough = []
    i = 0
    while i < len(words):
        w = words[i]
        if w == "--auth":
            auth = True
        elif w == "--bind" and i + 1 < len(words):
            bind = words[i + 1]
            i += 1
        elif w == "--core":
            force_core = True
        elif w == "--mem-default":
            caps = False
        else:
            passthrough.append(w)
        i += 1

    if force_core:
        local = os.environ.get("LOCALAPPDATA", "")
        exe = _newest(os.path.join(local, "influxdb3", "*", "influxdb3" + EXE)) if local else None
        edition = "core" if exe else None
    else:
        exe, edition = find_influxdb3()
    if exe is None:
        fail("influxdb3 not found. Unpack InfluxDB 3 Enterprise into %LOCALAPPDATA%\\influxdb3-enterprise\\<version> "
             "(or Core into %LOCALAPPDATA%\\influxdb3\\<version>), put influxdb3 on PATH, "
             "or set PHOTON_INFLUXDB3 (see photon help).")

    # Enterprise gets its own folder: its catalog differs from Core's and an upgrade
    # cannot be undone, so the Core data is left as it was.
    data = data_dir() / ("influxdb3-enterprise" if edition == "enterprise" else "influxdb3")
    data.mkdir(parents=True, exist_ok=True)
    cmd = [exe, "serve",
           "--node-id", NODE_ID,
           "--object-store", "file",
           "--data-dir", data,
           "--http-bind", bind,
           "--wal-flush-interval", WAL_FLUSH]
    if caps:
        cmd += MEMORY_CAPS
        if edition != "enterprise":
            cmd += CORE_MEMORY_CAPS
    if edition == "enterprise":
        cmd += ["--cluster-id", CLUSTER_ID]
        # --num-cores must be 2 or more and at most the machine's cores, so it is only
        # needed (and only allowed) on a machine with more cores than the licence.
        if (os.cpu_count() or 1) > int(ENTERPRISE_CORES):
            cmd += ["--num-cores", ENTERPRISE_CORES]
        if caps:
            cmd += ENTERPRISE_MEMORY_CAPS
        licence = data / CLUSTER_ID / "trial_or_home_license"
        email = os.environ.get("PHOTON_INFLUX_LICENSE_EMAIL", "").strip()
        if email:
            cmd += ["--license-type", "home", "--license-email", email]
        elif not licence.exists():
            fail("InfluxDB 3 Enterprise needs its free At-Home licence on first start: set "
                 "PHOTON_INFLUX_LICENSE_EMAIL to the address that should receive the verification "
                 "email, run photon influx, and click the button in that email. Or use --core.")
    if not auth:
        # Local development: no tokens. Bound to loopback by default, so only this
        # machine can write. Use --auth on a shared machine and give photon-server
        # the token with PHOTON_INFLUX_TOKEN.
        cmd.append("--without-auth")
    name = "InfluxDB 3 Enterprise" if edition == "enterprise" else "InfluxDB 3 Core"
    run_step(name + " on " + bind + " (data in " + str(data) + ")", cmd + passthrough)
