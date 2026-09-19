"""Live PVU1 UDP viewer that bypasses photon.html / WASM / ImGui.

Subscribe to Oracle (or local) :6800, assemble NALs, decode with ffmpeg,
and show lag vs the NAL timestamp (Unix ms stamped by photon-video-relay).

If this window is snappy and photon.html Live is not, the HTML renderer
(and its live-pacing clock) is the lag. If both are late, it is the pipe.

Usage:
  python show_live_udp_video.py
  python show_live_udp_video.py --host 150.136.85.207 --port 6800 --seconds 20
  python show_live_udp_video.py --no-gui --seconds 8
"""
from __future__ import annotations

import argparse
import collections
import os
import queue
import re
import socket
import struct
import subprocess
import sys
import threading
import time
from typing import Optional

PVU1_MAGIC = 0x50565531
PVU1_HDR = 22
PASSWORD = b"laptop"


def now_ms() -> int:
    return int(time.time() * 1000)


def subscribe_blob(password: bytes, on: bool = True) -> bytes:
    return b"PHAU" + bytes([len(password)]) + password + bytes([1 if on else 0])


def decode_pvu1(buf: bytes) -> Optional[dict]:
    if len(buf) < PVU1_HDR:
        return None
    magic, seq, frag_i, frag_n, ts, flags = struct.unpack_from(">IIHHQB", buf, 0)
    if magic != PVU1_MAGIC or frag_n == 0 or frag_i >= frag_n:
        return None
    return {
        "seq": seq,
        "frag_i": frag_i,
        "frag_n": frag_n,
        "ts": ts,
        "flags": flags,
        "payload": buf[PVU1_HDR:],
    }


class Assembler:
    def __init__(self) -> None:
        self.seq = None
        self.count = 0
        self.ts = 0
        self.flags = 0
        self.parts: list[Optional[bytes]] = []
        self.got = 0

    def add(self, pkt: dict) -> Optional[dict]:
        if self.seq != pkt["seq"] or self.count != pkt["frag_n"] or not self.parts:
            self.seq = pkt["seq"]
            self.count = pkt["frag_n"]
            self.ts = pkt["ts"]
            self.flags = pkt["flags"]
            self.parts = [None] * pkt["frag_n"]
            self.got = 0
        i = pkt["frag_i"]
        if i >= len(self.parts) or self.parts[i] is not None:
            return None
        self.parts[i] = pkt["payload"]
        self.got += 1
        if self.got < self.count:
            return None
        payload = b"".join(self.parts)  # type: ignore[arg-type]
        self.parts = []
        return {"seq": self.seq, "ts": self.ts, "flags": self.flags, "payload": payload}


def nal_type(annexb: bytes) -> int:
    if annexb.startswith(b"\x00\x00\x00\x01") and len(annexb) > 4:
        return annexb[4] & 0x1F
    if annexb.startswith(b"\x00\x00\x01") and len(annexb) > 3:
        return annexb[3] & 0x1F
    return 0


def with_start_code(nal: bytes) -> bytes:
    if nal.startswith(b"\x00\x00\x00\x01") or nal.startswith(b"\x00\x00\x01"):
        return nal
    return b"\x00\x00\x00\x01" + nal


def parse_sps_size(nal: bytes) -> Optional[tuple[int, int]]:
    sc = 4 if nal.startswith(b"\x00\x00\x00\x01") else 3 if nal.startswith(b"\x00\x00\x01") else 0
    i = sc + 1
    rbsp = bytearray()
    while i < len(nal):
        if i + 2 < len(nal) and nal[i] == 0 and nal[i + 1] == 0 and nal[i + 2] == 3:
            rbsp.extend(b"\x00\x00")
            i += 3
            continue
        rbsp.append(nal[i])
        i += 1
    if len(rbsp) < 4:
        return None
    bit = 0

    def get_bit() -> int:
        nonlocal bit
        idx = bit >> 3
        if idx >= len(rbsp):
            return 0
        v = (rbsp[idx] >> (7 - (bit & 7))) & 1
        bit += 1
        return v

    def u(n: int) -> int:
        v = 0
        for _ in range(n):
            v = (v << 1) | get_bit()
        return v

    def ue() -> int:
        z = 0
        while get_bit() == 0:
            z += 1
            if z > 31:
                return 0
        return ((1 << z) - 1) + u(z)

    profile = u(8)
    u(8)
    u(8)
    ue()
    high = profile in {100, 110, 122, 244, 44, 83, 86, 118, 128, 138}
    if high:
        chroma = ue()
        if chroma == 3:
            u(1)
        ue()
        ue()
        u(1)
        if u(1):
            return None
    ue()
    poc = ue()
    if poc == 0:
        ue()
    elif poc == 1:
        u(1)
        ue()
        ue()
        n = ue()
        for _ in range(n):
            ue()
    ue()
    u(1)
    w_mbs = ue()
    h_map = ue()
    frame_only = u(1)
    w = (w_mbs + 1) * 16
    h = (h_map + 1) * 16 * (2 - frame_only)
    if 16 <= w <= 4096 and 16 <= h <= 4096:
        return w, h
    return None


