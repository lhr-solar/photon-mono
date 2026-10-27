"""photon server, photon station, and photon client: run a built bundle from out/dist."""

from __future__ import annotations

import os
import re
import subprocess
import sys
from pathlib import Path

from photon.common import DIST, ROOT, exe, fail, rel, show_command, state


def password() -> str:
    secret = os.environ.get("PHOTON_PASSWORD", "").strip()
    return secret or "laptop"


def data_dir() -> Path:
    folder = os.environ.get("PHOTON_DATA_DIR", "").strip()
    return Path(folder) if folder else ROOT / "data"


def in_bundle(bundle: str, *parts: str):
    path = DIST.joinpath(bundle, *parts)
    return path.resolve() if path.exists() else None


def split_host_port(spec: str, default_port: int):
    match = re.match(r"^(?P<host>.+):(?P<port>\d+)$", spec)
    if match:
        return match.group("host"), int(match.group("port"))
    return spec, default_port


def run_logged(program: Path, cwd, args) -> None:
    """Run a bundle's program in the foreground; its exit code is photon's."""
    cmd = [str(program)] + [str(a) for a in args]
    if state.dry_run:
        show_command("run", cmd, cwd)
        return
    proc = subprocess.Popen(cmd, cwd=cwd)
    try:
        code = proc.wait()
    except KeyboardInterrupt:
        # Ctrl+C reached the program too; let it shut down on its own.
        code = proc.wait()
    if code != 0:
        sys.exit(code)


def server(words, with_ui: bool) -> None:
    pw = password()
    data = data_dir()
    # A station is the server with the local UI beside it; it has its own bundle.
    bundle = "station" if with_ui else "server"
    folder = DIST / bundle
    program = in_bundle(bundle, exe("photon-server"))
    ingress = None
    if words and words[0].lower() == "ingest":
        if len(words) < 2 or not words[1].isdigit():
            fail("photon server ingest needs a port", 2)
        ingress = words[1]
        words = words[2:]
    args = ["-data-dir", data, "-password", pw]
    if ingress:
        args += ["-ingress", ingress]
    if not with_ui:
        args += ["-ui", "off"]
    args += words
    if not state.dry_run:
        data.mkdir(parents=True, exist_ok=True)
    print("ingest " + (ingress or "6100") + "  password " + pw + "  data " + str(data))
    if program is None:
        if state.dry_run:
            show_command("run", [folder / exe("photon-server")] + args, folder)
            return
        fail(rel(folder) + " is not built. Run: photon build " + bundle)
    run_logged(program, folder, args)


def start_ui(source: str, words, ui=None, daq=None, video=None, video_udp=None, log_path=None) -> None:
    folder = DIST / "client"
    backend = in_bundle("client", exe("photon-backend"))
    if backend is None:
        if not state.dry_run:
            fail(rel(folder) + " is not built. Run: photon build client")
        backend = folder / exe("photon-backend")
    pw = password()
    args = ["--no-live-tail", "--source", source, "--ws-port", "8765", "--password", pw,
            "--assets-dir", folder / "assets"]
    if source == "daq_tcp":
        args += ["--daq-host", daq[0], "--daq-port", daq[1], "--video-host", video[0], "--video-port", video[1],
                 "--video-udp-port", video_udp[1], "--video-udp-host", video_udp[0]]
    elif source == "log_replay":
        args += ["--log-path", log_path]
    args += words

    native = in_bundle("client", exe("photon-ui"))
    page = in_bundle("client", "ui", "photon.html")
    if ui == "desktop":
        if native is None and not state.dry_run:
            fail(rel(folder) + " has no " + exe("photon-ui") + ". Run: photon build client --ui desktop")
        page = None
    elif ui == "web":
        if page is None and not state.dry_run:
            fail(rel(folder) + " has no page. Run: photon build client --ui web")
        native = None
    print("password " + pw)
    if source == "daq_tcp":
        print("daq %s:%s  video %s:%s  udp %s:%s" % (daq + video + video_udp))
    page_server = [sys.executable, "-m", "http.server", "8080", "--bind", "127.0.0.1", "--directory"]
    if state.dry_run:
        show_command("backend", [backend] + args)
        if ui == "desktop" or (native and ui != "web"):
            show_command("ui", [folder / exe("photon-ui")])
        elif page or ui == "web":
            show_command("page", page_server + [folder / "ui"])
        else:
            print("ui: not built (run photon build client, or photon build client --ui web for the page)")
        return

    children = []
    if native:
        children.append(subprocess.Popen([str(native)], cwd=native.parent))
        print("ui " + str(native))
    elif page:
        children.append(subprocess.Popen(page_server + [str(page.parent)], stdout=subprocess.DEVNULL,
                                         stderr=subprocess.DEVNULL))
        print("page http://127.0.0.1:8080/photon.html")
    else:
        print("ui binary and photon.html were not built; the daemon will still listen on ws://127.0.0.1:8765")
    try:
        # From the caller's folder, so relative paths in passed-through flags mean what they say.
        run_logged(backend, None, args)
    finally:
        for child in children:
            if child.poll() is None:
                child.terminate()


def client(words) -> None:
    """photon client [FROM] [--ui desktop|web] [backend flags]. FROM is read the way a person
    would say it: nothing, oracle, pcan, a log on disk, or a host."""
    ui = None
    rest = []
    i = 0
    while i < len(words):
        if words[i].lower() in ("--ui", "-ui"):
            if i + 1 >= len(words) or words[i + 1].lower() not in ("desktop", "web"):
                fail("--ui is desktop or web", 2)
            ui = words[i + 1].lower()
            i += 2
            continue
        rest.append(words[i])
        i += 1
    source = rest.pop(0) if rest and not rest[0].startswith("-") else None
    key = (source or "").lower()
    if key == "":
        print("from this machine")
        start_ui("daq_tcp", rest, ui, ("127.0.0.1", 6500), ("127.0.0.1", 6700), ("127.0.0.1", 6800))
    elif key == "oracle":
        print("from the Oracle pipe")
        start_ui("daq_tcp", rest, ui, ("150.136.85.207", 6110), ("150.136.85.207", 6700), ("10.66.0.1", 6800))
    elif key == "pcan":
        print("from a USB PCAN")
        start_ui("pcan", rest, ui)
    elif Path(source).exists():
        print("replaying " + source)
        start_ui("log_replay", rest, ui, log_path=Path(source).resolve())
    elif re.search(r"[\\/]", source) or re.search(r"\.(log|zip)$", key):
        fail("no log or folder at " + source, 2)
    else:
        host, port = split_host_port(source, 6500)
        print("from %s:%d" % (host, port))
        start_ui("daq_tcp", rest, ui, (host, port), (host, 6700), (host, 6800))
