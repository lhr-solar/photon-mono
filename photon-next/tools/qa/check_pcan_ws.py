#!/usr/bin/env python3
"""Confirm select_source pcan without hardware yields the graceful error_alert."""

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
import time
from pathlib import Path


EXPECTED = "PCANBasic.dll not found or device unavailable"


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


def recv_json_until(sock: socket.socket, deadline: float, predicate) -> dict:
    while time.time() < deadline:
        sock.settimeout(max(0.1, deadline - time.time()))
        try:
            opcode, data = read_frame(sock)
        except TimeoutError:
            continue
        if opcode != 0x1:
            continue
        msg = json.loads(data.decode("utf-8"))
        kind = msg.get("type")
        if kind == "signal_catalog":
            print("text", kind, f"signals={len(msg.get('signals') or [])}")
        else:
            print("text", kind, msg)
        if predicate(msg):
            return msg
    raise RuntimeError("timed out waiting for control message")


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
    # The client bundle from photon build, then a preset build tree.
    out = Path(__file__).resolve().parents[3] / "out"
    candidates = [out / "dist" / "client" / "photon-backend.exe"]
    for preset in ("desktop-debug", "headless-debug", "desktop-release", "headless-release"):
        candidates.append(out / "build" / preset / "photon" / "daemon" / "photon-backend.exe")
    for candidate in candidates:
        if candidate.exists():
            return candidate
    return candidates[0]


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=18765)
    p.add_argument("--timeout", type=float, default=6.0)
    p.add_argument("--backend", default="")
    p.add_argument("--external", action="store_true", help="do not spawn photon-backend")
    args = p.parse_args()

    proc = None
    if not args.external:
        exe = Path(args.backend) if args.backend else default_backend()
        if not exe.exists():
            print(f"backend not found: {exe}", file=sys.stderr)
            return 2
        proc = subprocess.Popen(
            [str(exe), "--ws-port", str(args.port)],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        try:
            wait_port(args.host, args.port, args.timeout)
        except Exception:
            if proc.poll() is None:
                proc.terminate()
            out = proc.communicate(timeout=3)[0] if proc.stdout else ""
            print(out)
            raise

    try:
        sock = socket.create_connection((args.host, args.port), timeout=args.timeout)
        try:
            handshake(sock, args.host, args.port)
            send_json(sock, {"type": "hello", "protocol_version": 1, "client": "check_pcan_ws"})
            recv_json_until(
                sock,
                time.time() + args.timeout,
                lambda m: m.get("type") == "hello_ack",
            )
            send_json(
                sock,
                {
                    "type": "select_source",
                    "source": "pcan",
                    "protocol_version": 1,
                    "channel": 1,
                    "baud": 250000,
                    "listen_only": True,
                },
            )
            err = recv_json_until(
                sock,
                time.time() + args.timeout,
                lambda m: m.get("type") == "error_alert",
            )
            message = err.get("message") or ""
            print("select_source pcan ->", err.get("code"), message)
            if EXPECTED not in message:
                print(f"expected message to contain {EXPECTED!r}", file=sys.stderr)
                return 1
            return 0
        finally:
            sock.close()
    finally:
        if proc is not None:
            proc.terminate()
            try:
                out, _ = proc.communicate(timeout=3)
            except subprocess.TimeoutExpired:
                proc.kill()
                out, _ = proc.communicate()
            if out:
                print("--- backend ---")
                print(out)

    return 0


if __name__ == "__main__":
    sys.exit(main())