SIZE_RE = re.compile(r"(\d{2,4})x(\d{2,4})")


class FfmpegDecoder:
    def __init__(self) -> None:
        self.proc: Optional[subprocess.Popen] = None
        self.w = 0
        self.h = 0
        self.err_buf = b""
        self.lock = threading.Lock()
        self.frames: queue.Queue = queue.Queue(maxsize=2)
        self.reader: Optional[threading.Thread] = None
        self.err_thread: Optional[threading.Thread] = None

    def start(self, w: int, h: int) -> None:
        self.stop()
        self.w, self.h = w, h
        cmd = [
            "ffmpeg",
            "-hide_banner",
            "-loglevel",
            "info",
            "-fflags",
            "nobuffer+discardcorrupt",
            "-flags",
            "low_delay",
            "-probesize",
            "32768",
            "-analyzeduration",
            "0",
            "-f",
            "h264",
            "-i",
            "pipe:0",
            "-an",
            "-pix_fmt",
            "bgr24",
            "-f",
            "rawvideo",
            "-s",
            f"{w}x{h}",
            "pipe:1",
        ]
        self.proc = subprocess.Popen(
            cmd,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            bufsize=0,
        )
        self.reader = threading.Thread(target=self._read_out, daemon=True)
        self.err_thread = threading.Thread(target=self._read_err, daemon=True)
        self.reader.start()
        self.err_thread.start()

    def _read_err(self) -> None:
        assert self.proc and self.proc.stderr
        for line in self.proc.stderr:
            self.err_buf += line
            if len(self.err_buf) > 64 * 1024:
                self.err_buf = self.err_buf[-32 * 1024 :]

    def _read_out(self) -> None:
        assert self.proc and self.proc.stdout
        nbytes = self.w * self.h * 3
        leftover = b""
        while self.proc.poll() is None or leftover:
            chunk = self.proc.stdout.read(nbytes - len(leftover))
            if not chunk:
                if self.proc.poll() is not None:
                    break
                time.sleep(0.002)
                continue
            leftover += chunk
            if len(leftover) < nbytes:
                continue
            frame = leftover[:nbytes]
            leftover = leftover[nbytes:]
            item = (time.perf_counter(), frame)
            try:
                self.frames.put_nowait(item)
            except queue.Full:
                try:
                    self.frames.get_nowait()
                except queue.Empty:
                    pass
                try:
                    self.frames.put_nowait(item)
                except queue.Full:
                    pass

    def write(self, annexb: bytes) -> bool:
        if not self.proc or not self.proc.stdin:
            return False
        try:
            self.proc.stdin.write(annexb)
            self.proc.stdin.flush()
            return True
        except OSError:
            return False

    def take(self) -> Optional[bytes]:
        try:
            _, frame = self.frames.get_nowait()
            return frame
        except queue.Empty:
            return None

    def stop(self) -> None:
        if not self.proc:
            return
        try:
            if self.proc.stdin:
                self.proc.stdin.close()
        except OSError:
            pass
        try:
            self.proc.kill()
        except OSError:
            pass
        self.proc = None


def recv_loop(sock: socket.socket, host: str, port: int, password: bytes, stop: threading.Event, out: queue.Queue) -> None:
    asm = Assembler()
    last_sub = 0.0
    blob = subscribe_blob(password, True)
    while not stop.is_set():
        now = time.time()
        if now - last_sub > 1.0:
            try:
                sock.sendto(blob, (host, port))
            except OSError:
                pass
            last_sub = now
        try:
            sock.settimeout(0.25)
            data, _ = sock.recvfrom(2048)
        except socket.timeout:
            continue
        except OSError:
            break
        pkt = decode_pvu1(data)
        if not pkt:
            continue
        frame = asm.add(pkt)
        if frame:
            try:
                out.put_nowait((time.perf_counter(), now_ms(), frame))
            except queue.Full:
                try:
                    out.get_nowait()
                except queue.Empty:
                    pass


