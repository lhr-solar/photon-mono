"""Drive three replay transitions through the real hidden native UI.

Usage: python tests/test_ui_video_switch.py UI.exe sample1.h264 sample2.h264 sample3.h264
The final short run reaches Pause; stale replies/frames from earlier runs
arrive after the switch. A capture is kept in the UI build directory.
"""
import argparse
import base64
import hashlib
import os
from pathlib import Path
import re
import socket
import struct
import subprocess
import threading
import time
from test_ui_reconnect import read_exact, receive, send_text


def binary(conn, kind, sequence, payload):
    data = struct.pack('<HBBII', 0x5057, 1, kind, sequence, len(payload)) + payload
    header = b'\x82\x7f' + struct.pack('!Q', len(data))
    conn.sendall(header + data)


def tagged(conn, kind, query, payload):
    inner = struct.pack('<HBBII', 0x5057, 1, kind, query, len(payload)) + payload
    binary(conn, 8, query, inner)


def units(data):
    starts = [m.start() for m in re.finditer(b'\x00\x00\x00\x01|\x00\x00\x01', data)]
    return [data[a:b] for a, b in zip(starts, starts[1:] + [len(data)])]


def kind(nal):
    return nal[4 if nal.startswith(b'\0\0\0\1') else 3] & 31


def first_mb(nal):
    start = 5 if nal.startswith(b'\0\0\0\1') else 4
    bits = ''.join(f'{b:08b}' for b in nal[start:])
    zeros = len(bits) - len(bits.lstrip('0'))
    return int(bits[zeros:zeros * 2 + 1], 2) - 1


def locate(base, end, sps, pps, found=True):
    prefix = struct.pack('>BqBqIQQ', 0 if found else 1, 0, 1, 0, 2 if found else 0, base, end)
    prefix += struct.pack('>H', len(sps)) + sps + struct.pack('>H', len(pps)) + pps
    if not found:
        return prefix + b'\0\0\0\0'
    row = struct.pack('>QQQIIIBBH', base, end, 0, 4096, 1, 30, 0, 0, 0)
    later = struct.pack('>QQQIIIBBH', end + 1, end + 900, 4096, 4096, 1, 30, 0, 0, 0)
    return prefix + struct.pack('>H', 2) + row + later + struct.pack('>HIQQ', 1, 1, base, end)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('ui', type=Path)
    parser.add_argument('samples', nargs=3, type=Path)
    args = parser.parse_args()
    samples = [units(p.read_bytes()) for p in args.samples]
    bases = [1000000, 3000000, 2000000]  # Include a backwards run switch.
    requested = {}
    sent_next = set()
    errors = []
    stopped = threading.Event()
    listener = socket.socket()
    listener.bind(('127.0.0.1', 0)); listener.listen(); listener.settimeout(1)

    def catalog(conn, index):
        base = bases[index]
        send_text(conn, {'type': 'catalog_result', 'sessions': [{'id': 'switch-fixture',
                  'min_ms': base, 'max_ms': base + 300, 'dbc_hash': 'fixture', 'has_video': True}]})

    def serve():
        try:
            conn, _ = listener.accept()
            with conn:
                conn.settimeout(0.1)
                request = b''
                while b'\r\n\r\n' not in request: request += read_exact(conn, 1)
                headers = dict(line.split(':', 1) for line in request.decode().split('\r\n')[1:] if ':' in line)
                key = next(v.strip() for k, v in headers.items() if k.lower() == 'sec-websocket-key')
                accept = base64.b64encode(hashlib.sha1((key + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').encode()).digest()).decode()
                conn.sendall(('HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ' + accept + '\r\n\r\n').encode())
                while not stopped.is_set():
                    try: opcode, data = receive(conn)
                    except socket.timeout: continue
                    if opcode == 8: break
                    if opcode != 1: continue
                    import json
                    message = json.loads(data)
                    if message['type'] == 'hello':
                        send_text(conn, {'type':'hello_ack','server':'video-switch-fixture',
                                        'active_source':{'kind':'daq_tcp','host':'127.0.0.1','port':6500}})
                        catalog(conn, 0)
                    if message['type'] != 'video_locate' or not message['at_ms']: continue
                    at = message['at_ms']; query = message['query_id']
                    index = min(range(3), key=lambda i: abs(bases[i] - at))
                    base = bases[index]
                    requested.setdefault(index, query)
                    nals = samples[index]
                    sps = next(n for n in nals if kind(n) == 7)
                    pps = next(n for n in nals if kind(n) == 8)
                    pictures = sum(kind(n) in (1,5) and first_mb(n) == 0 for n in nals)
                    end = base + (pictures - 1) * 33
                    if index:
                        # A delayed missing-video reply and junk live frame
                        # would poison or latch the old decoder without guards.
                        tagged(conn, 7, requested[0], locate(bases[0], bases[0]+900, b'', b'', False))
                        binary(conn, 4, 0, struct.pack('<QB3x', bases[0], 0) + b'\0\0\1\x41\x80')
                    tagged(conn, 7, query, locate(base, end, sps, pps))
                    picture = -1
                    for nal in nals:
                        t = kind(nal)
                        if t in (1,5) and first_mb(nal) == 0: picture += 1
                        flags = 2 if t in (7,8) else 1 if t == 5 else 0
                        tagged(conn, 4, query, struct.pack('<QB3x', base + max(0,picture)*33, flags) + nal)
                        time.sleep(0.002)  # NALs arrive across multiple UI paints after the pointer.
                    send_text(conn, {'type':'video_status', 'query_id':query, 'connected':True, 'mode':'cursor'})
                    if index < 2 and index not in sent_next:
                        sent_next.add(index); catalog(conn, index+1)
        except (EOFError, BrokenPipeError, ConnectionResetError): pass
        except Exception as exc: errors.append(str(exc))

    worker = threading.Thread(target=serve, daemon=True); worker.start()
    capture = args.ui.resolve().parent / 'video-three-run-pause.bmp'
    env = dict(os.environ, PHOTON_GL3D='0', PHOTON_PIN_SESSION='switch-fixture')
    try:
        result = subprocess.run([str(args.ui.resolve()), '--private', '--screen','live','--ws',
                 'ws://127.0.0.1:'+str(listener.getsockname()[1]), '--capture',str(capture),
                 '--width','1200','--height','800','--frames','600'], env=env,
                 capture_output=True, text=True, timeout=30)
        assert result.returncode == 0, result.stdout + result.stderr
        assert not errors, errors
        assert len(requested) == 3, requested
        assert len(set(requested.values())) == 3, requested
        assert capture.exists()
        print('Native UI switched through three runs with unique seek IDs and rejected stale packets; paused capture:', capture)
    finally:
        stopped.set(); listener.close(); worker.join(timeout=2)


if __name__ == '__main__': main()
