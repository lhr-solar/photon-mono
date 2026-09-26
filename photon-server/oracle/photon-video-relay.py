#!/usr/bin/env python3
"""Oracle UDP video tee: car RTP :6200 -> Mac ingest + live PVU1 :6800.

Camera still sends RTP/H.264 PT=96 to public UDP :6200. Each datagram is
copied to MACBOOK :6200 for the Saturday store. Oracle depacketizes NALs
and fans them out as the existing PVU1 UDP protocol on :6800 so UIs do not
wait on a WireGuard round trip.

Subscribe is PHAU + password + cmd 1 (same as photon-server VideoUdpFanout).
"""
from __future__ import annotations

import os
import select
import socket
import struct
import threading
import time
from typing import Optional

BIND = os.environ.get("PHOTON_BIND", "0.0.0.0")
INGRESS_PORT = int(os.environ.get("PHOTON_VIDEO_INGRESS_PORT", "6200"))
FANOUT_PORT = int(os.environ.get("PHOTON_VIDEO_FANOUT_PORT", "6800"))
STORE = os.environ.get("PHOTON_VIDEO_STORE", "10.66.0.2:6200").strip()
PASSWORD = os.environ.get("PHOTON_PASSWORD", "laptop").encode("utf-8")
SUB_TIMEOUT_S = 3.0
MAX_SUBS = 8
PVU1_MAGIC = 0x50565531
PVU1_HDR = 22
PVU1_MAX = 1200
FLAG_KEY = 1
FLAG_CFG = 2
AUTH_MAGIC = b"PHAU"
ANNEXB = b"\x00\x00\x00\x01"


def log(msg: str) -> None:
    print(time.strftime("%Y-%m-%d %H:%M:%S"), msg, flush=True)


def now_ms() -> int:
    return int(time.time() * 1000)


def udp_bind(port: int) -> socket.socket:
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind((BIND, port))
    s.setblocking(False)
    return s


def parse_host_port(spec: str) -> Optional[tuple[str, int]]:
    if not spec or spec.lower() in {"off", "-", "none"}:
        return None
    host, _, port = spec.rpartition(":")
    if not host or not port:
        return None
    return host, int(port)


def parse_rtp(datagram: bytes) -> Optional[tuple[bytes, int]]:
    if len(datagram) < 12 or (datagram[0] >> 6) != 2:
        return None
    if (datagram[1] & 0x7F) != 96:
        return None
    cc = datagram[0] & 0x0F
    header = 12 + cc * 4
    if header > len(datagram):
        return None
    if datagram[0] & 0x10:
        if header + 4 > len(datagram):
            return None
        ext = struct.unpack(">H", datagram[header + 2 : header + 4])[0]
        header += 4 + ext * 4
        if header > len(datagram):
            return None
    end = len(datagram)
    if datagram[0] & 0x20:
        pad = datagram[-1]
        if pad == 0 or pad > end - header:
            return None
        end -= pad
    if header >= end:
        return None
    seq = struct.unpack(">H", datagram[2:4])[0]
    return datagram[header:end], seq


class RTPDepacketizer:
    def __init__(self) -> None:
        self.expected = 0
        self.started = False
        self.in_fu = False
        self.fu: bytearray = bytearray()

    def reset_fu(self) -> None:
        self.in_fu = False
        self.fu = bytearray()

    def push(self, datagram: bytes) -> list[bytes]:
        parsed = parse_rtp(datagram)
        if parsed is None:
            return []
        payload, seq = parsed
        if self.started:
            delta = (seq - self.expected + 32768) % 65536 - 32768
            if delta != 0:
                self.reset_fu()
                if delta < 0:
                    return []
        self.expected = (seq + 1) & 0xFFFF
        self.started = True
        if not payload:
            return []
        nal = payload[0] & 0x1F
        if 1 <= nal <= 23:
            self.reset_fu()
            return [ANNEXB + payload]
        if nal == 24:
            self.reset_fu()
            return self._stapa(payload)
        if nal == 28:
            return self._fua(payload)
        self.reset_fu()
        return []

    def _stapa(self, payload: bytes) -> list[bytes]:
        out: list[bytes] = []
        off = 1
        while off + 2 <= len(payload):
            size = struct.unpack(">H", payload[off : off + 2])[0]
            off += 2
            if size <= 0 or off + size > len(payload):
                break
            out.append(ANNEXB + payload[off : off + size])
            off += size
        return out

    def _fua(self, payload: bytes) -> list[bytes]:
        if len(payload) < 2:
            self.reset_fu()
            return []
        start = payload[1] & 0x80
        end = payload[1] & 0x40
        frag_type = payload[1] & 0x1F
        frag = payload[2:]
        if start:
            header = (payload[0] & 0xE0) | frag_type
            self.fu = bytearray([header])
            self.fu.extend(frag)
            self.in_fu = True
        elif not self.in_fu:
            return []
        else:
            if len(self.fu) + len(frag) > 16 * 1024 * 1024:
                self.reset_fu()
                return []
            self.fu.extend(frag)
        if not self.in_fu or not end:
            return []
        nal = bytes(self.fu)
        self.reset_fu()
        return [ANNEXB + nal]


def nal_type(annexb: bytes) -> int:
    if annexb.startswith(b"\x00\x00\x00\x01") and len(annexb) > 4:
        return annexb[4] & 0x1F
    if annexb.startswith(b"\x00\x00\x01") and len(annexb) > 3:
        return annexb[3] & 0x1F
    return 0


def nal_flags(nt: int) -> int:
    flags = 0
    if nt == 5:
        flags |= FLAG_KEY
    if nt in (7, 8):
        flags |= FLAG_CFG
    return flags


