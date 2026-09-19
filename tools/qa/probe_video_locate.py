"""Time photon-server video_locate + range on :6700."""
from __future__ import annotations

import socket
import struct
import sys
import time

PASSWORD = b"laptop"


def auth() -> bytes:
    return b"PHAU" + bytes([len(PASSWORD)]) + PASSWORD


def locate_req(at_ms: int, prefetch: int) -> bytes:
    return bytes([3]) + struct.pack(">Q", at_ms) + bytes([0]) + struct.pack(">H", prefetch)


def range_req(seg: int, off: int, length: int) -> bytes:
    return bytes([4]) + struct.pack(">Q", 0) + struct.pack(">I", seg) + struct.pack(">Q", off) + struct.pack(
        ">I", length
    )


def recvn(sock: socket.socket, n: int) -> bytes:
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise RuntimeError("eof")
        buf += chunk
    return buf


def main() -> None:
    host = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 6700
    at = int(sys.argv[3]) if len(sys.argv) > 3 else 0
    prefetch = int(sys.argv[4]) if len(sys.argv) > 4 else 1
    t0 = time.perf_counter()
    s = socket.create_connection((host, port), timeout=5)
    t_conn = time.perf_counter()
    s.sendall(auth())
    t_auth = time.perf_counter()
    s.sendall(locate_req(at, prefetch))
    n = struct.unpack(">I", recvn(s, 4))[0]
    body = recvn(s, n) if n else b""
    t_loc = time.perf_counter()
    print(f"conn={1000*(t_conn-t0):.1f}ms auth={1000*(t_auth-t_conn):.1f}ms locate={1000*(t_loc-t_auth):.1f}ms reply={n}B")
    if n < 20:
        print("short locate reply")
        return
    # status u8, clock u64, prov u8, residual i64, then rows...
    # skip parse of header; find first GOP row-ish: look at known layout from tests
    # After decodeLocateReply: we'll print first 32 bytes
    print("head", body[:16].hex())
    off = 0
    status = body[off]; off += 1
    off += 8 + 1 + 8 + 4 + 8 + 8  # clock, prov, residual, totalRows, min, max
    def blob():
        nonlocal off
        ln = struct.unpack_from(">H", body, off)[0]
        off += 2 + ln
    blob(); blob()
    n_rows = struct.unpack_from(">H", body, off)[0]
    off += 2
    print(f"status={status} rows={n_rows}")
    if n_rows == 0 or off + 40 > len(body):
        print("no gop row")
        return
    start, end, byte_off, byte_len, seg, nal = struct.unpack_from(">QQQIII", body, off)
    print(f"gop start={start} end={end} seg={seg} off={byte_off} len={byte_len} nals={nal}")
    s.sendall(range_req(seg, byte_off, byte_len))
    rn = struct.unpack(">I", recvn(s, 4))[0]
    payload = recvn(s, rn) if rn else b""
    t_range = time.perf_counter()
    print(f"range={1000*(t_range-t_loc):.1f}ms bytes={len(payload)} total={1000*(t_range-t0):.1f}ms")


if __name__ == "__main__":
    main()
