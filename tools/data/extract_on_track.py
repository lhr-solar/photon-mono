#!/usr/bin/env python3
"""Extract VSD1 frames that fall in on-track racing windows.

Mirrors photon-next/core/src/log_sessions.cpp:
  enter   speed >= speed_min_mps  (MC_VehicleVelocity, else Speed_GPS)
  leave   speed < speed_off_mps (Schmitt hysteresis; crawl does not split)
  hole    <= hole_s stays one run
  run     < min_moving_s is dropped
  gap     <= merge_gap_s joins sessions (pit / brief stop)
  pad     pad_s on each end

Writes:
  OUT/catalog.json              day index for a later "open this test day" UI
  OUT/days/YYYY-MM-DD/run.log   concatenated on-track VSD1 for that local day
  OUT/days/YYYY-MM-DD/session-NN.log   one file per stint

Parser matches log_coverage.py / vsd1.cpp / canp.cpp.
"""

from __future__ import annotations

import argparse
import json
import os
import struct
import time
from collections import defaultdict
from datetime import datetime, timedelta, timezone
from pathlib import Path

VSD1_MAGIC = 0x56534431
VSD1_HEADER_SIZE = 24
CANP_MAGIC = 0x43414E31
CANP_VERSION = 3
CANP_MAX_BATCH = 64
CANP_HEADER_SIZE = 20
CANP_PACKET_SIZE = 29
CAN_ID_MASK = 0x1FFFFFFF
MAX_PAYLOAD = 16 * 1024 * 1024

MC_VELOCITY_ID = 1059  # MC_VelocityMeasurement
GPS_LAT_ID = 4288
GPS_LON_ID = 4289
GPS_SPEED_ID = 4290

DEFAULT_LOGS = Path(r"F:\lhr\_logs_extract\logs")
DEFAULT_OUT = Path(r"F:\lhr\_logs_extract\on_track")

_VSD1_HDR = struct.Struct(">I I Q I I")
_CANP_HDR = struct.Struct(">I H H I Q")
_PKT_ID = struct.Struct(">I")
_F32 = struct.Struct("<f")
_I32 = struct.Struct("<i")
_U16 = struct.Struct("<H")


def fmt_ts(ms: int, tz: timezone) -> str:
    return datetime.fromtimestamp(ms / 1000.0, tz=timezone.utc).astimezone(tz).strftime(
        "%Y-%m-%d %H:%M:%S"
    )


def date_of(ms: int, tz: timezone) -> str:
    return datetime.fromtimestamp(ms / 1000.0, tz=timezone.utc).astimezone(tz).strftime("%Y-%m-%d")


def decode_speed(can_id: int, data: bytes, dlc: int) -> float | None:
    if can_id == MC_VELOCITY_ID and dlc >= 8:
        return float(_F32.unpack_from(data, 4)[0])
    if can_id == GPS_SPEED_ID and dlc >= 2:
        return float(_U16.unpack_from(data, 0)[0]) * 0.01
    return None


def decode_gps(can_id: int, data: bytes, dlc: int) -> tuple[str, float] | None:
    if can_id == GPS_LAT_ID and dlc >= 4:
        return ("lat", float(_I32.unpack_from(data, 0)[0]) * 1e-6)
    if can_id == GPS_LON_ID and dlc >= 4:
        return ("lon", float(_I32.unpack_from(data, 0)[0]) * 1e-6)
    return None


def extract_packets(payload: bytes) -> list[tuple[int, list[tuple[int, bytes, int]]]]:
    """Return (timestamp_ms, [(can_id, data8, dlc), ...]) batches."""
    out: list[tuple[int, list[tuple[int, bytes, int]]]] = []
    i = 0
    n = len(payload)
    while i + CANP_HEADER_SIZE <= n:
        magic, version, count, _seq, timestamp = _CANP_HDR.unpack_from(payload, i)
        if magic != CANP_MAGIC:
            i += 1
            continue
        if version != CANP_VERSION or count < 1 or count > CANP_MAX_BATCH:
            i += 1
            continue
        need = CANP_HEADER_SIZE + count * CANP_PACKET_SIZE
        available = (n - i - CANP_HEADER_SIZE) // CANP_PACKET_SIZE
        take = count if i + need <= n else available
        if take <= 0:
            break
        packets: list[tuple[int, bytes, int]] = []
        off = i + CANP_HEADER_SIZE
        for _ in range(take):
            raw = _PKT_ID.unpack_from(payload, off)[0]
            dlc = payload[off + 4]
            data = payload[off + 5 : off + 13]
            packets.append((raw & CAN_ID_MASK, data, dlc))
            off += CANP_PACKET_SIZE
        out.append((timestamp, packets))
        if take < count:
            break
        i += need
    return out


