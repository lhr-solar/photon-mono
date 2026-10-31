"""photon clean [--deps]: remove build output. data/ holds telemetry and is never touched."""

from __future__ import annotations

import os
import shutil
import stat
import sys

from photon import procs
from photon.common import ROOT, UI_DIR, fail, rel, state


def _size_mb(path) -> str:
    total = 0
    for folder, _, files in os.walk(path):
        for name in files:
            try:
                total += os.lstat(os.path.join(folder, name)).st_size
            except OSError:
                pass
    return "{:,.0f} MB".format(total / (1024 * 1024))


def _clear_readonly(func, path, _):
    # git marks its objects read-only, which stops rmtree on Windows.
    os.chmod(path, stat.S_IWRITE)
    func(path)


def _remove(path) -> None:
    if sys.version_info >= (3, 12):
        shutil.rmtree(path, onexc=_clear_readonly)
    else:
        shutil.rmtree(path, onerror=_clear_readonly)


def main(words) -> None:
    deps = False
    for word in words:
        if word.lower() == "--deps":
            deps = True
        else:
            fail("unknown clean word: " + word, 2)
    # build/ and photon-next/.artifacts* are where builds went before out/.
    dirs = [ROOT / "out", ROOT / "build"]
    dirs += sorted(p for p in UI_DIR.glob(".artifacts*") if p.is_dir())
    if deps:
        # Fetched libraries and emsdk; the next build fetches them again, setup reinstalls emsdk.
        dirs.append(UI_DIR / ".external")
    dirs = [d for d in dirs if d.exists()]
    if not dirs:
        print("nothing to clean")
        return
    running = procs.running_under(dirs)
    if running:
        fail("stop these first: " + procs.describe(running))
    for folder in dirs:
        size = _size_mb(folder)
        if state.dry_run:
            print("would remove " + rel(folder) + "  " + size)
            continue
        _remove(folder)
        print("removed " + rel(folder) + "  " + size)
