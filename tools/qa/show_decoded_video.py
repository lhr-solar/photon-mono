"""Fetch GOPs from photon-server :6700, decode H.264, show in an OpenCV window.

Proves whether the store bitstream is playable independent of photon-ui.
Usage:
  python show_decoded_video.py --host 127.0.0.1 --port 6700
  python show_decoded_video.py --at 0 --seconds 20
"""
from __future__ import annotations

import argparse
import hashlib
import os
import socket
import struct
import subprocess
import sys
import tempfile
import time

PASSWORD = b"laptop"


def recvn(sock: socket.socket, n: int) -> bytes:
    buf = bytearray()
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise RuntimeError("eof from video store")
        buf.extend(chunk)
    return bytes(buf)


def auth_blob() -> bytes:
    return b"PHAU" + bytes([len(PASSWORD)]) + PASSWORD


def locate_req(at_ms: int, prefetch: int) -> bytes:
    return bytes([3]) + struct.pack(">Q", at_ms) + bytes([0]) + struct.pack(">H", prefetch)


def range_req(seg: int, off: int, length: int) -> bytes:
    return bytes([4]) + struct.pack(">Q", 0) + struct.pack(">I", seg) + struct.pack(
        ">Q", off
    ) + struct.pack(">I", length)


def parse_locate(body: bytes) -> dict:
    off = 0
    status = body[off]
    off += 1
    clock = struct.unpack_from(">Q", body, off)[0]
    off += 8
    prov = body[off]
    off += 1
    residual = struct.unpack_from(">q", body, off)[0]
    off += 8
    total = struct.unpack_from(">I", body, off)[0]
    off += 4
    min_ms, max_ms = struct.unpack_from(">QQ", body, off)
    off += 16

    def blob() -> bytes:
        nonlocal off
        ln = struct.unpack_from(">H", body, off)[0]
        off += 2
        data = body[off : off + ln]
        off += ln
        return data

    sps = blob()
    pps = blob()
    n_rows = struct.unpack_from(">H", body, off)[0]
    off += 2
    rows = []
    for _ in range(n_rows):
        start, end, byte_off, byte_len, seg, nal_count, stream, flags, _res = struct.unpack_from(
            ">QQQIIIBBH", body, off
        )
        off += 40
        rows.append(
            {
                "start": start,
                "end": end,
                "off": byte_off,
                "len": byte_len,
                "seg": seg,
                "nals": nal_count,
                "flags": flags,
            }
        )
    return {
        "found": status == 0,
        "clock": clock,
        "prov": prov,
        "residual": residual,
        "total": total,
        "min_ms": min_ms,
        "max_ms": max_ms,
        "sps": sps,
        "pps": pps,
        "rows": rows,
    }


def annexb_ok(data: bytes) -> bool:
    return len(data) >= 4 and data[:4] in (b"\x00\x00\x00\x01", b"\x00\x00\x01\x00"[:3] + data[3:4])


def with_start_code(nal: bytes) -> bytes:
    if nal.startswith(b"\x00\x00\x00\x01") or nal.startswith(b"\x00\x00\x01"):
        return nal
    return b"\x00\x00\x00\x01" + nal


def decode_gop(h264: bytes) -> list:
    import cv2

    fd, path = tempfile.mkstemp(suffix=".h264")
    os.close(fd)
    try:
        with open(path, "wb") as f:
            f.write(h264)
        cap = cv2.VideoCapture(path, cv2.CAP_FFMPEG)
        if not cap.isOpened():
            # Fallback: ffmpeg to png sequence in the same temp dir.
            return decode_gop_ffmpeg(h264)
        frames = []
        while True:
            ok, frame = cap.read()
            if not ok:
                break
            frames.append(frame)
        cap.release()
        if not frames:
            return decode_gop_ffmpeg(h264)
        return frames
    finally:
        try:
            os.remove(path)
        except OSError:
            pass


def decode_gop_ffmpeg(h264: bytes) -> list:
    import cv2

    work = tempfile.mkdtemp(prefix="photon-gop-")
    src = os.path.join(work, "gop.h264")
    dst = os.path.join(work, "f.png")
    with open(src, "wb") as f:
        f.write(h264)
    r = subprocess.run(
        [
            "ffmpeg",
            "-hide_banner",
            "-loglevel",
            "error",
            "-f",
            "h264",
            "-i",
            src,
            "-frames:v",
            "60",
            "-vsync",
            "0",
            os.path.join(work, "f_%03d.png"),
        ],
        capture_output=True,
        text=True,
    )
    frames = []
    if r.returncode == 0:
        names = sorted(n for n in os.listdir(work) if n.endswith(".png"))
        for name in names:
            im = cv2.imread(os.path.join(work, name))
            if im is not None:
                frames.append(im)
    else:
        print("ffmpeg:", r.stderr.strip() or r.returncode)
    for name in os.listdir(work):
        try:
            os.remove(os.path.join(work, name))
        except OSError:
            pass
    try:
        os.rmdir(work)
    except OSError:
        pass
    return frames