def detect_sessions(
    speeds: list[tuple[int, float]],
    gps: list[tuple[int, float, float]],
    *,
    speed_min: float,
    speed_off: float,
    min_moving_s: float,
    merge_gap_s: float,
    pad_s: float,
    hole_s: float,
    tz: timezone,
) -> list[dict]:
    ordered = sorted(speeds, key=lambda x: x[0])
    hole_ms = int(max(0.0, hole_s) * 1000)
    min_ms = int(max(0.0, min_moving_s) * 1000)
    merge_ms = int(max(0.0, merge_gap_s) * 1000)
    pad_ms = int(max(0.0, pad_s) * 1000)
    enter = speed_min
    leave = speed_off if speed_off > 0.0 and speed_off <= enter else enter

    runs: list[tuple[int, int]] = []
    in_run = False
    start = end = last_moving = 0

    def close() -> None:
        nonlocal in_run
        if not in_run:
            return
        if end >= start and (end - start) >= min_ms:
            runs.append((start, end))
        in_run = False

    for ts, speed in ordered:
        moving = speed >= (leave if in_run else enter)
        if moving:
            if not in_run:
                start = end = ts
                in_run = True
            elif ts > last_moving + hole_ms:
                close()
                start = end = ts
                in_run = True
            else:
                end = ts
            last_moving = ts
        elif in_run and ts > last_moving + hole_ms:
            close()
    close()

    merged: list[list[int]] = []
    for a, b in runs:
        if merged and a <= merged[-1][1] + merge_ms:
            merged[-1][1] = max(merged[-1][1], b)
        else:
            merged.append([a, b])

    sessions: list[dict] = []
    for raw_start, raw_end in merged:
        start_ms = max(0, raw_start - pad_ms)
        end_ms = raw_end + pad_ms
        vals = [s for t, s in ordered if raw_start <= t <= raw_end and s >= 0]
        gps_hit = [g for g in gps if start_ms <= g[0] <= end_ms]
        date = date_of(start_ms, tz)
        session = {
            "date": date,
            "start_ms": start_ms,
            "end_ms": end_ms,
            "raw_start_ms": raw_start,
            "raw_end_ms": raw_end,
            "duration_s": (end_ms - start_ms) / 1000.0,
            "speed_max_mps": max(vals) if vals else 0.0,
            "speed_mean_mps": (sum(vals) / len(vals)) if vals else 0.0,
            "speed_samples": len(vals),
        }
        if gps_hit:
            lats = [g[1] for g in gps_hit]
            lons = [g[2] for g in gps_hit]
            session["gps"] = {
                "lat": sum(lats) / len(lats),
                "lon": sum(lons) / len(lons),
                "lat_min": min(lats),
                "lat_max": max(lats),
                "lon_min": min(lons),
                "lon_max": max(lons),
            }
        sessions.append(session)

    by_date: dict[str, int] = defaultdict(int)
    for session in sessions:
        by_date[session["date"]] += 1
        n = by_date[session["date"]]
        session["id"] = f"{session['date']}-{n:02d}"
        session["file"] = f"days/{session['date']}/session-{n:02d}.log"
    return sessions


def in_sessions(ts: int, ranges: list[tuple[int, int]]) -> bool:
    lo, hi = 0, len(ranges)
    while lo < hi:
        mid = (lo + hi) // 2
        if ts < ranges[mid][0]:
            hi = mid
        else:
            lo = mid + 1
    if lo == 0:
        return False
    return ts <= ranges[lo - 1][1]


def session_for(ts: int, sessions: list[dict]) -> dict | None:
    lo, hi = 0, len(sessions)
    while lo < hi:
        mid = (lo + hi) // 2
        if ts < sessions[mid]["start_ms"]:
            hi = mid
        else:
            lo = mid + 1
    if lo == 0:
        return None
    s = sessions[lo - 1]
    return s if ts <= s["end_ms"] else None


