#!/usr/bin/env python3
"""Scan VSD1 log segments and report CAN id coverage against the car DBC.

Mirrors photon-next core framing:
  vsd1.cpp  — 24-byte BE header (magic u32, payload u32, id u64, ingest u32, crc u32)
  canp.cpp  — CANP v3 BE header (magic u32, version u16, count u16, seq u32, ts u64)
              then count * 29-byte packets (can_id u32, dlc u8, data[8], dt u16[8])
  dbc.cpp   — BO_ decimal ids, matched with can_id & 0x1FFFFFFF

Streams one VSD1 frame at a time; never loads a whole segment into RAM.
"""

from __future__ import annotations

import argparse
import os
import re
import struct
import sys
import time
from collections import defaultdict
from datetime import datetime, timezone
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

CRITICAL = [
    "BPS_Status",
    "BPS_Voltage_Aggregate_Arr",
    "BPS_Temperature_Aggregate_Arr",
    "BPS_Fault_Val_Arr",
    "VCU_Status",
    "MC_Status",
    "MC_BusMeasurement",
    "MC_VelocityMeasurement",
    "MC_Motor_TempMeasurement",
    "Pedal_Status",
    "Brake_Pressure_1",
    "Brake_Pressure_2",
    "Supp_Battery_Status",
    "LV_Carrier_Status",
    "Pump_Status",
    "Coolant_Temperature",
    "ChargerInterface_Status",
    "MPPT_A_PowerMeasurements",
    "MPPT_B_PowerMeasurements",
    "MPPT_C_PowerMeasurements",
    "Suspension_FL",
    "Suspension_FR",
    "Suspension_Back",
    "WheelRPM_FL",
    "WheelRPM_FR",
    "LWS_Standard",
    "Lattitude_GPS",
    "Longitude_GPS",
    "Speed_GPS",
    "IMU_Accel_FR",
    "Telemetry_Status",
]

# parents[3] is the photon-mono root. Default scan is the checked-in 30 s clip.
_REPO = Path(__file__).resolve().parents[3]
DEFAULT_LOGS = _REPO / "examples" / "brainerd-30s"
DEFAULT_DBC = _REPO / "photon-next" / "assets" / "dbc" / "carcan_telemcan.dbc"
DEFAULT_OUT = _REPO / "examples" / "brainerd-30s" / "coverage.md"

_VSD1_HDR = struct.Struct(">I I Q I I")
_CANP_HDR = struct.Struct(">I H H I Q")
_PKT_ID = struct.Struct(">I")
_BO_LINE = re.compile(r"^BO_\s+(\d+)\s+(\S+?)\s*:")