def fetch_gop(sock: socket.socket, at_ms: int) -> tuple[dict, bytes, float]:
    t0 = time.perf_counter()
    sock.sendall(locate_req(at_ms, 1))
    n = struct.unpack(">I", recvn(sock, 4))[0]
    body = recvn(sock, n) if n else b""
    loc = parse_locate(body)
    if not loc["rows"]:
        return loc, b"", 1000 * (time.perf_counter() - t0)
    # Entry is first. Do not pick max startMs — future-stamped prefetch GOPs
    # are why Live froze on an old still.
    row = loc["rows"][0]
    sock.sendall(range_req(row["seg"], row["off"], row["len"]))
    rn = struct.unpack(">I", recvn(sock, 4))[0]
    payload = recvn(sock, rn) if rn else b""
    dt = 1000 * (time.perf_counter() - t0)
    h264 = b""
    if loc["sps"]:
        h264 += with_start_code(loc["sps"])
    if loc["pps"]:
        h264 += with_start_code(loc["pps"])
    h264 += payload
    loc["picked"] = row
    return loc, h264, dt


def main() -> int:
    p = argparse.ArgumentParser(description="Decode photon-server GOPs and display them")
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=6700)
    p.add_argument("--at", type=int, default=0, help="video_locate timestamp; 0 = newest GOP")
    p.add_argument("--seconds", type=float, default=15.0)
    p.add_argument("--no-gui", action="store_true")
    args = p.parse_args()

    print(f"connecting {args.host}:{args.port} at={args.at}")
    sock = socket.create_connection((args.host, args.port), timeout=8)
    sock.sendall(auth_blob())
    loc, h264, dt = fetch_gop(sock, args.at)
    row = loc.get("picked")
    print(
        f"locate {dt:.1f}ms found={loc['found']} rows={len(loc['rows'])} "
        f"sps={len(loc['sps'])} pps={len(loc['pps'])} min={loc['min_ms']} max={loc['max_ms']}"
    )
    if row:
        print(
            f"gop start={row['start']} end={row['end']} span={row['end']-row['start']}ms "
            f"seg={row['seg']} off={row['off']} len={row['len']} nals={row['nals']}"
        )
    print(f"h264 bytes={len(h264)} annexb={h264[:8].hex()}")
    if not h264:
        print("no GOP bytes")
        return 2

    t1 = time.perf_counter()
    frames = decode_gop(h264)
    print(f"decoded {len(frames)} frames in {1000*(time.perf_counter()-t1):.0f}ms")
    if not frames:
        print("decode produced 0 pictures — bitstream/client issue still possible, store GOP is empty of pictures")
        return 3
    h0 = hashlib.md5(frames[0].tobytes()).hexdigest()[:12]
    print(f"frame0 {frames[0].shape} md5={h0}")

    if args.no_gui:
        sock.close()
        return 0

    import cv2

    win = "photon GOP (q quit)"
    cv2.namedWindow(win, cv2.WINDOW_NORMAL)
    deadline = time.time() + args.seconds
    poll = 0
    last_md5 = h0
    changed = 0
    while time.time() < deadline:
        if frames:
            for fr in frames:
                vis = fr.copy()
                cv2.putText(
                    vis,
                    f"poll={poll} n={len(frames)} {vis.shape[1]}x{vis.shape[0]} q=quit",
                    (16, 32),
                    cv2.FONT_HERSHEY_SIMPLEX,
                    0.7,
                    (0, 220, 0),
                    2,
                    cv2.LINE_AA,
                )
                cv2.imshow(win, vis)
                key = cv2.waitKey(33) & 0xFF
                if key in (ord("q"), 27):
                    deadline = 0
                    break
        if deadline == 0:
            break
        try:
            loc, h264, dt = fetch_gop(sock, args.at)
        except Exception as exc:
            print("refetch failed", exc)
            break
        poll += 1
        if not h264:
            print(f"poll {poll} empty gop dt={dt:.1f}ms")
            continue
        new_frames = decode_gop(h264)
        if not new_frames:
            print(f"poll {poll} decode 0 dt={dt:.1f}ms bytes={len(h264)}")
            continue
        md5 = hashlib.md5(new_frames[0].tobytes()).hexdigest()[:12]
        if md5 != last_md5:
            changed += 1
            last_md5 = md5
        print(
            f"poll {poll} dt={dt:.1f}ms frames={len(new_frames)} "
            f"md5={md5} changed={changed} gop_nals={loc.get('picked', {}).get('nals')}"
        )
        frames = new_frames
    sock.close()
    cv2.destroyAllWindows()
    print(f"done polls={poll} picture_changes={changed}")
    if args.at == 0 and poll > 2 and changed == 0:
        print("LIVE GOP first-frame md5 never changed — store is sending a still, or camera is idle")
    return 0


if __name__ == "__main__":
    sys.exit(main())
