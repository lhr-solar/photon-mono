import subprocess
import socket
import struct
import random
import time

# ---------- Configuration ----------
DEST_IP = "127.0.0.1"
DEST_PORT = 5004
FPS = 30
MAX_RTP_PAYLOAD = 1200
PAYLOAD_TYPE = 96
FFMPEG_PATH = "ffmpeg"

FFMPEG_CMD = [
    FFMPEG_PATH,
    "-f", "dshow",
    "-video_size", "1920x1080",
    "-framerate", str(FPS),
    "-i", "video=ASUS FHD webcam",
    "-c:v", "libx264",
    "-preset", "veryfast",               # balances speed vs compression
    "-tune", "zerolatency",              # low-latency streaming
    "-profile:v", "baseline",            # max compatibility
    "-pix_fmt", "yuv420p",               # ensures clean first frame
    "-g", str(FPS // 2),                 # more frequent keyframes (every 0.5 sec)
    "-keyint_min", "1",                  # allow minimum 1-frame interval between keyframes
    "-x264opts", "slice-max-size=10000", # larger slice size to prevent bottom-half distortion
    "-f", "rtp",
    f"rtp://{DEST_IP}:{DEST_PORT}",
    "-loglevel", "quiet"
]


RTP_HEADER_SIZE = 12


# ---------- Helper Functions ----------
def build_rtp_header(seq, ts, ssrc, marker):
    header = bytearray(RTP_HEADER_SIZE)
    header[0] = 0x80
    header[1] = (0x80 if marker else 0x00) | PAYLOAD_TYPE
    header[2] = (seq >> 8) & 0xFF
    header[3] = seq & 0xFF
    header[4] = (ts >> 24) & 0xFF
    header[5] = (ts >> 16) & 0xFF
    header[6] = (ts >> 8) & 0xFF
    header[7] = ts & 0xFF
    header[8] = (ssrc >> 24) & 0xFF
    header[9] = (ssrc >> 16) & 0xFF
    header[10] = (ssrc >> 8) & 0xFF
    header[11] = ssrc & 0xFF
    return header

def find_nal_start(buf, start=0):
    # Find the next start code (3 or 4 bytes)
    for i in range(start, len(buf)-3):
        if buf[i]==0 and buf[i+1]==0:
            if buf[i+2]==1:
                return i, 3
            elif i+3 < len(buf) and buf[i+2]==0 and buf[i+3]==1:
                return i, 4
    return -1, 0

def extract_nals(buf):
    nals = []
    offset = 0
    while True:
        start, sc_len = find_nal_start(buf, offset)
        if start == -1:
            break
        next_start, _ = find_nal_start(buf, start + sc_len)
        if next_start == -1:
            nal = buf[start + sc_len:]
            nals.append(nal)
            break
        else:
            nal = buf[start + sc_len:next_start]
            nals.append(nal)
            offset = next_start
    return nals

def packetize_nal(nal):
    if len(nal) <= MAX_RTP_PAYLOAD:
        return [nal]
    # FU-A fragmentation
    F = (nal[0] & 0x80) >> 7
    NRI = (nal[0] & 0x60) >> 5
    T = nal[0] & 0x1F
    fu_indicator = (F << 7) | (NRI << 5) | 28
    mtu = MAX_RTP_PAYLOAD - 2
    payload = nal[1:]
    chunks = []
    for i in range(0, len(payload), mtu):
        chunk = payload[i:i+mtu]
        fu_header = 0
        if i == 0:
            fu_header |= 0x80  # Start
        if i + mtu >= len(payload):
            fu_header |= 0x40  # End
        fu_header |= T
        chunks.append(bytes([fu_indicator, fu_header]) + chunk)
    return chunks

# ---------- Main Loop ----------
def main():
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    addr = (DEST_IP, DEST_PORT)
    seq = random.randint(0, 65535)
    ts = random.randint(0, 0xFFFFFFFF)
    ssrc = random.randint(0, 0xFFFFFFFF)
    ts_inc = int(90000 / FPS)
    frame_count = 0
    sps_pps_sent = False
    leftover = b""

    print("Starting FFmpeg...")
    proc = subprocess.Popen(FFMPEG_CMD, stdout=subprocess.PIPE, bufsize=0)
    print("Streaming to RTP...")

    try:
        while True:
            data = proc.stdout.read(4096)
            if not data:
                break
            leftover += data
            nals = extract_nals(leftover)
            if not nals:
                continue
            # Keep trailing bytes if incomplete NAL
            last_nal_end = leftover.rfind(nals[-1])
            leftover = leftover[last_nal_end + len(nals[-1]):] if last_nal_end != -1 else b""

            for nal in nals:
                if len(nal) == 0:
                    continue
                nal_type = nal[0] & 0x1F
                frame_count += 1
                # print(f"[Frame {frame_count}] NAL type={nal_type}, size={len(nal)}")

                packets = []
                marker = True
                # Only prepend SPS/PPS once at start before first IDR
                if nal_type == 7 or nal_type == 8:
                    if not sps_pps_sent:
                        packets.append(nal)
                        sps_pps_sent = True
                    else:
                        continue  # Skip repeated SPS/PPS
                else:
                    # Packetize NAL if too big
                    packets.extend(packetize_nal(nal))
                    # Marker bit only on last RTP packet of a frame
                    for i in range(len(packets)-1):
                        send_pkt = build_rtp_header(seq, ts, ssrc, False) + packets[i]
                        sock.sendto(send_pkt, addr)
                        # print(f"  Sent packet {i+1}/{len(packets)}, size={len(send_pkt)}, seq={seq}, ts={ts}, marker=False")
                        seq = (seq + 1) % 65536
                    # Last packet marker=True
                    send_pkt = build_rtp_header(seq, ts, ssrc, True) + packets[-1]
                    sock.sendto(send_pkt, addr)
                    # print(f"  Sent packet {len(packets)}/{len(packets)}, size={len(send_pkt)}, seq={seq}, ts={ts}, marker=True")
                    seq = (seq + 1) % 65536
                    ts += ts_inc
                    time.sleep(1.0 / FPS)
    except KeyboardInterrupt:
        print("Stopping stream.")
    finally:
        proc.terminate()
        sock.close()

if __name__ == "__main__":
    main()
