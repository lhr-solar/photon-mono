"""photon test [go] [native] [e2e] [--config Debug|Release] [--strict]. No words runs all three."""

from __future__ import annotations

import os
import sys

from photon.build_cmd import native_preset
from photon.common import BUILD_ROOT, DIST, ROOT, SERVER_DIR, UI_DIR, fail, resolve, run_step, state
from photon.selection import SelectionError, read_selection


def main(words) -> None:
    run = {"go": False, "native": False, "e2e": False}
    config = "Debug"
    strict = False
    i = 0
    while i < len(words):
        word = words[i].lower()
        if word in ("--config", "-config") and i + 1 < len(words):
            try:
                config = read_selection(["--config", words[i + 1]]).config
            except SelectionError as e:
                fail(str(e), 2)
            i += 2
            continue
        if word == "--strict":
            strict = True
        elif word in run:
            run[word] = True
        else:
            fail("unknown test word: " + words[i], 2)
        i += 1
    if not any(run.values()):
        run = dict.fromkeys(run, True)

    if run["go"]:
        run_step("go", ["go", "vet", "./..."], cwd=SERVER_DIR)
        # cache is the unfinished one-second tally; its tests are its spec until it is written.
        run_step("go", ["go", "test", "-count=1", "./server", "./observer", "./sshd", "./cmd/..."], cwd=SERVER_DIR)
        print("go: server/cache not run (unfinished; go test ./cache in photon-server)")

    if run["native"]:
        presets = [native_preset(desktop, config) for desktop in (True, False)]
        presets = [p for p in presets if (BUILD_ROOT / p / "CMakeCache.txt").exists()]
        if not presets and not state.dry_run:
            fail("no native build tree for " + config + ". Run: photon build --config " + config)
        for preset in presets:
            # photon build makes only the shipped targets; the test executables come from here.
            run_step("cmake", ["cmake", "--build", "--preset", preset], cwd=UI_DIR)
            run_step("ctest", ["ctest", "--preset", preset], cwd=UI_DIR)
        if resolve("node"):
            run_step("node", ["node", ROOT / "tests" / "web" / "test_video_webcodecs.cjs"])
        else:
            print("node: not on PATH, video decoder glue test not run")

    if run["e2e"]:
        e2e = ROOT / "tests" / "e2e"
        env = dict(os.environ, PHOTON_DIST_DIR=str(DIST))
        if strict:
            env["PHOTON_E2E_STRICT"] = "1"
        run_step("e2e", [sys.executable, "-m", "unittest", "discover", "-s", e2e, "-t", e2e, "-v"], cwd=e2e, env=env)