def scan_speeds(files: list[Path]) -> tuple[list[tuple[int, float]], list[tuple[int, float, float]]]:
    speeds: list[tuple[int, float]] = []
    pending_lat: dict[int, float] = {}
    pending_lon: dict[int, float] = {}
    gps: list[tuple[int, float, float]] = []

    for i, path in enumerate(files, start=1):
        t0 = time.perf_counter()
        n_speed = 0
        with path.open("rb", buffering=1024 * 1024) as fh:
            while True:
                header = fh.read(VSD1_HEADER_SIZE)
                if len(header) < VSD1_HEADER_SIZE:
                    break
                magic, payload_bytes, _fid, _ingest, _crc = _VSD1_HDR.unpack(header)
                if magic != VSD1_MAGIC:
                    fh.seek(-(VSD1_HEADER_SIZE - 1), os.SEEK_CUR)
                    continue
                if payload_bytes > MAX_PAYLOAD:
                    fh.seek(-(VSD1_HEADER_SIZE - 1), os.SEEK_CUR)
                    continue
                payload = fh.read(payload_bytes) if payload_bytes else b""
                if payload_bytes and len(payload) < payload_bytes:
                    break
                for timestamp, packets in extract_packets(payload):
                    best: float | None = None
                    for can_id, data, dlc in packets:
                        speed = decode_speed(can_id, data, dlc)
                        if speed is None:
                            g = decode_gps(can_id, data, dlc)
                            if g is None:
                                continue
                            kind, value = g
                            if kind == "lat":
                                pending_lat[timestamp] = value
                            else:
                                pending_lon[timestamp] = value
                            continue
                        if can_id == MC_VELOCITY_ID:
                            best = speed
                        elif best is None:
                            best = speed
                    if best is not None:
                        speeds.append((timestamp, best))
                        n_speed += 1
                    if timestamp in pending_lat and timestamp in pending_lon:
                        gps.append((timestamp, pending_lat.pop(timestamp), pending_lon.pop(timestamp)))
        print(
            f"  scan [{i:02d}/{len(files)}] {path.name}  +{n_speed:,} speed  {time.perf_counter() - t0:.1f}s",
            flush=True,
        )
    return speeds, gps


def write_extract(
    files: list[Path],
    sessions: list[dict],
    out_dir: Path,
) -> dict[str, int]:
    ranges = [(s["start_ms"], s["end_ms"]) for s in sessions]
    day_handles: dict[str, object] = {}
    sess_handles: dict[str, object] = {}
    bytes_out = 0
    frames_out = 0
    try:
        for i, path in enumerate(files, start=1):
            t0 = time.perf_counter()
            kept = 0
            with path.open("rb", buffering=1024 * 1024) as fh:
                while True:
                    header = fh.read(VSD1_HEADER_SIZE)
                    if len(header) < VSD1_HEADER_SIZE:
                        break
                    magic, payload_bytes, _fid, _ingest, _crc = _VSD1_HDR.unpack(header)
                    if magic != VSD1_MAGIC:
                        fh.seek(-(VSD1_HEADER_SIZE - 1), os.SEEK_CUR)
                        continue
                    if payload_bytes > MAX_PAYLOAD:
                        fh.seek(-(VSD1_HEADER_SIZE - 1), os.SEEK_CUR)
                        continue
                    payload = fh.read(payload_bytes) if payload_bytes else b""
                    if payload_bytes and len(payload) < payload_bytes:
                        break
                    batches = extract_packets(payload)
                    if not batches:
                        continue
                    ts = batches[0][0]
                    if not in_sessions(ts, ranges):
                        continue
                    session = session_for(ts, sessions)
                    if session is None:
                        continue
                    blob = header + payload
                    date = session["date"]
                    if date not in day_handles:
                        day_dir = out_dir / "days" / date
                        day_dir.mkdir(parents=True, exist_ok=True)
                        day_handles[date] = (day_dir / "run.log").open("wb")
                    if session["id"] not in sess_handles:
                        sess_path = out_dir / session["file"]
                        sess_path.parent.mkdir(parents=True, exist_ok=True)
                        sess_handles[session["id"]] = sess_path.open("wb")
                    day_handles[date].write(blob)
                    sess_handles[session["id"]].write(blob)
                    bytes_out += len(blob)
                    frames_out += 1
                    kept += 1
            print(
                f"  copy [{i:02d}/{len(files)}] {path.name}  kept {kept:,} frames  "
                f"{time.perf_counter() - t0:.1f}s",
                flush=True,
            )
    finally:
        for h in day_handles.values():
            h.close()
        for h in sess_handles.values():
            h.close()
    return {"bytes": bytes_out, "frames": frames_out}


