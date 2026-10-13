"""Verify the native UI rejoins live video without restarting the shared source."""
import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import threading


def read_exact(conn, size):
    data = b''
    while len(data) < size:
        part = conn.recv(size - len(data))
        if not part:
            raise EOFError
        data += part
    return data


def receive(conn):
    first, second = read_exact(conn, 2)
    length = second & 127
    if length == 126:
        length = struct.unpack('!H', read_exact(conn, 2))[0]
    elif length == 127:
        length = struct.unpack('!Q', read_exact(conn, 8))[0]
    mask = read_exact(conn, 4) if second & 128 else b''
    data = read_exact(conn, length)
    if mask:
        data = bytes(value ^ mask[i % 4] for i, value in enumerate(data))
    return first & 15, data


def send_text(conn, message):
    data = json.dumps(message).encode()
    header = bytes([0x81, len(data)]) if len(data) < 126 else b'\x81\x7e' + struct.pack('!H', len(data))
    conn.sendall(header + data)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('ui', type=Path)
    args = parser.parse_args()
    received = []
    errors = []
    listener = socket.socket()
    listener.bind(('127.0.0.1', 0))
    listener.listen()
    listener.settimeout(0.25)
    stopped = threading.Event()

    def serve():
        connection = 0
        while not stopped.is_set():
            try:
                conn, _ = listener.accept()
            except socket.timeout:
                continue
            connection += 1
            try:
                with conn:
                    conn.settimeout(0.5)
                    request = b''
                    while b'\r\n\r\n' not in request:
                        request += read_exact(conn, 1)
                    headers = dict(line.split(':', 1) for line in request.decode().split('\r\n')[1:] if ':' in line)
                    key = next(v.strip() for k, v in headers.items() if k.lower() == 'sec-websocket-key')
                    accept = base64.b64encode(hashlib.sha1((key + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').encode()).digest()).decode()
                    conn.sendall(('HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ' + accept + '\r\n\r\n').encode())
                    while not stopped.is_set():
                        try:
                            opcode, data = receive(conn)
                        except socket.timeout:
                            continue
                        if opcode == 8:
                            break
                        if opcode != 1:
                            continue
                        message = json.loads(data)
                        received.append((connection, message['type']))
                        if message['type'] == 'hello':
                            send_text(conn, {'type': 'hello_ack', 'server': 'reconnect-fixture', 'active_source': {'kind': 'daq_tcp', 'host': '127.0.0.1', 'port': 6500}})
                        if connection == 1 and message['type'] == 'video_subscribe':
                            # Drop a joined socket, then observe the UI's real reconnect.
                            break
            except (EOFError, ConnectionResetError, BrokenPipeError):
                pass
            except Exception as exc:
                errors.append(str(exc))

    worker = threading.Thread(target=serve, daemon=True)
    worker.start()
    try:
        with tempfile.TemporaryDirectory(prefix='photon-reconnect-') as tmp:
            env = dict(os.environ, PHOTON_GL3D='0', PHOTON_UI_STORE=str(Path(tmp) / 'settings'))
            result = subprocess.run([str(args.ui.resolve()), '--screen', 'live', '--private', '--ws', 'ws://127.0.0.1:' + str(listener.getsockname()[1]), '--capture', str(Path(tmp) / 'capture.bmp'), '--frames', '3600', '--quit-after-ms', '5000'], env=env, capture_output=True, text=True, timeout=20)
        assert result.returncode == 0, result.stdout + result.stderr
        assert not errors, errors
        assert sum(kind == 'hello' for _, kind in received) >= 2, received
        assert any(epoch >= 2 and kind == 'video_subscribe' for epoch, kind in received), received
        assert not any(kind in ('select_source', 'timeline_control', 'video_stop') for _, kind in received), received
        print('Live reconnect passed: subscription restored, no source restart, seek, or decoder stop.')
    finally:
        stopped.set()
        worker.join(timeout=2)
        listener.close()


if __name__ == '__main__':
    main()
