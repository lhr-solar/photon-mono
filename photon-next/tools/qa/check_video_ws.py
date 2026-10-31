#!/usr/bin/env python3
"""Synthetic photon-server video listener + WS client check for 0x04 VideoFrame."""

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
MSG_VIDEO_FRAME = 0x04


def annexb(nal_type: int, body: bytes) -> bytes:
    return b"\x00\x00\x00\x01" + bytes([nal_type]) + body


def encode_server_frame(ts_ms: int, flags: int, payload: bytes) -> bytes:
    return struct.pack("!IQB", len(payload), ts_ms, flags) + payload


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
    raise RuntimeError(f"did not listen on {host}:{port}")


def default_backend() -> Path:
    # The client bundle from photon build, then a preset build tree.
    out = Path(__file__).resolve().parents[3] / "out"
    name = "photon-backend.exe" if os.name == "nt" else "photon-backend"
    candidates = [out / "dist" / "client" / name]
    for preset in ("desktop-debug", "headless-debug", "desktop-release", "headless-release"):
        candidates.append(out / "build" / preset / "photon" / "daemon" / name)
    for candidate in candidates:
        if candidate.exists():
            return candidate
    return candidates[0]


class FakeVideoServer:
    def __init__(self, host: str, port: int) -> None:
        self.host = host
        self.port = port
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None
        self.got_subscribe = threading.Event()
        self.subscribe_mode: int | None = None
        self.subscribe_start = 0
        self.served = 0
        self.error: str | None = None

        self.frames = [
            (1000, 0x02, annexb(0x67, bytes.fromhex("42C01E"))),
            (1000, 0x02, annexb(0x68, bytes.fromhex("CE3C80"))),
            (1033, 0x01, annexb(0x65, b"\x88\x80\x20\x00")),
            (1066, 0x00, annexb(0x41, b"\x9a\x00")),
            (1099, 0x00, annexb(0x41, b"\x9b\x00")),
        ]

    def start(self) -> None:
        self._ls = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._ls.bind((self.host, self.port))
        self._ls.listen(4)
        self._ls.settimeout(0.2)
        self.port = self._ls.getsockname()[1]
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        if self._thread:
            self._thread.join(timeout=2)
        try:
            self._ls.close()
        except OSError:
            pass

    def _run(self) -> None:
        while not self._stop.is_set():
            try:
                conn, _ = self._ls.accept()
            except TimeoutError:
                continue
            except OSError:
                return
            try:
                self._serve(conn)
            except Exception as exc:  # noqa: BLE001
                self.error = str(exc)
            finally:
                try:
                    conn.close()
                except OSError:
                    pass

    def _serve(self, conn: socket.socket) -> None:
        conn.settimeout(2.0)
        hdr = b""
        while len(hdr) < 9:
            chunk = conn.recv(9 - len(hdr))
            if not chunk:
                raise RuntimeError("client closed before subscribe header")
            hdr += chunk
        self.subscribe_mode = hdr[0]
        self.subscribe_start = struct.unpack("!Q", hdr[1:9])[0]
        self.got_subscribe.set()
        for ts, flags, payload in self.frames:
            conn.sendall(encode_server_frame(ts, flags, payload))
            self.served += 1
            time.sleep(0.02)
        # Hold the connection until stop so the backend does not immediately reconnect.
        while not self._stop.is_set():
            time.sleep(0.05)


def decode_photon_frame(blob: bytes) -> tuple[int, bytes]:
    if len(blob) < 12:
        raise RuntimeError(f"short photon frame {len(blob)}")
    magic, version, msg_type, _seq, payload_len = struct.unpack_from("<HBBII", blob, 0)
    if magic != MAGIC or version != 1:
        raise RuntimeError(f"bad photon header magic={magic:#x} ver={version}")
    if len(blob) < 12 + payload_len:
        raise RuntimeError("truncated photon payload")
    return msg_type, blob[12 : 12 + payload_len]