def build_catalog(
    sessions: list[dict],
    source: Path,
    tz_name: str,
    tz_offset_min: int,
    params: dict,
) -> dict:
    days: dict[str, dict] = {}
    for session in sessions:
        day = days.setdefault(
            session["date"],
            {
                "date": session["date"],
                "path": f"days/{session['date']}",
                "session_count": 0,
                "duration_s": 0.0,
                "sessions": [],
            },
        )
        public = {
            "id": session["id"],
            "date": session["date"],
            "start_ms": session["start_ms"],
            "end_ms": session["end_ms"],
            "duration_s": session["duration_s"],
            "speed_max_mps": session["speed_max_mps"],
            "speed_mean_mps": session["speed_mean_mps"],
            "speed_samples": session["speed_samples"],
            "file": session["file"],
        }
        if "gps" in session:
            public["gps"] = session["gps"]
        day["sessions"].append(public)
        day["session_count"] += 1
        day["duration_s"] += session["duration_s"]
    return {
        "version": 1,
        "source": str(source),
        "timezone": tz_name,
        "detector": {**params, "tz_offset_min": tz_offset_min},
        "days": list(days.values()),
    }


def main() -> int:
    ap = argparse.ArgumentParser(description="Extract on-track VSD1 windows by test day")
    ap.add_argument("--logs", type=Path, default=DEFAULT_LOGS)
    ap.add_argument("--out", type=Path, default=DEFAULT_OUT)
    ap.add_argument("--speed-min", type=float, default=2.0)
    ap.add_argument("--speed-off", type=float, default=0.75, help="leave-run speed (hysteresis)")
    ap.add_argument("--min-moving", type=float, default=45.0)
    ap.add_argument("--merge-gap", type=float, default=180.0)
    ap.add_argument("--pad", type=float, default=15.0)
    ap.add_argument("--hole", type=float, default=8.0)
    ap.add_argument("--tz", default="America/Chicago")
    ap.add_argument("--catalog-only", action="store_true")
    args = ap.parse_args()

    if not args.logs.exists():
        print(f"Logs path not found: {args.logs}")
        return 2
    files = sorted(p for p in args.logs.iterdir() if p.is_file() and p.suffix.lower() == ".log")
    if not files:
        print(f"No .log segments in {args.logs}")
        return 2

    try:
        from zoneinfo import ZoneInfo

        tz = ZoneInfo(args.tz)
    except Exception:
        tz = timezone(timedelta(hours=-5), name=args.tz)

    now = datetime.now(tz)
    tz_offset_min = int(now.utcoffset().total_seconds() // 60) if now.utcoffset() else -300

    print(f"Scanning {len(files)} segments in {args.logs}")
    t0 = time.perf_counter()
    speeds, gps = scan_speeds(files)
    print(f"Speed samples {len(speeds):,}  GPS fixes {len(gps):,}  {time.perf_counter() - t0:.1f}s")

    sessions = detect_sessions(
        speeds,
        gps,
        speed_min=args.speed_min,
        speed_off=args.speed_off,
        min_moving_s=args.min_moving,
        merge_gap_s=args.merge_gap,
        pad_s=args.pad,
        hole_s=args.hole,
        tz=tz,
    )
    if not sessions:
        print("No on-track sessions found (speed never stayed above threshold).")
        return 1

    catalog = build_catalog(
        sessions,
        args.logs,
        args.tz,
        tz_offset_min,
        {
            "speed_min_mps": args.speed_min,
            "speed_off_mps": args.speed_off,
            "min_moving_s": args.min_moving,
            "merge_gap_s": args.merge_gap,
            "pad_s": args.pad,
            "hole_s": args.hole,
        },
    )

    args.out.mkdir(parents=True, exist_ok=True)
    catalog_path = args.out / "catalog.json"
    catalog_path.write_text(json.dumps(catalog, indent=2), encoding="utf-8")

    print()
    print(f"On-track sessions: {len(sessions)} across {len(catalog['days'])} day(s)")
    for day in catalog["days"]:
        print(f"  {day['date']}  {day['session_count']} session(s)  {day['duration_s'] / 60.0:.1f} min")
        for s in day["sessions"]:
            print(
                f"    {s['id']}  {fmt_ts(s['start_ms'], tz)} -> {fmt_ts(s['end_ms'], tz)}  "
                f"{s['duration_s'] / 60.0:.1f} min  max {s['speed_max_mps']:.1f} m/s"
            )
    print(f"Catalog: {catalog_path}")

    if args.catalog_only:
        return 0

    stats = write_extract(files, sessions, args.out)
    print(
        f"Wrote {stats['frames']:,} VSD1 frames, {stats['bytes'] / (1024**2):.1f} MiB -> {args.out}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
