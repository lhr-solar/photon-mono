import socket
import struct
import time
import threading
from collections import defaultdict

SERVER_IP = "127.0.0.1"  # or real AWS IP if remote
PORT = 6100

CAN_STRUCT = struct.Struct("<IB8sB")
CAN_SIZE = CAN_STRUCT.size

WINDOW = 1.0

stats = defaultdict(lambda: {"count": 0, "last": b"", "bus": 0})
lock = threading.Lock()

def reporter():
    while True:
        time.sleep(WINDOW)

        with lock:
            snap = dict(stats)
            stats.clear()

        print("\n" + "="*80)
        print("LIVE OBSERVER VIEW")
        print("="*80)

        for cid, v in sorted(snap.items()):
            print(
                f"0x{cid:08X}  bus={v['bus']}  "
                f"count={v['count']}  "
                f"last={v['last'].hex()}"
            )

sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
sock.connect((SERVER_IP, PORT))

threading.Thread(target=reporter, daemon=True).start()

buffer = b""

while True:
    data = sock.recv(65536)
    if not data:
        break

    buffer += data

    packets = len(buffer) // CAN_SIZE
    if packets == 0:
        continue

    chunk = buffer[:packets * CAN_SIZE]
    buffer = buffer[packets * CAN_SIZE:]

    with lock:
        for i in range(packets):
            start = i * CAN_SIZE
            can_id, dlc, payload, bus = CAN_STRUCT.unpack(chunk[start:start+CAN_SIZE])

            stats[can_id]["count"] += 1
            stats[can_id]["last"] = payload[:dlc]
            stats[can_id]["bus"] = bus
