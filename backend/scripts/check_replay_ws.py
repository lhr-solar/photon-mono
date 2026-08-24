#!/usr/bin/env python3
"""hello_ack active_source + log_replay TimelineStatus + DBC TelemetryBatch check."""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import os
import socket
import struct
import subprocess
import sys
import threading
import time
from pathlib import Path

MAGIC = 0x5057
MSG_TELEMETRY = 0x01
MSG_CAN = 0x02
MSG_TIMELINE = 0x05
MODE_BUFFERING = 4


def ws_key() -> str:
    return base64.b64encode(os.urandom(16)).decode("ascii")


def handshake(sock: socket.socket, host: str, port: int) -> None:
    key = ws_key()
    req = (
        f"GET / HTTP/1.1\r\n"
        f"Host: {host}:{port}\r\n"
        f"Upgrade: websocket\r\n"
        f"Connection: Upgrade\r\n"
        f"Sec-WebSocket-Key: {key}\r\n"
        f"Sec-WebSocket-Version: 13\r\n"
        f"\r\n"
    )
    sock.sendall(req.encode("ascii"))
    buf = b""
    while b"\r\n\r\n" not in buf:
        chunk = sock.recv(4096)
        if not chunk:
            raise RuntimeError("handshake closed")
        buf += chunk
    head, _ = buf.split(b"\r\n\r\n", 1)
    if b"101" not in head.split(b"\r\n", 1)[0]:
        raise RuntimeError(f"bad handshake: {head[:120]!r}")
    accept = None
    for line in head.split(b"\r\n"):
        if line.lower().startswith(b"sec-websocket-accept:"):
            accept = line.split(b":", 1)[1].strip()
    expect = base64.b64encode(
        hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode("ascii")).digest()
    )
    if accept != expect:
        raise RuntimeError("accept key mismatch")


def mask_frame(opcode: int, payload: bytes) -> bytes:
    mask = os.urandom(4)
    masked = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
    header = bytes([0x80 | opcode])
    n = len(payload)
    if n < 126:
        header += bytes([0x80 | n])
    elif n < 65536:
        header += bytes([0x80 | 126]) + struct.pack("!H", n)
    else:
        header += bytes([0x80 | 127]) + struct.pack("!Q", n)
    return header + mask + masked


def read_frame(sock: socket.socket) -> tuple[int, bytes]:
    hdr = b""
    while len(hdr) < 2:
        chunk = sock.recv(2 - len(hdr))
        if not chunk:
            raise RuntimeError("eof")
        hdr += chunk
    opcode = hdr[0] & 0x0F
    n = hdr[1] & 0x7F
    if n == 126:
        ext = sock.recv(2)
        n = struct.unpack("!H", ext)[0]
    elif n == 127:
        ext = sock.recv(8)
        n = struct.unpack("!Q", ext)[0]
    data = b""
    while len(data) < n:
        chunk = sock.recv(n - len(data))
        if not chunk:
            raise RuntimeError("eof payload")
        data += chunk
    return opcode, data


def send_json(sock: socket.socket, obj: dict) -> None:
    sock.sendall(mask_frame(0x1, json.dumps(obj).encode("utf-8")))


def wait_port(host: str, port: int, timeout: float) -> None:
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with socket.create_connection((host, port), timeout=0.2):
                return
        except OSError:
            time.sleep(0.05)
    raise RuntimeError(f"backend did not listen on {host}:{port}")


def default_backend() -> Path:
    root = Path(__file__).resolve().parents[2]
    candidates = [
        root / ".artifacts" / "backend" / "photon-backend.exe",
        root / ".artifacts" / "backend" / "photon-backend",
        root / ".artifacts" / "photon-backend.exe",
        root / ".artifacts" / "photon-backend",
    ]
    for candidate in candidates:
        if candidate.exists():
            return candidate
    return candidates[0]


def decode_timeline(payload: bytes) -> dict:
    if len(payload) < 33:
        raise RuntimeError(f"short TimelineStatus {len(payload)}")
    mode, cursor, mn, mx, batches = struct.unpack_from("<BQQQQ", payload, 0)
    return {"mode": mode, "cursor_ms": cursor, "min_ms": mn, "max_ms": mx, "batches": batches}


