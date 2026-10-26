"""Shared pieces for the end-to-end tests: paths, processes, sockets, and the Photon wire.

Standard library only, so CI and a fresh laptop need nothing beyond Python.

PHOTON_BUILD_DIR   the build to test (default <repo>/build)
PHOTON_E2E_LOGS    where process logs go (default a temp folder, printed at start)
PHOTON_E2E_STRICT  1 turns "this piece was not built" skips into failures, as CI wants
"""

from __future__ import annotations

import base64
import hashlib
import json
import os
import shutil
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time
import unittest
import urllib.error
import urllib.request
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
BUILD = Path(os.environ.get("PHOTON_BUILD_DIR") or REPO / "build").resolve()
EXAMPLE_DIR = REPO / "examples" / "brainerd-30s"
EXAMPLE_LOG = EXAMPLE_DIR / "2026-07-23-03-30s.log"
STRICT = os.environ.get("PHOTON_E2E_STRICT", "") not in ("", "0")
WINDOWS = os.name == "nt"

_logs = os.environ.get("PHOTON_E2E_LOGS")
LOGS = Path(_logs) if _logs else Path(tempfile.mkdtemp(prefix="photon-e2e-logs-"))
LOGS.mkdir(parents=True, exist_ok=True)


def exe(name: str) -> Path:
    return BUILD / (name + (".exe" if WINDOWS else ""))


def require(*paths: Path) -> None:
    """Skip when a piece was not built; fail instead under PHOTON_E2E_STRICT."""
    missing = [str(p) for p in paths if not p.exists()]
    if missing:
        message = "not built: " + ", ".join(missing)
        if STRICT:
            raise AssertionError(message)
        raise unittest.SkipTest(message)


def free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def port_open(port: int, host: str = "127.0.0.1") -> bool:
    try:
        with socket.create_connection((host, port), timeout=0.2):
            return True
    except OSError:
        return False


def wait_until(check, timeout: float, what: str, step: float = 0.1):
    deadline = time.monotonic() + timeout
    while True:
        value = check()
        if value:
            return value
        if time.monotonic() >= deadline:
            raise AssertionError(f"timed out after {timeout:.0f} s waiting for {what}")
        time.sleep(step)


# --- processes ----------------------------------------------------------------


class Proc:
    """A child process whose output goes to a log file under LOGS."""

    def __init__(self, name: str, args: list, cwd: Path | None = None, env: dict | None = None):
        self.name = name
        self.log_path = LOGS / f"{name}.log"
        self._log = open(self.log_path, "w", encoding="utf-8", errors="replace")
        flags = subprocess.CREATE_NEW_PROCESS_GROUP if WINDOWS else 0
        self.p = subprocess.Popen(
            [str(a) for a in args],
            cwd=str(cwd) if cwd else None,
            env={**os.environ, **(env or {})},
            stdout=self._log,
            stderr=subprocess.STDOUT,
            stdin=subprocess.DEVNULL,
            creationflags=flags,
            start_new_session=not WINDOWS,
        )

    def output(self) -> str:
        if not self._log.closed:
            self._log.flush()
        return self.log_path.read_text(encoding="utf-8", errors="replace")

    def wait_for_output(self, text: str, timeout: float) -> None:
        def seen():
            if text in self.output():
                return True
            if self.p.poll() is not None:
                raise AssertionError(f"{self.name} exited with {self.p.returncode}:\n{self.tail()}")
            return False

        wait_until(seen, timeout, f"{self.name} to print {text!r}")

    def wait_for_port(self, port: int, timeout: float) -> None:
        def listening():
            if self.p.poll() is not None:
                raise AssertionError(f"{self.name} exited with {self.p.returncode}:\n{self.tail()}")
            return port_open(port)

        wait_until(listening, timeout, f"{self.name} to listen on {port}")

    def wait(self, timeout: float) -> int:
        """Wait for a run-to-completion tool; a hang fails with its output instead of blocking CI."""
        try:
            self.p.wait(timeout)
        except subprocess.TimeoutExpired:
            self.kill()
            raise AssertionError(f"{self.name} did not finish in {timeout:.0f} s:\n{self.tail()}")
        self._log.close()
        return self.p.returncode

    def tail(self, lines: int = 60) -> str:
        return "\n".join(self.output().splitlines()[-lines:])

    def stop(self, timeout: float = 20.0) -> int | None:
        """Interrupt the way Ctrl+C would, then kill the whole tree if it does not exit."""
        if self.p.poll() is None:
            try:
                if WINDOWS:
                    os.kill(self.p.pid, signal.CTRL_BREAK_EVENT)
                else:
                    os.killpg(self.p.pid, signal.SIGINT)
            except OSError:
                pass
            try:
                self.p.wait(timeout)
            except subprocess.TimeoutExpired:
                self.kill()
        self._log.close()
        return self.p.returncode

    def kill(self) -> None:
        if self.p.poll() is None:
            if WINDOWS:
                subprocess.run(["taskkill", "/T", "/F", "/PID", str(self.p.pid)], capture_output=True)
            else:
                os.killpg(self.p.pid, signal.SIGKILL)
            self.p.wait(10)
        self._log.close()