def main() -> int:
    p = argparse.ArgumentParser(description="Live UDP video without photon.html")
    p.add_argument("--host", default="150.136.85.207")
    p.add_argument("--port", type=int, default=6800)
    p.add_argument("--password", default="laptop")
    p.add_argument("--seconds", type=float, default=0.0, help="0 = run until q / Ctrl+C")
    p.add_argument("--no-gui", action="store_true")
    p.add_argument("--rotate", action="store_true", help="90° CCW + vflip like photon.html")
    args = p.parse_args()

    password = args.password.encode("utf-8")
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("0.0.0.0", 0))
    print(f"subscribe udp://{args.host}:{args.port} from {sock.getsockname()}", flush=True)

    stop = threading.Event()
    nals: queue.Queue = queue.Queue(maxsize=64)
    t = threading.Thread(
        target=recv_loop, args=(sock, args.host, args.port, password, stop, nals), daemon=True
    )
    t.start()

    decoder = FfmpegDecoder()
    sps = b""
    pps = b""
    started = False
    size: Optional[tuple[int, int]] = None
    last_ts = 0
    last_arrival = 0
    stats = collections.deque(maxlen=120)
    frames_in = 0
    pics = 0
    t0 = time.time()
    deadline = t0 + args.seconds if args.seconds > 0 else None
    win = "photon live UDP (q quit) — not HTML"

    try:
        import cv2  # noqa: F401
    except ImportError:
        print("cv2 missing; --no-gui stats only", flush=True)
        args.no_gui = True

    if not args.no_gui:
        import cv2

        cv2.namedWindow(win, cv2.WINDOW_NORMAL)

    print("waiting for PVU1… (camera must be sending RTP to Oracle :6200)", flush=True)

    try:
        while True:
            if deadline and time.time() > deadline:
                break
            try:
                _recv_at, wall_ms, frame = nals.get(timeout=0.05)
            except queue.Empty:
                frame = None
            if frame:
                payload = with_start_code(frame["payload"])
                nt = nal_type(payload)
                frames_in += 1
                last_ts = frame["ts"]
                last_arrival = wall_ms
                lag_ms = wall_ms - int(frame["ts"]) if frame["ts"] > 1_000_000_000_000 else None
                if lag_ms is not None:
                    stats.append(lag_ms)
                if nt == 7:
                    sps = payload
                    size = parse_sps_size(payload) or size
                elif nt == 8:
                    pps = payload
                if not started and sps and pps and size and nt in (1, 5):
                    decoder.start(size[0], size[1])
                    decoder.write(sps)
                    decoder.write(pps)
                    started = True
                    print(f"decoder {size[0]}x{size[1]}", flush=True)
                if started:
                    decoder.write(payload)
                if frames_in == 1 or frames_in % 60 == 0:
                    med = sorted(stats)[len(stats) // 2] if stats else None
                    print(
                        f"nals={frames_in} pics={pics} seq={frame['seq']} flags={frame['flags']} "
                        f"ts={frame['ts']} arrival_lag_ms={lag_ms} median={med} bytes={len(payload)}",
                        flush=True,
                    )

            raw = decoder.take() if started else None
            if raw is not None and size:
                pics += 1
                if not args.no_gui:
                    import cv2
                    import numpy as np

                    img = np.frombuffer(raw, dtype=np.uint8).reshape((size[1], size[0], 3)).copy()
                    if args.rotate:
                        img = cv2.rotate(img, cv2.ROTATE_90_COUNTERCLOCKWISE)
                        img = cv2.flip(img, 0)
                    med = sorted(stats)[len(stats) // 2] if stats else -1
                    lag = last_arrival - last_ts if last_ts > 1_000_000_000_000 else -1
                    cv2.putText(
                        img,
                        f"UDP bypass  lag {lag}ms  med {med}ms  nals {frames_in}  pics {pics}  q=quit",
                        (16, 32),
                        cv2.FONT_HERSHEY_SIMPLEX,
                        0.7,
                        (0, 220, 0),
                        2,
                        cv2.LINE_AA,
                    )
                    cv2.imshow(win, img)
            if not args.no_gui:
                import cv2

                key = cv2.waitKey(1) & 0xFF
                if key in (ord("q"), 27):
                    break
    except KeyboardInterrupt:
        pass
    finally:
        stop.set()
        decoder.stop()
        sock.close()
        if not args.no_gui:
            import cv2

            cv2.destroyAllWindows()

    med = sorted(stats)[len(stats) // 2] if stats else None
    print(
        f"done nals={frames_in} pics={pics} median_arrival_lag_ms={med} "
        f"last_ts={last_ts}",
        flush=True,
    )
    if frames_in == 0:
        print("no PVU1 received — camera/relay down, or UDP 6800 blocked", flush=True)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
