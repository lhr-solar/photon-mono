"""photon build: build the pieces named, then assemble one runnable bundle each under out/dist."""

from __future__ import annotations

import os
import shlex
import shutil
import subprocess
from pathlib import Path

from photon import procs, web
from photon.common import (BUILD_ROOT, DIST, GO_OUT, IS_WINDOWS, ROOT, SERVER_DIR, UI_DIR, WEB_TREE, exe, fail,
                           output, rel, resolve, run_step, show_command, state)
from photon.selection import Selection, SelectionError, format_selection, read_selection
from photon.usage import USE


def native_preset(desktop: bool, config: str) -> str:
    """The native preset for a build: desktop only when photon-ui is wanted, so a server
    or web-only client never fetches SDL, ImGui, or WebView2."""
    return ("desktop" if desktop else "headless") + "-" + config.lower()


def check_toolchain(sel: Selection, words) -> None:
    need = []
    if sel.server or sel.client or sel.tools:
        need += ["cmake", "ninja", "clang"]
    if sel.server or sel.tools:
        need.append("go")
    if sel.desktop or sel.web:
        need.append("git")
    missing = [tool for tool in need if resolve(tool) is None]
    if missing and not state.dry_run:
        fail("missing " + ", ".join(missing) + ". Run: photon setup " + " ".join(words))


def bundles(sel: Selection) -> dict:
    """What each bundle under out/dist holds: install components from the native tree,
    Go programs from out/build/go, and the page (the web component) from the web tree."""
    dbc = str(Path("assets/dbc/carcan_telemcan.dbc"))
    tracks = str(Path("assets/tracks"))
    out = {}
    if sel.server:
        out["server"] = dict(native=["server"], go=[exe("photon-server"), exe("photon-sshd")], web=False,
                             expect=[exe("photon-server"), exe("photon-sshd"), exe("photon-index"), dbc])
    if sel.client:
        native = ["client"]
        expect = [exe("photon-backend"), dbc, tracks]
        if sel.desktop:
            native.append("desktop")
            expect += [exe("photon-ui"), str(Path("assets/fonts")), str(Path("assets/maps3d.html"))]
        if sel.web:
            expect += [str(Path("ui/photon.html")), str(Path("ui/photon.wasm"))]
        out["client"] = dict(native=native, go=[], web=sel.web, expect=expect)
    if sel.server and sel.client and sel.web:
        out["station"] = dict(native=["server", "client"], go=[exe("photon-server"), exe("photon-sshd")], web=True,
                              expect=[exe("photon-server"), exe("photon-index"), exe("photon-backend"), dbc, tracks,
                                      str(Path("ui/photon.html")), str(Path("ui/models"))])
    if sel.tools:
        out["tools"] = dict(native=["tools"], go=[exe("photon-observer")], web=False,
                            expect=[exe("photon-export"), exe("photon-observer")])
    # Only programs linked with /DEBUG on Windows leave a .pdb; an empty bundle is dropped.
    out["symbols"] = dict(native=["symbols"], go=[], web=False, expect=[], optional=True)
    return out


def bundle_readme(name: str, folder: Path) -> str:
    """The run lines for one bundle, in this OS's shell."""
    run = "" if IS_WINDOWS else "./"
    set_pw = "set PHOTON_PASSWORD=<password>" if IS_WINDOWS else "export PHOTON_PASSWORD=<password>"
    sep = os.sep
    if name == "server":
        return (f"Run from this folder:\n  {set_pw}\n  {run}{exe('photon-server')} -data-dir data -ui off\n\n"
                "The DAQ dials TCP 6100. Clients read the CANP fan-out on 6500 and video on 6700.\n"
                f"{exe('photon-index')} writes the TSDB to data{sep}tsdb. {exe('photon-server')} -h lists the flags.\n")
    if name == "station":
        return (f"Run from this folder:\n  {set_pw}\n  {run}{exe('photon-server')} -data-dir data\n\n"
                "The server starts photon-index and the local UI, then opens\n"
                "http://127.0.0.1:8080/photon.html, which asks for the password once.\n")
    if name == "client":
        text = ("Run from this folder, the backend first:\n"
                f"  {run}{exe('photon-backend')} --no-live-tail --assets-dir assets --daq-host <server> "
                "--password <password>\n"
                "Other sources: --source pcan, or --source log_replay --log-path <log or folder>.\n\nThen a UI:\n")
        if (folder / exe("photon-ui")).exists():
            text += f"  {run}{exe('photon-ui')}\n"
        if (folder / "ui" / "photon.html").exists():
            python = "python" if IS_WINDOWS else "python3"
            text += (f"  {python} -m http.server 8080 --bind 127.0.0.1 --directory ui\n"
                     "  and open http://127.0.0.1:8080/photon.html\n")
        return text
    if name == "tools":
        return (f"{run}{exe('photon-export')} --store <data{sep}tsdb> --list-sessions\n"
                f"{run}{exe('photon-export')} --store <data{sep}tsdb> --session <id> --out run.parquet --all-signals\n"
                f"{exe('photon-observer')} watches a running server's status; -h lists its flags.\n")
    return "Debug symbols for the .exe files built from the same commit.\n"