def parse_dbc(path: Path) -> dict[int, str]:
    """BO_ id -> name, masking extended-flag bits the way dbc.cpp does."""
    by_id: dict[int, str] = {}
    with path.open("r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            m = _BO_LINE.match(line)
            if not m:
                continue
            can_id = int(m.group(1)) & CAN_ID_MASK
            name = m.group(2)
            if name.endswith(":"):
                name = name[:-1]
            by_id[can_id] = name
    return by_id


def fmt_ts(ms: int | None) -> str:
    if ms is None:
        return "—"
    sec = ms / 1000.0
    dt = datetime.fromtimestamp(sec, tz=timezone.utc)
    return dt.strftime("%Y-%m-%d %H:%M:%S.%f")[:-3] + "Z"


def rate_hz(count: int, first_ms: int, last_ms: int) -> float | None:
    if count <= 0 or last_ms <= first_ms:
        return None
    return count / ((last_ms - first_ms) / 1000.0)


def fmt_rate(rate: float | None) -> str:
    if rate is None:
        return "—"
    if rate >= 100:
        return f"{rate:.1f}"
    if rate >= 1:
        return f"{rate:.2f}"
    return f"{rate:.4f}"


def fmt_int(n: int) -> str:
    return f"{n:,}"


def extract_canp(payload: bytes) -> list[tuple[int, list[int], bool]]:
    """Return (timestamp_ms, [masked can ids], truncated) batches.

    Mirrors canp::StreamParser (resync one byte on bad header). If a valid
    header remains but the VSD1 payload is short of count*29, keep every
    complete 29-byte packet (C++ drops the whole incomplete batch).
    """
    out: list[tuple[int, list[int], bool]] = []
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
        ids: list[int] = []
        off = i + CANP_HEADER_SIZE
        for _ in range(take):
            raw = _PKT_ID.unpack_from(payload, off)[0]
            ids.append(raw & CAN_ID_MASK)
            off += CANP_PACKET_SIZE
        truncated = take < count
        out.append((timestamp, ids, truncated))
        if truncated:
            break
        i += need
    return out


class IdStat:
    __slots__ = ("count", "first_ms", "last_ms")

    def __init__(self) -> None:
        self.count = 0
        self.first_ms: int | None = None
        self.last_ms: int | None = None

    def add(self, ts: int, n: int = 1) -> None:
        self.count += n
        if self.first_ms is None or ts < self.first_ms:
            self.first_ms = ts
        if self.last_ms is None or ts > self.last_ms:
            self.last_ms = ts


def scan_file(path: Path, stats: dict[int, IdStat], counters: dict[str, int]) -> None:
    with path.open("rb", buffering=1024 * 1024) as fh:
        while True:
            header = fh.read(VSD1_HEADER_SIZE)
            if len(header) < VSD1_HEADER_SIZE:
                break
            magic, payload_bytes, _fid, _ingest, _crc = _VSD1_HDR.unpack(header)
            if magic != VSD1_MAGIC:
                # Slide one byte. A bad alignment should not skip the rest of the file.
                fh.seek(-(VSD1_HEADER_SIZE - 1), os.SEEK_CUR)
                counters["vsd1_resync"] += 1
                continue
            if payload_bytes > MAX_PAYLOAD:
                fh.seek(-(VSD1_HEADER_SIZE - 1), os.SEEK_CUR)
                counters["vsd1_bad_len"] += 1
                continue
            payload = fh.read(payload_bytes) if payload_bytes else b""
            if payload_bytes and len(payload) < payload_bytes:
                counters["vsd1_short"] += 1
                break
            counters["vsd1_frames"] += 1
            batches = extract_canp(payload)
            if not batches:
                counters["non_canp"] += 1
                continue
            for timestamp, ids, truncated in batches:
                counters["canp_batches"] += 1
                if truncated:
                    counters["canp_truncated"] += 1
                counters["can_frames"] += len(ids)
                grouped: dict[int, int] = defaultdict(int)
                for can_id in ids:
                    grouped[can_id] += 1
                for can_id, n in grouped.items():
                    stats[can_id].add(timestamp, n)
                if counters["min_ts"] == -1 or timestamp < counters["min_ts"]:
                    counters["min_ts"] = timestamp
                if timestamp > counters["max_ts"]:
                    counters["max_ts"] = timestamp


def write_report(
    out_path: Path,
    dbc_path: Path,
    logs_dir: Path,
    files: list[Path],
    by_id: dict[int, str],
    stats: dict[int, IdStat],
    counters: dict[str, int],
    elapsed_s: float,
    total_bytes: int,
) -> None:
    name_to_id = {name: can_id for can_id, name in by_id.items()}
    unknown = [(can_id, st) for can_id, st in stats.items() if can_id not in by_id]
    unknown.sort(key=lambda x: x[1].count, reverse=True)
    unknown_frames = sum(st.count for _, st in unknown)

    present = []
    missing = []
    crit_rows = []
    for name in CRITICAL:
        can_id = name_to_id.get(name)
        st = stats.get(can_id) if can_id is not None else None
        if can_id is None:
            crit_rows.append((name, None, "NOT IN DBC", 0, None, None, None))
            missing.append(name)
            continue
        if st is None or st.count == 0:
            crit_rows.append((name, can_id, "MISSING", 0, None, None, None))
            missing.append(name)
        else:
            rate = rate_hz(st.count, st.first_ms or 0, st.last_ms or 0)
            crit_rows.append((name, can_id, "PRESENT", st.count, rate, st.first_ms, st.last_ms))
            present.append(name)

    min_ts = counters["min_ts"] if counters["min_ts"] != -1 else None
    max_ts = counters["max_ts"] if counters["max_ts"] else None
    span_s = ((max_ts - min_ts) / 1000.0) if min_ts is not None and max_ts and max_ts > min_ts else None

    lines: list[str] = []
    lines.append("# VSD1 / CAN log coverage")
    lines.append("")
    lines.append(f"Generated: {datetime.now(timezone.utc).strftime('%Y-%m-%d %H:%M:%SZ')}")
    lines.append("")
    lines.append("Read-only scan of recorded VSD1 segments for Longhorn Racing Solar.")
    lines.append("Parser mirrors `photon-next/core/src/vsd1.cpp`, `canp.cpp`, and `dbc.cpp`")
    lines.append("(VSD1 id is u64; CANP version/count are u16; DBC match is `can_id & 0x1FFFFFFF`).")
    lines.append("Truncated CANP batches (valid header, short VSD1 payload) keep complete packets;")
    lines.append("core `StreamParser` would drop those batches.")
    lines.append("")
    lines.append("## Run")
    lines.append("")
    lines.append("| Item | Value |")
    lines.append("|---|---|")
    lines.append(f"| Logs | `{logs_dir}` |")
    lines.append(f"| Segments | {len(files)} |")
    lines.append(f"| Bytes scanned | {fmt_int(total_bytes)} ({total_bytes / (1024**3):.2f} GiB) |")
    lines.append(f"| DBC | `{dbc_path}` ({len(by_id)} BO_ messages) |")
    lines.append(f"| Runtime | {elapsed_s:.1f} s |")
    lines.append("")
    lines.append("## Log-set summary")
    lines.append("")
    lines.append("| Metric | Value |")
    lines.append("|---|---|")
    lines.append(f"| VSD1 frames | {fmt_int(counters['vsd1_frames'])} |")
    lines.append(f"| VSD1 resync steps | {fmt_int(counters['vsd1_resync'])} |")
    lines.append(f"| VSD1 short/truncated files | {fmt_int(counters['vsd1_short'])} |")
    lines.append(f"| Non-CANP payloads | {fmt_int(counters['non_canp'])} |")
    lines.append(f"| CANP batches | {fmt_int(counters['canp_batches'])} |")
    lines.append(f"| CANP truncated batches | {fmt_int(counters['canp_truncated'])} |")
    lines.append(f"| Total CAN frames | {fmt_int(counters['can_frames'])} |")
    lines.append(f"| Unique CAN ids | {fmt_int(len(stats))} |")
    lines.append(f"| Unknown-id frames | {fmt_int(unknown_frames)} |")
    lines.append(f"| Unique unknown ids | {fmt_int(len(unknown))} |")
    lines.append(f"| First timestamp | {fmt_ts(min_ts)} (`{min_ts}`) |")
    lines.append(f"| Last timestamp | {fmt_ts(max_ts)} (`{max_ts}`) |")
    if span_s is not None:
        lines.append(f"| Time span | {span_s:,.1f} s ({span_s / 3600.0:.2f} h) |")
    else:
        lines.append("| Time span | — |")
    lines.append("")
    lines.append("### Segments")
    lines.append("")
    lines.append("| File | Bytes |")
    lines.append("|---|---|")
    for p in files:
        lines.append(f"| `{p.name}` | {fmt_int(p.stat().st_size)} |")
    lines.append("")
    lines.append("## Critical-message coverage")
    lines.append("")
    lines.append(f"PRESENT: **{len(present)}** · MISSING: **{len(missing)}** · checked: {len(CRITICAL)}")
    lines.append("")
    lines.append("| Message | DBC id | Status | Count | Rate (Hz) | First | Last |")
    lines.append("|---|---:|---|---:|---:|---|---|")
    for name, can_id, status, count, rate, first, last in crit_rows:
        id_s = f"{can_id} (0x{can_id:X})" if can_id is not None else "—"
        lines.append(
            f"| `{name}` | {id_s} | {status} | {fmt_int(count) if count else '0'} | "
            f"{fmt_rate(rate)} | {fmt_ts(first)} | {fmt_ts(last)} |"
        )
    lines.append("")
    if present:
        lines.append("Present: " + ", ".join(f"`{n}`" for n in present))
        lines.append("")
    if missing:
        lines.append("Missing: " + ", ".join(f"`{n}`" for n in missing))
        lines.append("")
    lines.append("## Top 10 unknown CAN ids")
    lines.append("")
    if not unknown:
        lines.append("No unknown ids.")
        lines.append("")
    else:
        lines.append("| Rank | CAN id | Count | Rate (Hz) | First | Last |")
        lines.append("|---:|---|---:|---:|---|---|")
        for i, (can_id, st) in enumerate(unknown[:10], start=1):
            rate = rate_hz(st.count, st.first_ms or 0, st.last_ms or 0)
            lines.append(
                f"| {i} | {can_id} (0x{can_id:X}) | {fmt_int(st.count)} | "
                f"{fmt_rate(rate)} | {fmt_ts(st.first_ms)} | {fmt_ts(st.last_ms)} |"
            )
        lines.append("")
    lines.append("## All CAN ids")
    lines.append("")
    lines.append("| CAN id | Hex | DBC name | Count | Rate (Hz) | First | Last |")
    lines.append("|---:|---|---|---:|---:|---|---|")
    ranked = sorted(stats.items(), key=lambda kv: (-kv[1].count, kv[0]))
    for can_id, st in ranked:
        name = by_id.get(can_id, "UNKNOWN")
        rate = rate_hz(st.count, st.first_ms or 0, st.last_ms or 0)
        lines.append(
            f"| {can_id} | 0x{can_id:X} | `{name}` | {fmt_int(st.count)} | "
            f"{fmt_rate(rate)} | {fmt_ts(st.first_ms)} | {fmt_ts(st.last_ms)} |"
        )
    lines.append("")

    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text("\n".join(lines), encoding="utf-8")


def print_summary(
    files: list[Path],
    by_id: dict[int, str],
    stats: dict[int, IdStat],
    counters: dict[str, int],
    elapsed_s: float,
    out_path: Path,
) -> None:
    name_to_id = {name: can_id for can_id, name in by_id.items()}
    unknown = [(can_id, st) for can_id, st in stats.items() if can_id not in by_id]
    unknown.sort(key=lambda x: x[1].count, reverse=True)
    unknown_frames = sum(st.count for _, st in unknown)
    min_ts = counters["min_ts"] if counters["min_ts"] != -1 else None
    max_ts = counters["max_ts"] if counters["max_ts"] else None
    span_s = ((max_ts - min_ts) / 1000.0) if min_ts is not None and max_ts and max_ts > min_ts else None

    present = []
    missing = []
    print()
    print("=== Critical coverage ===")
    for name in CRITICAL:
        can_id = name_to_id.get(name)
        st = stats.get(can_id) if can_id is not None else None
        if can_id is None or st is None or st.count == 0:
            missing.append(name)
            print(f"  MISSING  {name}")
        else:
            present.append(name)
            rate = rate_hz(st.count, st.first_ms or 0, st.last_ms or 0)
            print(f"  PRESENT  {name:32s}  {fmt_int(st.count):>12s}  {fmt_rate(rate):>8s} Hz")
    print()
    print(f"Critical PRESENT {len(present)} / {len(CRITICAL)}  MISSING {len(missing)}")
    if missing:
        print("Missing:", ", ".join(missing))
    print()
    print("=== Totals ===")
    print(f"  Segments           {len(files)}")
    print(f"  VSD1 frames        {fmt_int(counters['vsd1_frames'])}")
    print(f"  CANP batches       {fmt_int(counters['canp_batches'])}  (truncated {fmt_int(counters['canp_truncated'])})")
    print(f"  Non-CANP payloads  {fmt_int(counters['non_canp'])}")
    print(f"  Total CAN frames   {fmt_int(counters['can_frames'])}")
    print(f"  Unique CAN ids     {len(stats)}")
    print(f"  Unknown-id frames  {fmt_int(unknown_frames)}  ({len(unknown)} ids)")
    print(f"  Time span          {fmt_ts(min_ts)}  ->  {fmt_ts(max_ts)}")
    if span_s is not None:
        print(f"                     {span_s:,.1f} s ({span_s / 3600.0:.2f} h)")
    print(f"  Runtime            {elapsed_s:.1f} s")
    print()
    print("=== Top 10 unknown ids ===")
    for can_id, st in unknown[:10]:
        print(f"  {can_id:>6d}  0x{can_id:X}  {fmt_int(st.count)}")
    print()
    print(f"Report: {out_path}")


def main() -> int:
    ap = argparse.ArgumentParser(description="VSD1 CAN coverage vs DBC")
    ap.add_argument("--logs", type=Path, default=DEFAULT_LOGS)
    ap.add_argument("--dbc", type=Path, default=DEFAULT_DBC)
    ap.add_argument("--out", type=Path, default=DEFAULT_OUT)
    args = ap.parse_args()

    if not args.dbc.is_file():
        print(f"DBC not found: {args.dbc}", file=sys.stderr)
        return 2
    if not args.logs.exists():
        print(f"Logs path not found: {args.logs}", file=sys.stderr)
        return 2

    files = sorted(p for p in args.logs.iterdir() if p.is_file() and p.suffix.lower() == ".log")
    if not files:
        print(f"No .log segments in {args.logs}", file=sys.stderr)
        return 2

    by_id = parse_dbc(args.dbc)
    stats: dict[int, IdStat] = defaultdict(IdStat)
    counters = {
        "vsd1_frames": 0,
        "vsd1_resync": 0,
        "vsd1_bad_len": 0,
        "vsd1_short": 0,
        "non_canp": 0,
        "canp_batches": 0,
        "canp_truncated": 0,
        "can_frames": 0,
        "min_ts": -1,
        "max_ts": 0,
    }
    total_bytes = sum(p.stat().st_size for p in files)
    print(f"Scanning {len(files)} segments, {total_bytes / (1024**3):.2f} GiB, {len(by_id)} DBC messages")

    t0 = time.perf_counter()
    for i, path in enumerate(files, start=1):
        file_t0 = time.perf_counter()
        before = counters["can_frames"]
        scan_file(path, stats, counters)
        dt = time.perf_counter() - file_t0
        added = counters["can_frames"] - before
        print(
            f"  [{i:02d}/{len(files)}] {path.name}  "
            f"{path.stat().st_size / (1024**2):.1f} MiB  "
            f"+{fmt_int(added)} frames  {dt:.1f}s",
            flush=True,
        )
    elapsed = time.perf_counter() - t0

    write_report(args.out, args.dbc, args.logs, files, by_id, stats, counters, elapsed, total_bytes)
    print_summary(files, by_id, stats, counters, elapsed, args.out)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
