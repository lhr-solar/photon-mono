"""Paths, platform facts, and the helpers every photon command shares."""

from __future__ import annotations

import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SERVER_DIR = ROOT / "photon-server"
UI_DIR = ROOT / "photon-next"
# Build trees (the CMake presets put theirs here) and the runnable bundles assembled from them.
BUILD_ROOT = ROOT / "out" / "build"
GO_OUT = BUILD_ROOT / "go"
WEB_TREE = BUILD_ROOT / "web-release"
DIST = ROOT / "out" / "dist"

IS_WINDOWS = sys.platform == "win32"
IS_MAC = sys.platform == "darwin"
IS_LINUX = sys.platform.startswith("linux")
EXE = ".exe" if IS_WINDOWS else ""


class _State:
    # --dry-run: print each step instead of running it.
    dry_run = False


state = _State()


def exe(name: str) -> str:
    return name + EXE


def rel(path) -> str:
    """A path as a person reads it: relative to the repo when it is inside it."""
    p = Path(path)
    try:
        return str(p.resolve().relative_to(ROOT))
    except ValueError:
        return str(p)


def fail(message: str, code: int = 1):
    print(message, flush=True)
    sys.exit(code)


def _quote(text: str) -> str:
    return '"' + text + '"' if " " in text and not text.startswith('"') else text


def show_command(title: str, cmd, cwd=None) -> None:
    print(title + ":")
    if cwd:
        print("  cd " + str(cwd))
    print("  " + " ".join(_quote(str(c)) for c in cmd), flush=True)


def resolve(program: str):
    """The program's full path from PATH, or None. Finds .cmd and .bat files on Windows too."""
    return shutil.which(program)


def run_step(title: str, cmd, cwd=None, env=None) -> None:
    """Run one step of a command. A failure ends photon with that step's exit code."""
    cmd = [str(c) for c in cmd]
    if state.dry_run:
        show_command(title, cmd, cwd)
        return
    program = resolve(cmd[0])
    if program is None and not Path(cmd[0]).exists():
        fail(cmd[0] + " is not on PATH. Run: photon setup")
    print("", flush=True)
    code = subprocess.call([program or cmd[0]] + cmd[1:], cwd=cwd, env=env)
    if code != 0:
        sys.exit(code)


def output(cmd, cwd=None) -> str:
    """A command's stdout, or "" if it is missing or fails. For checks, never for steps."""
    program = resolve(str(cmd[0]))
    if program is None:
        return ""
    try:
        result = subprocess.run([program] + [str(c) for c in cmd[1:]], cwd=cwd, capture_output=True, text=True)
    except OSError:
        return ""
    return result.stdout.strip() if result.returncode == 0 else ""
