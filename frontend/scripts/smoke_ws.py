#!/usr/bin/env python3
"""Headless photon-wire v1 handshake against ws://127.0.0.1:8765."""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import os
import socket
import struct
import sys
import time


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


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=8765)
    p.add_argument("--timeout", type=float, default=4.0)
    args = p.parse_args()

    sock = socket.create_connection((args.host, args.port), timeout=args.timeout)
    try:
        handshake(sock, args.host, args.port)
        send_json(sock, {"type": "hello", "protocol_version": 1, "client": "smoke_ws"})
        deadline = time.time() + args.timeout
        saw_ack = False
        saw_catalog = False
        catalog_n = 0
        saw_telem = False
        saw_can = False
        saw_tl = False
        send_json(sock, {"type": "select_source", "source": "synthetic", "protocol_version": 1})
        send_json(sock, {"type": "subscribe_signals", "signal_ids": [1, 2, 3, 4], "protocol_version": 1})
        while time.time() < deadline:
            sock.settimeout(max(0.1, deadline - time.time()))
            try:
                opcode, data = read_frame(sock)
            except TimeoutError:
                break
            if opcode == 0x1:
                msg = json.loads(data.decode("utf-8"))
                kind = msg.get("type")
                if kind == "signal_catalog":
                    catalog_n = len(msg.get("signals") or [])
                    print("text", kind, f"signals={catalog_n}")
                else:
                    print("text", kind, msg)
                if kind == "hello_ack":
                    saw_ack = True
                if kind == "signal_catalog":
                    saw_catalog = True
                    if catalog_n < 1:
                        print("signal_catalog empty", file=sys.stderr)
                        return 2
                if kind == "error_alert":
                    print("error_alert", msg, file=sys.stderr)
                    return 2
            elif opcode == 0x2 and len(data) >= 12:
                magic, ver, mtype, seq, plen = struct.unpack_from("<HBBII", data, 0)
                print(f"bin magic=0x{magic:04x} ver={ver} type=0x{mtype:02x} seq={seq} len={plen}")
                if magic != 0x5057:
                    print("bad magic", file=sys.stderr)
                    return 3
                if mtype == 0x01:
                    saw_telem = True
                elif mtype == 0x02:
                    saw_can = True
                elif mtype == 0x05:
                    saw_tl = True
                if saw_ack and saw_telem and saw_can:
                    break
        print(
            f"result hello_ack={saw_ack} signal_catalog={saw_catalog} catalog_n={catalog_n} "
            f"telemetry={saw_telem} can={saw_can} timeline={saw_tl}"
        )
        if not (saw_ack and saw_catalog and saw_telem):
            return 1

        send_json(sock, {"type": "track_sim_list", "protocol_version": 1})
        saw_list = False
        hill = False
        deadline = time.time() + args.timeout
        while time.time() < deadline:
            sock.settimeout(max(0.1, deadline - time.time()))
            try:
                opcode, data = read_frame(sock)
            except TimeoutError:
                break
            if opcode != 0x1:
                continue
            msg = json.loads(data.decode("utf-8"))
            print("text", msg.get("type"), msg)
            if msg.get("type") == "error_alert":
                print("error_alert", msg, file=sys.stderr)
                return 2
            if msg.get("type") == "track_sim_list":
                saw_list = True
                ids = []
                for item in msg.get("tracks", []):
                    if isinstance(item, dict):
                        ids.append(item.get("id"))
                    else:
                        ids.append(item)
                print("tracks", ids)
                hill = "hill_loop" in ids
                break
        if not (saw_list and hill):
            print(f"result track_sim_list={saw_list} hill_loop={hill}")
            return 1

        send_json(
            sock,
            {
                "type": "track_sim_request",
                "protocol_version": 1,
                "track_id": "hill_loop",
                "time_budget_s": 400.0,
                "initial_soc": 1.0,
            },
        )
        saw_result = False
        feasible = False
        deadline = time.time() + args.timeout
        while time.time() < deadline:
            sock.settimeout(max(0.1, deadline - time.time()))
            try:
                opcode, data = read_frame(sock)
            except TimeoutError:
                break
            if opcode != 0x1:
                continue
            msg = json.loads(data.decode("utf-8"))
            print("text", msg.get("type"), msg)
            if msg.get("type") == "error_alert":
                print("error_alert", msg, file=sys.stderr)
                return 2
            if msg.get("type") == "track_sim_result":
                saw_result = True
                feasible = bool(msg.get("feasible"))
                segs = msg.get("segments") or []
                print(
                    "track_sim_result",
                    f"feasible={feasible}",
                    f"total_time_s={msg.get('total_time_s')}",
                    f"energy_wh={msg.get('energy_wh')}",
                    f"end_soc={msg.get('end_soc')}",
                    f"segments={len(segs)}",
                )
                if not segs:
                    print("track_sim_result missing segments", file=sys.stderr)
                    return 1
                break
        print(
            f"result track_sim_list={saw_list} hill_loop={hill} "
            f"track_sim_result={saw_result} feasible={feasible}"
        )
        if not (saw_result and feasible):
            return 1
        return 0
    finally:
        sock.close()


if __name__ == "__main__":
    sys.exit(main())