def discardable(nt: int) -> bool:
    return nt in (6, 9, 10, 11, 12)


def encode_pvu1(seq: int, ts_ms: int, flags: int, payload: bytes) -> list[bytes]:
    n = max(len(payload), 1)
    count = (n + PVU1_MAX - 1) // PVU1_MAX
    out: list[bytes] = []
    off = 0
    for i in range(count):
        end = min(off + PVU1_MAX, len(payload))
        chunk = payload[off:end]
        pkt = bytearray(PVU1_HDR + len(chunk))
        struct.pack_into(">I", pkt, 0, PVU1_MAGIC)
        struct.pack_into(">I", pkt, 4, seq & 0xFFFFFFFF)
        struct.pack_into(">H", pkt, 8, i)
        struct.pack_into(">H", pkt, 10, count)
        struct.pack_into(">Q", pkt, 12, ts_ms)
        pkt[20] = flags & 0xFF
        pkt[21] = 0
        pkt[PVU1_HDR:] = chunk
        out.append(bytes(pkt))
        off = end
    return out


def auth_cmd(buf: bytes) -> Optional[int]:
    if len(buf) < 6 or buf[:4] != AUTH_MAGIC:
        return None
    n = buf[4]
    if n < 1 or n > 64 or 5 + n >= len(buf):
        return None
    if buf[5 : 5 + n] != PASSWORD:
        return None
    return buf[5 + n]


class Hub:
    def __init__(self) -> None:
        self.mu = threading.Lock()
        self.subs: dict[tuple[str, int], float] = {}
        self.seq = 0
        self.sps: Optional[bytes] = None
        self.pps: Optional[bytes] = None
        self.key: Optional[bytes] = None
        self.rtp = 0
        self.nals = 0


HUB = Hub()
STOP = threading.Event()


def prune_subs() -> None:
    cutoff = time.time() - SUB_TIMEOUT_S
    with HUB.mu:
        dead = [a for a, seen in HUB.subs.items() if seen < cutoff]
        for a in dead:
            del HUB.subs[a]


def sendto(sock: socket.socket, addr: tuple[str, int], data: bytes) -> None:
    try:
        sock.sendto(data, addr)
    except OSError:
        pass


def seed_sub(fanout: socket.socket, addr: tuple[str, int]) -> None:
    ts = now_ms()
    with HUB.mu:
        sps, pps, key, seq = HUB.sps, HUB.pps, HUB.key, HUB.seq
    if sps:
        for pkt in encode_pvu1(max(seq, 1), ts, FLAG_CFG, sps):
            sendto(fanout, addr, pkt)
    if pps:
        for pkt in encode_pvu1(max(seq, 1), ts, FLAG_CFG, pps):
            sendto(fanout, addr, pkt)
    if key:
        for pkt in encode_pvu1(max(seq, 1), ts, FLAG_KEY, key):
            sendto(fanout, addr, pkt)


def broadcast(fanout: socket.socket, payload: bytes, flags: int) -> None:
    prune_subs()
    with HUB.mu:
        HUB.seq += 1
        seq = HUB.seq
        addrs = list(HUB.subs.keys())
        nt = nal_type(payload)
        if nt == 7:
            HUB.sps = payload
        elif nt == 8:
            HUB.pps = payload
        elif nt == 5:
            HUB.key = payload
        HUB.nals += 1
        n = HUB.nals
        rtp = HUB.rtp
    pkts = encode_pvu1(seq, now_ms(), flags, payload)
    for addr in addrs:
        for pkt in pkts:
            sendto(fanout, addr, pkt)
    if n == 1 or n % 200 == 0:
        log(f"video rtp={rtp} nals={n} subs={len(addrs)} seq={seq}")


def handle_fanout(fanout: socket.socket, data: bytes, addr: tuple[str, int]) -> None:
    cmd = auth_cmd(data)
    if cmd is None:
        return
    if cmd == 0:
        with HUB.mu:
            HUB.subs.pop(addr, None)
            n = len(HUB.subs)
        log(f"video unsub {addr[0]}:{addr[1]} n={n}")
        return
    if cmd != 1:
        return
    with HUB.mu:
        if addr not in HUB.subs and len(HUB.subs) >= MAX_SUBS:
            return
        first = addr not in HUB.subs
        HUB.subs[addr] = time.time()
        n = len(HUB.subs)
    if first:
        log(f"video sub {addr[0]}:{addr[1]} n={n}")
        seed_sub(fanout, addr)


def main() -> int:
    store = parse_host_port(STORE)
    ingress = udp_bind(INGRESS_PORT)
    fanout = udp_bind(FANOUT_PORT)
    depay = RTPDepacketizer()
    log(
        f"video relay udp :{INGRESS_PORT} -> store {STORE or 'off'} "
        f"live pvu1 :{FANOUT_PORT} auth=PHAU"
    )
    while not STOP.is_set():
        r, _, _ = select.select([ingress, fanout], [], [], 0.4)
        if fanout in r:
            while True:
                try:
                    data, addr = fanout.recvfrom(2048)
                except BlockingIOError:
                    break
                except OSError:
                    break
                handle_fanout(fanout, data, addr)
        if ingress in r:
            while True:
                try:
                    data, _addr = ingress.recvfrom(65535)
                except BlockingIOError:
                    break
                except OSError:
                    break
                with HUB.mu:
                    HUB.rtp += 1
                if store is not None:
                    sendto(ingress, store, data)
                for nal in depay.push(data):
                    nt = nal_type(nal)
                    if discardable(nt):
                        continue
                    broadcast(fanout, nal, nal_flags(nt))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        STOP.set()
        raise SystemExit(0)
