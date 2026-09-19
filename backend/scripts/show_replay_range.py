"""Replay a bounded video window from photon-server :6700. No live. No HTML.

Default window is the Sep 12 2026 3:29–3:30 PM America/Chicago clip from the UI.
Usage:
  python show_replay_range.py
  python show_replay_range.py --start-ms 1789244940000 --end-ms 1789245000000
"""
from __future__ import annotations

import argparse
import hashlib
import socket
import struct
import sys
import tempfile
import time
from datetime import datetime
from zoneinfo import ZoneInfo

PASSWORD = b"laptop"
TZ = ZoneInfo("America/Chicago")


def recvn(sock: socket.socket, n: int) -> bytes:
    buf = bytearray()
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise EOFError("eof")
        buf.extend(chunk)
    return bytes(buf)


def auth_blob() -> bytes:
    return b"PHAU" + bytes([len(PASSWORD)]) + PASSWORD


def replay_range_req(start_ms: int, end_ms: int) -> bytes:
    return bytes([2]) + struct.pack(">Q", start_ms) + struct.pack(">Q", end_ms)


def locate_req(at_ms: int, prefetch: int) -> bytes:
    return bytes([3]) + struct.pack(">Q", at_ms) + bytes([0]) + struct.pack(">H", prefetch)


def fmt_ms(ms: int) -> str:
    return datetime.fromtimestamp(ms / 1000.0, TZ).strftime("%Y-%m-%d %I:%M:%S %p %Z")


def read_frames(sock: socket.socket) -> list[tuple[int, int, bytes]]:
    frames: list[tuple[int, int, bytes]] = []
    sock.settimeout(20)
    while True:
        try:
            hdr = recvn(sock, 13)
        except (EOFError, TimeoutError, socket.timeout):
            break
        n, ts, flags = struct.unpack(">IQB", hdr)
        if n > 16 * 1024 * 1024:
            raise RuntimeError(f"bad payloadLen {n}")
        payload = recvn(sock, n) if n else b""
        frames.append((ts, flags, payload))
    return frames


def decode_h264(h264: bytes) -> list:
    import os

    import cv2

    fd, path = tempfile.mkstemp(suffix=".h264")
    os.close(fd)
    try:
        with open(path, "wb") as f:
            f.write(h264)
        cap = cv2.VideoCapture(path, cv2.CAP_FFMPEG)
        frames = []
        if cap.isOpened():
            while True:
                ok, im = cap.read()
                if not ok:
                    break
                frames.append(im)
            cap.release()
        return frames
    finally:
        try:
            os.remove(path)
        except OSError:
            pass


def main() -> int:
    p = argparse.ArgumentParser(description="Replay one recorded minute without photon.html")
    p.add_argument("--host", default="150.136.85.207")
    p.add_argument("--port", type=int, default=6700)
    p.add_argument("--start-ms", type=int, default=1789244940000)
    p.add_argument("--end-ms", type=int, default=1789245000000)
    p.add_argument("--no-gui", action="store_true")
    args = p.parse_args()

    print(f"replay {fmt_ms(args.start_ms)} -> {fmt_ms(args.end_ms)}", flush=True)
    sock = socket.create_connection((args.host, args.port), timeout=8)
    sock.sendall(auth_blob())
    sock.sendall(locate_req(args.start_ms, 2))
    ln = struct.unpack(">I", recvn(sock, 4))[0]
    body = recvn(sock, ln) if ln else b""
    status = body[0] if body else 255
    print(f"locate status={status} reply_bytes={len(body)}", flush=True)

    sock.close()
    sock = socket.create_connection((args.host, args.port), timeout=8)
    sock.sendall(auth_blob())
    sock.sendall(replay_range_req(args.start_ms, args.end_ms))
    t0 = time.perf_counter()
    nals = read_frames(sock)
    sock.close()
    dt = 1000 * (time.perf_counter() - t0)
    if not nals:
        print("no NALs in window", flush=True)
        return 2
    keys = sum(1 for _, fl, _ in nals if fl & 1)
    cfgs = sum(1 for _, fl, _ in nals if fl & 2)
    print(
        f"nals={len(nals)} keys={keys} cfg={cfgs} fetch={dt:.0f}ms "
        f"first={fmt_ms(nals[0][0])} last={fmt_ms(nals[-1][0])}",
        flush=True,
    )
    h264 = b"".join(payload for _, _, payload in nals if payload)
    t1 = time.perf_counter()
    pictures = decode_h264(h264)
    print(
        f"decoded {len(pictures)} pictures in {1000*(time.perf_counter()-t1):.0f}ms "
        f"h264={len(h264)} bytes",
        flush=True,
    )
    if not pictures:
        print("decode produced 0 pictures", flush=True)
        return 3
    h0 = hashlib.md5(pictures[0].tobytes()).hexdigest()[:12]
    h1 = hashlib.md5(pictures[-1].tobytes()).hexdigest()[:12]
    print(f"frame0 {pictures[0].shape} md5={h0}  frameN md5={h1} changed={h0 != h1}", flush=True)
    if args.no_gui:
        return 0

    import cv2

    win = "photon replay Sep12 3:29-3:30 PM (q quit) — not HTML"
    cv2.namedWindow(win, cv2.WINDOW_NORMAL)
    delay_ms = max(16, int(60000 / max(len(pictures), 1)))
    for i, im in enumerate(pictures):
        vis = im.copy()
        cv2.putText(
            vis,
            f"{i+1}/{len(pictures)}  {vis.shape[1]}x{vis.shape[0]}  replay not live  q=quit",
            (16, 32),
            cv2.FONT_HERSHEY_SIMPLEX,
            0.7,
            (0, 220, 0),
            2,
            cv2.LINE_AA,
        )
        cv2.imshow(win, vis)
        if (cv2.waitKey(delay_ms) & 0xFF) in (ord("q"), 27):
            break
    cv2.destroyAllWindows()
    return 0


if __name__ == "__main__":
    sys.exit(main())