# --- recorded data ---------------------------------------------------------------


def vsd1_payloads(data: bytes | Path = EXAMPLE_LOG) -> list[bytes]:
    """The payloads of a VSD1 log, in order (24-byte big-endian frame header).

    photon-server stores what each ingress read returned, so a payload is a run of
    CANP bytes and a batch can straddle two payloads; join them before parsing.
    """
    if isinstance(data, Path):
        data = data.read_bytes()
    out, off = [], 0
    while off + 24 <= len(data):
        magic, n = struct.unpack_from(">II", data, off)
        assert magic == 0x56534431, f"no VSD1 frame at byte {off}"
        out.append(data[off + 24 : off + 24 + n])
        off += 24 + n
    return out


CANP_HEADER = 20
CANP_PACKET = 29


def canp_batches(stream: bytes) -> list[tuple[int, list[int]]]:
    """(timestamp ms, CAN ids) per complete CANP v3 batch in a byte stream (big-endian wire).

    Like photon's StreamParser, a header that is not a whole valid batch is skipped and
    parsing resumes at the next magic. Recorded logs hold batches cut short at the DAQ.
    """
    out, off = [], stream.find(b"CAN1")
    while 0 <= off and off + CANP_HEADER <= len(stream):
        _magic, version, count, _seq, ts = struct.unpack_from(">IHHIQ", stream, off)
        end = off + CANP_HEADER + count * CANP_PACKET
        if version != 3 or not 0 < count <= 64 or end > len(stream):
            off = stream.find(b"CAN1", off + 1)
            continue
        ids = [struct.unpack_from(">I", stream, off + CANP_HEADER + i * CANP_PACKET)[0] for i in range(count)]
        out.append((ts, ids))
        off = end
    return out


def clip_batches(payloads: list[bytes] | None = None) -> list[tuple[int, list[int]]]:
    """The batches photon-backend indexes from a log: each VSD1 frame parsed on its own."""
    return [b for p in (payloads if payloads is not None else vsd1_payloads()) for b in canp_batches(p)]


def feed(port: int, payloads: list[bytes], pace_s: float = 0.002) -> None:
    """Play CANP batches into photon-server ingress the way the DAQ does."""
    with socket.create_connection(("127.0.0.1", port), timeout=10) as sock:
        for payload in payloads:
            sock.sendall(payload)
            if pace_s:
                time.sleep(pace_s)
        time.sleep(0.5)


def client_auth(password: str) -> bytes:
    """The PHAU preamble photon-server reads before fan-out, observer, and video clients."""
    raw = password.encode("utf-8")
    return b"PHAU" + bytes([len(raw)]) + raw


def http_get(url: str, password: str | None = None, timeout: float = 10.0) -> tuple[int, bytes]:
    req = urllib.request.Request(url)
    if password is not None:
        token = base64.b64encode(f"photon:{password}".encode()).decode()
        req.add_header("Authorization", "Basic " + token)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return resp.status, resp.read()
    except urllib.error.HTTPError as err:
        with err:
            return err.code, err.read()


def copy_ship(dest: Path, names: list[str], dirs: list[str]) -> Path:
    """Copy the named build outputs into dest, so a test owns its exe-relative logs and data."""
    dest.mkdir(parents=True, exist_ok=True)
    for name in names:
        shutil.copy2(BUILD / name, dest / name)
    for d in dirs:
        shutil.copytree(BUILD / d, dest / d, dirs_exist_ok=True)
    return dest


# --- WebSocket ------------------------------------------------------------------