def decode_telemetry_ids(payload: bytes) -> list[int]:
    ids: list[int] = []
    off = 0
    while off + 8 <= len(payload):
        sig, count, _res = struct.unpack_from("<IHH", payload, off)
        off += 8
        need = count * 16
        if off + need > len(payload):
            break
        off += need
        ids.append(sig)
    return ids


def process_working_set(pid: int) -> int:
    if os.name != "nt":
        return 0
    try:
        import ctypes
        from ctypes import wintypes

        class PROCESS_MEMORY_COUNTERS(ctypes.Structure):
            _fields_ = [
                ("cb", wintypes.DWORD),
                ("PageFaultCount", wintypes.DWORD),
                ("PeakWorkingSetSize", ctypes.c_size_t),
                ("WorkingSetSize", ctypes.c_size_t),
                ("QuotaPeakPagedPoolUsage", ctypes.c_size_t),
                ("QuotaPagedPoolUsage", ctypes.c_size_t),
                ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
                ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
                ("PagefileUsage", ctypes.c_size_t),
                ("PeakPagefileUsage", ctypes.c_size_t),
            ]

        GetCurrentProcess = ctypes.windll.kernel32.OpenProcess
        GetCurrentProcess.restype = wintypes.HANDLE
        PROCESS_QUERY_INFORMATION = 0x0400
        PROCESS_VM_READ = 0x0010
        handle = GetCurrentProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, False, pid)
        if not handle:
            return 0
        counters = PROCESS_MEMORY_COUNTERS()
        counters.cb = ctypes.sizeof(PROCESS_MEMORY_COUNTERS)
        ok = ctypes.windll.psapi.GetProcessMemoryInfo(handle, ctypes.byref(counters), counters.cb)
        ctypes.windll.kernel32.CloseHandle(handle)
        if not ok:
            return 0
        return int(counters.WorkingSetSize)
    except Exception:
        return 0


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=8790)
    p.add_argument("--timeout", type=float, default=180.0)
    p.add_argument("--backend", default="")
    p.add_argument("--log-path", default=r"C:\Users\alper\Downloads\logs\logs")
    p.add_argument("--external", action="store_true", help="do not spawn photon-backend")
    args = p.parse_args()

    proc = None
    logs: list[str] = []
    t0 = time.monotonic()
    if not args.external:
        exe = Path(args.backend) if args.backend else default_backend()
        if not exe.exists():
            print(f"backend not found: {exe}", file=sys.stderr)
            return 2
        if not Path(args.log_path).exists():
            print(f"log path does not exist: {args.log_path}", file=sys.stderr)
            return 2
        proc = subprocess.Popen(
            [
                str(exe),
                "--ws-port",
                str(args.port),
                "--source",
                "log_replay",
                "--log-path",
                args.log_path,
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1,
        )

        def pump() -> None:
            assert proc is not None and proc.stdout is not None
            for line in proc.stdout:
                logs.append(line.rstrip())
                print("backend", line.rstrip(), flush=True)

        threading.Thread(target=pump, daemon=True).start()
        try:
            wait_port(args.host, args.port, min(args.timeout, 15.0))
        except Exception:
            if proc.poll() is None:
                proc.terminate()
            time.sleep(0.2)
            print("\n".join(logs))
            raise

    listen_ms = (time.monotonic() - t0) * 1000.0
    print(f"listen_ms={listen_ms:.1f}")
    ws_bytes = process_working_set(proc.pid) if proc is not None else 0
    if ws_bytes:
        print(f"working_set_after_listen_mb={ws_bytes / (1024 * 1024):.1f}")

    try:
        sock = socket.create_connection((args.host, args.port), timeout=args.timeout)
        try:
            handshake(sock, args.host, args.port)
            send_json(sock, {"type": "hello", "protocol_version": 1, "client": "check_replay_ws"})
            deadline = time.time() + args.timeout
            hello = None
            catalog_ids: list[int] = []
            subscribed = False
            saw_real_tl = False
            first_tl = None
            first_telem = None
            telem_ids: list[int] = []
            while time.time() < deadline:
                sock.settimeout(max(0.1, deadline - time.time()))
                try:
                    opcode, data = read_frame(sock)
                except TimeoutError:
                    continue
                if opcode == 0x1:
                    msg = json.loads(data.decode("utf-8"))
                    kind = msg.get("type")
                    if kind == "hello_ack":
                        hello = msg
                        active = msg.get("active_source") or {}
                        print("hello_ack", json.dumps(msg, sort_keys=True))
                        if active.get("kind") != "log_replay":
                            print("hello_ack.active_source.kind != log_replay", file=sys.stderr)
                            return 1
                        path = active.get("path") or ""
                        if args.log_path.replace("/", "\\") not in path.replace("/", "\\"):
                            print(f"hello_ack path mismatch {path!r}", file=sys.stderr)
                            return 1
                    elif kind == "signal_catalog":
                        signals = msg.get("signals") or []
                        catalog_ids = [int(s["id"]) for s in signals if s.get("id")]
                        print(f"signal_catalog n={len(catalog_ids)}")
                        if catalog_ids and not subscribed:
                            send_json(
                                sock,
                                {
                                    "type": "subscribe_signals",
                                    "signal_ids": catalog_ids[:32],
                                    "protocol_version": 1,
                                },
                            )
                            subscribed = True
                            print("subscribed", catalog_ids[:8], "...")
                    elif kind == "error_alert":
                        print("error_alert", msg, file=sys.stderr)
                        return 2
                    else:
                        print("text", kind)
                elif opcode == 0x2 and len(data) >= 12:
                    magic, ver, mtype, seq, plen = struct.unpack_from("<HBBII", data, 0)
                    if magic != MAGIC:
                        print("bad magic", file=sys.stderr)
                        return 3
                    payload = data[12 : 12 + plen]
                    if mtype == MSG_TIMELINE:
                        tl = decode_timeline(payload)
                        if first_tl is None:
                            first_tl = tl
                            print("timeline_first", tl)
                        if tl["mode"] != MODE_BUFFERING and tl["max_ms"] > tl["min_ms"] and tl["batches"] > 0:
                            if not saw_real_tl:
                                print("timeline_ready", tl, f"since_start_ms={(time.monotonic() - t0) * 1000:.1f}")
                            saw_real_tl = True
                    elif mtype == MSG_TELEMETRY:
                        ids = decode_telemetry_ids(payload)
                        if ids and first_telem is None:
                            first_telem = ids
                            telem_ids = ids
                            print(
                                "telemetry_first",
                                ids[:12],
                                f"since_start_ms={(time.monotonic() - t0) * 1000:.1f}",
                            )
                    elif mtype == MSG_CAN:
                        pass
                    if hello and saw_real_tl and first_telem:
                        break
            if hello is None:
                print("no hello_ack", file=sys.stderr)
                return 1
            if not catalog_ids:
                print("empty signal_catalog", file=sys.stderr)
                return 1
            if not saw_real_tl:
                print("timeline never left buffering / missing min/max", first_tl, file=sys.stderr)
                return 1
            if not first_telem:
                print("no TelemetryBatch", file=sys.stderr)
                return 1
            catalog_set = set(catalog_ids)
            real = [i for i in telem_ids if i in catalog_set]
            if not real:
                print(f"telemetry ids {telem_ids} not in DBC catalog", file=sys.stderr)
                return 1
            peak = process_working_set(proc.pid) if proc is not None else 0
            print(
                f"result hello_ack=1 catalog_n={len(catalog_ids)} timeline_ready=1 "
                f"telemetry_ids={real[:8]} listen_ms={listen_ms:.1f} "
                f"first_telem_ms={(time.monotonic() - t0) * 1000:.1f} "
                f"working_set_mb={peak / (1024 * 1024):.1f}"
            )
            return 0
        finally:
            sock.close()
    finally:
        if proc is not None:
            proc.terminate()
            try:
                proc.wait(timeout=8)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
            leftover = [line for line in logs if "index" in line.lower()]
            if leftover:
                print("--- index lines ---")
                for line in leftover:
                    print(line)

    return 0


if __name__ == "__main__":
    sys.exit(main())
