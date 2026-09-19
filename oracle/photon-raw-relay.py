#!/usr/bin/env python3
"""Oracle raw CANP tee: car -> Oracle -> clients, no MacBook in the client path.

Public TCP :6100 is terminated here (not DNATed). First bytes decide the role:

  CAN1...  FPGA / car ingest. Byte-copied to MacBook :6100 (store) and to every
           authenticated client. MacBook down does not drop the car or clients.
  PHAU...  Subscriber. After the same PHAU password photon-server uses, the
           socket receives the live car bytes (undecoded CANP).

Optional extra listen PHOTON_FANOUT (default 6110) is clients-only.
"""
from __future__ import annotations

import os
import select
import socket
import threading
import time
from typing import Optional

INGRESS_PORT = int(os.environ.get("PHOTON_INGRESS_PORT", "6100"))
FANOUT_PORT = int(os.environ.get("PHOTON_FANOUT_PORT", "6110"))
STORE = os.environ.get("PHOTON_STORE", "10.66.0.2:6100").strip()
PASSWORD = os.environ.get("PHOTON_PASSWORD", "laptop").encode("utf-8")
BIND = os.environ.get("PHOTON_BIND", "0.0.0.0")
AUTH_MAGIC = b"PHAU"
CANP_MAGIC = b"CAN1"
AUTH_MAX = 64
MAX_CLIENTS = 16


def log(msg: str) -> None:
    print(time.strftime("%Y-%m-%d %H:%M:%S"), msg, flush=True)


def tcp_listen(port: int) -> socket.socket:
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind((BIND, port))
    s.listen(32)
    s.setblocking(True)
    return s


def tcp_connect(host: str, port: int, timeout: float = 2.0) -> socket.socket:
    s = socket.create_connection((host, port), timeout=timeout)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    return s


def nodelay(sock: socket.socket) -> None:
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)


def try_send(sock: socket.socket, data: bytes) -> bool:
    try:
        sock.settimeout(0.05)
        sock.sendall(data)
        return True
    except OSError:
        return False
    finally:
        try:
            sock.settimeout(None)
        except OSError:
            pass


def recv_exact(sock: socket.socket, n: int, timeout: float) -> bytes:
    sock.settimeout(timeout)
    buf = bytearray()
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise OSError("eof")
        buf.extend(chunk)
    sock.settimeout(None)
    return bytes(buf)


def classify(sock: socket.socket) -> tuple[str, bytes]:
    """Return ('car'|'client'|'drop', prefix_already_read)."""
    try:
        head = recv_exact(sock, 4, 2.0)
    except OSError:
        return "drop", b""
    if head == AUTH_MAGIC:
        return "client", head
    return "car", head


def auth_client(sock: socket.socket, already: bytes) -> bool:
    if already != AUTH_MAGIC:
        return False
    try:
        n = recv_exact(sock, 1, 2.0)[0]
        if n < 1 or n > AUTH_MAX:
            return False
        got = recv_exact(sock, n, 2.0)
    except OSError:
        return False
    return got == PASSWORD


class Hub:
    def __init__(self) -> None:
        self.mu = threading.Lock()
        self.car: Optional[socket.socket] = None
        self.car_addr = ""
        self.store: Optional[socket.socket] = None
        self.clients: list[socket.socket] = []
        self.bytes_out = 0
        self.batches = 0

    def set_car(self, sock: socket.socket, addr: str) -> None:
        with self.mu:
            old = self.car
            self.car = sock
            self.car_addr = addr
        if old is not None:
            log(f"car replace {addr} (closing previous)")
            _close(old)
        log(f"car {addr}")

    def add_client(self, sock: socket.socket, addr: str) -> bool:
        with self.mu:
            if len(self.clients) >= MAX_CLIENTS:
                return False
            self.clients.append(sock)
            n = len(self.clients)
        log(f"client {addr} n={n}")
        return True

    def drop(self, sock: Optional[socket.socket], why: str) -> None:
        if sock is None:
            return
        with self.mu:
            if sock is self.car:
                self.car = None
                self.car_addr = ""
                kind = "car"
            elif sock is self.store:
                self.store = None
                kind = "store"
            elif sock in self.clients:
                self.clients.remove(sock)
                kind = "client"
            else:
                kind = "idle"
        log(f"drop {kind} {why}")
        _close(sock)

    def fanout(self, data: bytes) -> None:
        with self.mu:
            dests = []
            if self.store is not None:
                dests.append(("store", self.store))
            dests.extend(("client", s) for s in list(self.clients))
            self.bytes_out += len(data)
            self.batches += 1
            n = self.batches
        dead: list[tuple[str, socket.socket]] = []
        for kind, s in dests:
            if not try_send(s, data):
                dead.append((kind, s))
        for kind, s in dead:
            self.drop(s, f"{kind} send fail")
        if n == 1 or n % 500 == 0:
            with self.mu:
                log(
                    f"fwd bytes={self.bytes_out} n={n} car={self.car_addr or '-'} "
                    f"store={'up' if self.store else 'down'} clients={len(self.clients)}"
                )