class WebSocket:
    """A minimal RFC 6455 client: enough for photon-backend and the browser's DevTools."""

    GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

    def __init__(self, host: str, port: int, path: str = "/", headers: dict | None = None, timeout: float = 10.0):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        key = base64.b64encode(os.urandom(16)).decode()
        lines = [
            f"GET {path} HTTP/1.1",
            f"Host: {host}:{port}",
            "Upgrade: websocket",
            "Connection: Upgrade",
            f"Sec-WebSocket-Key: {key}",
            "Sec-WebSocket-Version: 13",
        ]
        lines += [f"{k}: {v}" for k, v in (headers or {}).items()]
        self.sock.sendall(("\r\n".join(lines) + "\r\n\r\n").encode())
        buf = b""
        while b"\r\n\r\n" not in buf:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise ConnectionError("websocket handshake closed")
            buf += chunk
        head, rest = buf.split(b"\r\n\r\n", 1)
        status = head.split(b"\r\n", 1)[0]
        if status.split(b" ")[1:2] != [b"101"]:
            raise ConnectionError(f"websocket handshake refused: {status!r}")
        want = base64.b64encode(hashlib.sha1((key + self.GUID).encode()).digest())
        accept = [l.split(b":", 1)[1].strip() for l in head.split(b"\r\n") if l.lower().startswith(b"sec-websocket-accept:")]
        if accept != [want]:
            raise ConnectionError("websocket accept key mismatch")
        self.pending = bytearray(rest)
        self.fragments: bytearray | None = None
        self.fragment_opcode = 0

    def send(self, opcode: int, payload: bytes) -> None:
        mask = os.urandom(4)
        n = len(payload)
        header = bytes([0x80 | opcode])
        if n < 126:
            header += bytes([0x80 | n])
        elif n < 65536:
            header += bytes([0x80 | 126]) + struct.pack("!H", n)
        else:
            header += bytes([0x80 | 127]) + struct.pack("!Q", n)
        body = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
        self.sock.sendall(header + mask + body)

    def send_text(self, text: str) -> None:
        self.send(0x1, text.encode("utf-8"))

    def _parse(self):
        b = self.pending
        if len(b) < 2:
            return None
        fin, opcode, n, off = b[0] & 0x80, b[0] & 0x0F, b[1] & 0x7F, 2
        if n == 126:
            if len(b) < 4:
                return None
            n, off = struct.unpack_from("!H", b, 2)[0], 4
        elif n == 127:
            if len(b) < 10:
                return None
            n, off = struct.unpack_from("!Q", b, 2)[0], 10
        mask = None
        if b[1] & 0x80:
            if len(b) < off + 4:
                return None
            mask, off = bytes(b[off : off + 4]), off + 4
        if len(b) < off + n:
            return None
        data = bytes(b[off : off + n])
        del b[: off + n]
        if mask:
            data = bytes(x ^ mask[i % 4] for i, x in enumerate(data))
        return bool(fin), opcode, data

    def recv(self, timeout: float):
        """The next data frame as (opcode, bytes), or None if none arrives in time."""
        deadline = time.monotonic() + timeout
        while True:
            frame = self._parse()
            if frame:
                fin, opcode, data = frame
                if opcode == 0x9:
                    self.send(0xA, data)
                elif opcode == 0x8:
                    raise ConnectionError("websocket closed by peer")
                elif opcode == 0x0 and self.fragments is not None:
                    self.fragments += data
                    if fin:
                        whole, self.fragments = bytes(self.fragments), None
                        return self.fragment_opcode, whole
                elif opcode in (0x1, 0x2):
                    if fin:
                        return opcode, data
                    self.fragments, self.fragment_opcode = bytearray(data), opcode
                continue
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return None
            self.sock.settimeout(remaining)
            try:
                chunk = self.sock.recv(65536)
            except (socket.timeout, TimeoutError):
                return None
            if not chunk:
                raise ConnectionError("websocket closed")
            self.pending += chunk

    def close(self) -> None:
        try:
            self.send(0x8, b"")
        except OSError:
            pass
        self.sock.close()


# --- photon-backend protocol v1 ---------------------------------------------------

PHOTON_MAGIC = 0x5057
MSG_TELEMETRY = 0x01
MSG_CAN = 0x02
MSG_TIMELINE = 0x05
MODE_PAUSE, MODE_PLAY, MODE_LIVE, MODE_BUFFERING = 1, 2, 3, 4