def commit_label() -> str:
    commit = output(["git", "-C", ROOT, "rev-parse", "--short", "HEAD"])
    if not commit:
        return "commit unknown"
    if output(["git", "-C", ROOT, "status", "--porcelain"]):
        return "commit " + commit + " with local changes"
    return "commit " + commit


def install_bundle(name: str, bundle: dict, tree: Path, label: str) -> None:
    """Install into a fresh staging folder, check it, and only then replace the old bundle,
    so a failed build never leaves a bundle that looks complete."""
    staging = DIST / ".staging" / name
    final = DIST / name
    installs = [(tree, component) for component in bundle["native"]]
    if bundle["web"]:
        installs.append((WEB_TREE, "web"))
    if state.dry_run:
        for src, component in installs:
            show_command("install", ["cmake", "--install", src, "--component", component, "--prefix", staging])
        for go in bundle["go"]:
            show_command("copy", [GO_OUT / go, "->", staging])
        show_command("swap", [staging, "->", final])
        return
    if staging.exists():
        shutil.rmtree(staging)
    staging.mkdir(parents=True)
    for src, component in installs:
        result = subprocess.run([resolve("cmake") or "cmake", "--install", str(src), "--component", component,
                                 "--prefix", str(staging)], capture_output=True, text=True)
        if result.returncode != 0:
            print(result.stdout + result.stderr)
            fail("installing " + component + " into the " + name + " bundle failed")
    for go in bundle["go"]:
        shutil.copy2(GO_OUT / go, staging)
    if bundle.get("optional") and not any(staging.iterdir()):
        # Nothing for this build, so an older one would only mislead.
        staging.rmdir()
        if final.exists():
            shutil.rmtree(final)
        return
    missing = [item for item in bundle["expect"] if not (staging / item).exists()]
    if missing:
        fail("the " + name + " bundle is missing " + ", ".join(missing) + "; left in " + rel(staging))
    readme = "Photon " + name + ", " + label + "\n\n" + bundle_readme(name, staging)
    if IS_WINDOWS:
        readme = readme.replace("\n", "\r\n")
    (staging / "README.txt").write_bytes(readme.encode("utf-8"))
    if final.exists():
        shutil.rmtree(final)
    staging.rename(final)
    print("bundle " + rel(final))


def main(words) -> None:
    try:
        sel = read_selection(words)
    except SelectionError as e:
        fail(str(e), 2)
    if sel.rest:
        print("unknown build word: " + " ".join(sel.rest))
        fail(USE, 2)
    preset = native_preset(sel.desktop, sel.config)
    tree = BUILD_ROOT / preset
    planned = bundles(sel)
    print("build " + format_selection(sel) + ", " + sel.config + ", preset " + preset)
    check_toolchain(sel, words)

    # Fail before building, not after, if a bundle about to be replaced is running.
    busy = procs.running_under([DIST / name for name in planned])
    if busy and not state.dry_run:
        fail("stop these first, they run from " + rel(DIST) + ": " + procs.describe(busy))

    targets = []
    if sel.server:
        targets.append("photon-index")
    if sel.client:
        targets.append("photon-backend")
    if sel.desktop:
        targets.append("photon-ui")
    if sel.tools:
        targets.append("photon-export")
    # PHOTON_CMAKE_ARGS: extra configure arguments, e.g. a cross-compile target.
    extra = shlex.split(os.environ.get("PHOTON_CMAKE_ARGS", ""), posix=not IS_WINDOWS)
    run_step("cmake", ["cmake", "--preset", preset] + extra, cwd=UI_DIR)
    run_step("cmake", ["cmake", "--build", "--preset", preset, "--target"] + targets, cwd=UI_DIR)

    go_builds = []
    if sel.server:
        go_builds += [(exe("photon-server"), "./server"), (exe("photon-sshd"), "./cmd/photon-sshd")]
    if sel.tools:
        go_builds.append((exe("photon-observer"), "./observer"))
    for name, package in go_builds:
        run_step("go", ["go", "build", "-o", GO_OUT / name, package], cwd=SERVER_DIR)

    if sel.web:
        web.build_web()

    label = commit_label() + ", " + sel.config
    for name, bundle in planned.items():
        install_bundle(name, bundle, tree, label)
    if not state.dry_run:
        staging = DIST / ".staging"
        if staging.exists() and not any(staging.iterdir()):
            staging.rmdir()
        print("built " + format_selection(sel) + " into " + rel(DIST))