def _close(sock: Optional[socket.socket]) -> None:
    if sock is None:
        return
    try:
        sock.shutdown(socket.SHUT_RDWR)
    except OSError:
        pass
    try:
        sock.close()
    except OSError:
        pass


HUB = Hub()
STOP = threading.Event()


def store_host_port() -> Optional[tuple[str, int]]:
    if not STORE or STORE.lower() in {"off", "-", "none"}:
        return None
    host, _, port = STORE.rpartition(":")
    if not host or not port:
        return None
    return host, int(port)


def ensure_store_now() -> None:
    dest = store_host_port()
    if dest is None:
        return
    with HUB.mu:
        if HUB.store is not None or HUB.car is None:
            return
    host, port = dest
    try:
        s = tcp_connect(host, port, timeout=2.0)
    except OSError as exc:
        log(f"store connect fail {exc}")
        return
    with HUB.mu:
        if HUB.store is not None or HUB.car is None:
            _close(s)
            return
        HUB.store = s
    log(f"store connected {host}:{port}")


def store_loop() -> None:
    dest = store_host_port()
    if dest is None:
        log("store copy off")
        return
    host, port = dest
    log(f"store copy {host}:{port}")
    while not STOP.is_set():
        with HUB.mu:
            has_car = HUB.car is not None
            has_store = HUB.store is not None
        if not has_car or has_store:
            time.sleep(0.5)
            continue
        ensure_store_now()
        time.sleep(2.0)
    _close(HUB.store)


def serve_conn(sock: socket.socket, addr: tuple, clients_only: bool = False) -> None:
    nodelay(sock)
    peer = f"{addr[0]}:{addr[1]}"
    role, prefix = classify(sock)
    if role == "drop":
        _close(sock)
        return
    if clients_only and role != "client":
        log(f"fanout reject non-PHAU {peer}")
        _close(sock)
        return
    if role == "client":
        if not auth_client(sock, prefix):
            log(f"client auth fail {peer}")
            _close(sock)
            return
        if not HUB.add_client(sock, peer):
            log(f"client full {peer}")
            _close(sock)
            return
        # Drain (and ignore) anything the client writes so TCP does not stall.
        try:
            while not STOP.is_set():
                r, _, _ = select.select([sock], [], [], 1.0)
                if not r:
                    continue
                data = sock.recv(4096)
                if not data:
                    break
        except OSError:
            pass
        HUB.drop(sock, "client closed")
        return

    # car
    HUB.set_car(sock, peer)
    ensure_store_now()
    try:
        if prefix:
            HUB.fanout(prefix)
        while not STOP.is_set():
            r, _, _ = select.select([sock], [], [], 1.0)
            if not r:
                continue
            data = sock.recv(65536)
            if not data:
                break
            HUB.fanout(data)
    except OSError as exc:
        log(f"car read {exc}")
    HUB.drop(sock, "car closed")


def accept_loop(listener: socket.socket, label: str, clients_only: bool = False) -> None:
    log(f"listen {label} {listener.getsockname()}")
    while not STOP.is_set():
        try:
            listener.settimeout(1.0)
            sock, addr = listener.accept()
        except TimeoutError:
            continue
        except OSError as exc:
            if STOP.is_set():
                return
            log(f"accept {label} {exc}")
            time.sleep(0.2)
            continue
        threading.Thread(
            target=serve_conn, args=(sock, addr, clients_only), daemon=True
        ).start()


def main() -> int:
    ingress = tcp_listen(INGRESS_PORT)
    fanout = None
    if FANOUT_PORT and FANOUT_PORT != INGRESS_PORT:
        try:
            fanout = tcp_listen(FANOUT_PORT)
        except OSError as exc:
            log(f"fanout :{FANOUT_PORT} not bound ({exc}); PHAU clients use :{INGRESS_PORT}")
    threading.Thread(target=store_loop, daemon=True).start()
    threading.Thread(target=accept_loop, args=(ingress, "ingress", False), daemon=True).start()
    if fanout is not None:
        threading.Thread(target=accept_loop, args=(fanout, "fanout", True), daemon=True).start()
    log(
        f"raw relay ingress=:{INGRESS_PORT} fanout=:{FANOUT_PORT if fanout else 'off'} "
        f"store={STORE or 'off'} auth=PHAU"
    )
    try:
        while True:
            time.sleep(60)
    except KeyboardInterrupt:
        pass
    STOP.set()
    _close(ingress)
    _close(fanout)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