class PhotonClient:
    """A UI stand-in: says hello, then keeps what the backend sends."""

    def __init__(self, port: int, client: str = "photon-e2e", host: str = "127.0.0.1"):
        self.ws = WebSocket(host, port)
        self.messages: list[dict] = []
        self.timelines: list[dict] = []
        self.telemetry: dict[int, int] = {}
        self.can_batches = 0
        self.send({"type": "hello", "protocol_version": 1, "client": client})

    def send(self, obj: dict) -> None:
        self.ws.send_text(json.dumps({"protocol_version": 1, **obj}))

    def pump(self, timeout: float = 0.2) -> None:
        frame = self.ws.recv(timeout)
        if frame is None:
            return
        opcode, data = frame
        if opcode == 0x1:
            self.messages.append(json.loads(data.decode("utf-8")))
            return
        magic, _ver, mtype, _seq, plen = struct.unpack_from("<HBBII", data, 0)
        assert magic == PHOTON_MAGIC, f"bad binary magic {magic:#x}"
        payload = data[12 : 12 + plen]
        if mtype == MSG_TIMELINE:
            mode, cursor, lo, hi, batches = struct.unpack_from("<BQQQQ", payload, 0)
            self.timelines.append({"mode": mode, "cursor_ms": cursor, "min_ms": lo, "max_ms": hi, "batches": batches})
        elif mtype == MSG_TELEMETRY:
            off = 0
            while off + 8 <= len(payload):
                sig, count, _ = struct.unpack_from("<IHH", payload, off)
                off += 8 + count * 16
                self.telemetry[sig] = self.telemetry.get(sig, 0) + count
        elif mtype == MSG_CAN:
            self.can_batches += 1

    def wait(self, check, timeout: float, what: str):
        deadline = time.monotonic() + timeout
        while True:
            value = check()
            if value:
                return value
            if time.monotonic() >= deadline:
                kinds = sorted({m.get("type", "?") for m in self.messages})
                raise AssertionError(f"timed out after {timeout:.0f} s waiting for {what}; saw {kinds}")
            self.pump(0.2)

    def message(self, kind: str) -> dict | None:
        for m in reversed(self.messages):
            if m.get("type") == kind:
                return m
        return None

    def wait_message(self, kind: str, timeout: float = 20.0, after: int = 0) -> dict:
        def found():
            for m in self.messages[after:]:
                if m.get("type") == kind:
                    return m
            return None

        return self.wait(found, timeout, kind)

    def close(self) -> None:
        self.ws.close()


# --- a headless browser over the DevTools protocol --------------------------------


def find_browser() -> str | None:
    env = os.environ.get("PHOTON_BROWSER")
    if env:
        return env
    candidates = []
    if WINDOWS:
        for root in (os.environ.get("ProgramFiles(x86)"), os.environ.get("ProgramFiles"), os.environ.get("LOCALAPPDATA")):
            if root:
                candidates += [
                    Path(root) / "Microsoft" / "Edge" / "Application" / "msedge.exe",
                    Path(root) / "Google" / "Chrome" / "Application" / "chrome.exe",
                ]
        for c in candidates:
            if c.exists():
                return str(c)
        return None
    for name in ("google-chrome", "chromium", "chromium-browser", "microsoft-edge"):
        found = shutil.which(name)
        if found:
            return found
    return None


class Browser:
    """Headless Chromium or Edge with software WebGL, driven over DevTools."""

    def __init__(self, path: str, profile: Path):
        self.port = free_port()
        self.proc = Proc(
            "browser",
            [
                path,
                "--headless=new",
                "--use-angle=swiftshader",
                "--enable-unsafe-swiftshader",
                "--no-first-run",
                "--no-default-browser-check",
                f"--user-data-dir={profile}",
                f"--remote-debugging-port={self.port}",
                "--window-size=1600,900",
                "about:blank",
            ],
        )

        def page_target():
            try:
                with urllib.request.urlopen(f"http://127.0.0.1:{self.port}/json", timeout=1) as resp:
                    targets = json.load(resp)
            except OSError:
                return None
            return next((t for t in targets if t.get("type") == "page"), None)

        target = wait_until(page_target, 30, "the browser's DevTools page")
        url = target["webSocketDebuggerUrl"]
        hostport, path = url.split("://", 1)[1].split("/", 1)
        self.ws = WebSocket("127.0.0.1", int(hostport.rsplit(":", 1)[1]), "/" + path)
        self.next_id = 0
        self.events: list[dict] = []

    def call(self, method: str, params: dict | None = None, timeout: float = 30.0) -> dict:
        self.next_id += 1
        my_id = self.next_id
        self.ws.send_text(json.dumps({"id": my_id, "method": method, "params": params or {}}))
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            frame = self.ws.recv(0.5)
            if frame is None:
                continue
            msg = json.loads(frame[1].decode("utf-8"))
            if msg.get("id") == my_id:
                if "error" in msg:
                    raise AssertionError(f"{method}: {msg['error']}")
                return msg.get("result", {})
            if "method" in msg:
                self.events.append(msg)
        raise AssertionError(f"{method} got no reply")

    def pump(self, seconds: float) -> None:
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            frame = self.ws.recv(max(0.05, deadline - time.monotonic()))
            if frame is not None:
                msg = json.loads(frame[1].decode("utf-8"))
                if "method" in msg:
                    self.events.append(msg)

    def exceptions(self) -> list[str]:
        out = []
        for e in self.events:
            if e["method"] == "Runtime.exceptionThrown":
                d = e["params"]["exceptionDetails"]
                out.append((d.get("exception") or {}).get("description") or d.get("text", "?"))
        return out

    def close(self) -> None:
        try:
            self.call("Browser.close", timeout=5)
        except (AssertionError, OSError):
            pass
        self.ws.sock.close()
        self.proc.stop(timeout=10)


def log(message: str) -> None:
    print(message, file=sys.stderr, flush=True)