def decode_video_payload(payload: bytes) -> tuple[int, int, bytes]:
    if len(payload) < 12:
        raise RuntimeError("short VideoFrame payload")
    ts = struct.unpack_from("<Q", payload, 0)[0]
    flags = payload[8]
    nal = payload[12:]
    return ts, flags, nal


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--ws-port", type=int, default=18766)
    p.add_argument("--video-port", type=int, default=6700)
    p.add_argument("--timeout", type=float, default=8.0)
    p.add_argument("--backend", default="")
    p.add_argument("--external", action="store_true")
    args = p.parse_args()

    video = FakeVideoServer(args.host, args.video_port)
    try:
        video.start()
    except OSError as exc:
        if args.video_port == 6700:
            print(f"port 6700 busy ({exc}); falling back to ephemeral")
            video = FakeVideoServer(args.host, 0)
            video.start()
        else:
            raise
    print(f"fake video listening on {args.host}:{video.port}")

    proc = None
    try:
        if not args.external:
            exe = Path(args.backend) if args.backend else default_backend()
            if not exe.exists():
                print(f"backend not found: {exe}", file=sys.stderr)
                return 2
            proc = subprocess.Popen(
                [
                    str(exe),
                    "--ws-port",
                    str(args.ws_port),
                    "--video-host",
                    args.host,
                    "--video-port",
                    str(video.port),
                ],
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
            )
            wait_port(args.host, args.ws_port, args.timeout)

        sock = socket.create_connection((args.host, args.ws_port), timeout=args.timeout)
        try:
            handshake(sock, args.host, args.ws_port)
            send_json(sock, {"type": "hello", "protocol_version": 1, "client": "check_video_ws"})
            send_json(sock, {"type": "video_subscribe", "protocol_version": 1, "mode": "live"})

            deadline = time.time() + args.timeout
            saw_connected = False
            frames: list[tuple[int, int, bytes]] = []
            while time.time() < deadline and (len(frames) < 5 or not saw_connected):
                sock.settimeout(max(0.1, deadline - time.time()))
                try:
                    opcode, data = read_frame(sock)
                except TimeoutError:
                    continue
                if opcode == 0x1:
                    msg = json.loads(data.decode("utf-8"))
                    kind = msg.get("type")
                    print("text", kind, msg)
                    if kind == "video_status" and msg.get("connected") is True:
                        saw_connected = True
                    if kind == "error_alert":
                        print("error_alert", msg, file=sys.stderr)
                        return 1
                elif opcode == 0x2:
                    msg_type, payload = decode_photon_frame(data)
                    if msg_type != MSG_VIDEO_FRAME:
                        print(f"binary msgType={msg_type:#x} ignored")
                        continue
                    ts, flags, nal = decode_video_payload(payload)
                    frames.append((ts, flags, nal))
                    print(f"video_frame ts={ts} flags={flags:#04x} nal={nal[:8].hex()} len={len(nal)}")

            if not video.got_subscribe.wait(args.timeout):
                print("fake server never received subscribe header", file=sys.stderr)
                return 1
            print(f"server subscribe mode={video.subscribe_mode} start={video.subscribe_start}")

            if not saw_connected:
                print("did not receive video_status connected=true", file=sys.stderr)
                return 1
            if len(frames) < 5:
                print(f"expected 5 VideoFrames, got {len(frames)}", file=sys.stderr)
                return 1

            expect = [
                (1000, 0x02),
                (1000, 0x02),
                (1033, 0x01),
                (1066, 0x00),
                (1099, 0x00),
            ]
            for i, (exp_ts, exp_flags) in enumerate(expect):
                ts, flags, nal = frames[i]
                if ts != exp_ts or flags != exp_flags:
                    print(f"frame {i}: expected ts={exp_ts} flags={exp_flags:#04x}, got ts={ts} flags={flags:#04x}",
                          file=sys.stderr)
                    return 1
                if nal[:4] != b"\x00\x00\x00\x01":
                    print(f"frame {i}: missing Annex-B start code", file=sys.stderr)
                    return 1

            print("check_video_ws: ok")
            return 0
        finally:
            sock.close()
    finally:
        video.stop()
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
