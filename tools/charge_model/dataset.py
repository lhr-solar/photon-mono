#!/usr/bin/env python3
"""VSD1 on-track logs -> 1 Hz charge-model features and remaining-range labels.

Streaming parser matches extract_on_track.py / log_coverage.py (VSD1 24-byte BE
header, CANP v3, 29-byte packets). Signal decode matches dbc.cpp + DBC
carcan_telemcan.dbc. SOC matches tracksim::estimatePackSoc.

Cache root (rerun train without re-scanning ~1.2 GiB):
  F:\\lhr\\_logs_extract\\on_track\\charge_model\\
"""

from __future__ import annotations

import argparse
import json
import os
import struct
import time
from pathlib import Path

import numpy as np
import pandas as pd

# --- framing (copy of extract_on_track.py; do not invent) ---
VSD1_MAGIC = 0x56534431
VSD1_HEADER_SIZE = 24
CANP_MAGIC = 0x43414E31
CANP_VERSION = 3
CANP_MAX_BATCH = 64
CANP_HEADER_SIZE = 20
CANP_PACKET_SIZE = 29
CAN_ID_MASK = 0x1FFFFFFF
MAX_PAYLOAD = 16 * 1024 * 1024

BPS_STATUS_ID = 1
MPPT_A_ID = 512
MPPT_B_ID = 544
MPPT_C_ID = 576
MC_BUS_ID = 1058
MC_VELOCITY_ID = 1059
GPS_LAT_ID = 4288
GPS_LON_ID = 4289
GPS_SPEED_ID = 4290
MPPT_IDS = (MPPT_A_ID, MPPT_B_ID, MPPT_C_ID)

_VSD1_HDR = struct.Struct(">I I Q I I")
_CANP_HDR = struct.Struct(">I H H I Q")
_PKT_ID = struct.Struct(">I")
_F32 = struct.Struct("<f")
_I32 = struct.Struct("<i")
_U16 = struct.Struct("<H")
_BE_I16 = struct.Struct(">h")

# --- tracksim.hpp Car defaults / estimatePackSoc ---
BATTERY_WH = 5000.0
SOC_MIN = 0.10
PACK_V_EMPTY = 83.2
PACK_V_SPAN = 51.2
SOC_EXP = 0.55
RHO = 1.225
CDA = 0.12
CRR = 0.0045
MASS_KG = 280.0
G = 9.81
P_NET_FLOOR_W = 50.0

DEFAULT_CATALOG = Path(r"F:\lhr\_logs_extract\on_track\catalog.json")
DEFAULT_CACHE = Path(r"F:\lhr\_logs_extract\on_track\charge_model")
VAL_SESSION_ID = "2026-07-23-03"

# Live-available kinematics / pack / array only. pack_i is omitted so the
# network cannot cheat with p = V*I; remaining_wh is applied at infer time.
FEATURE_COLS = [
    "speed",
    "pack_v",
    "array_w",
    "soc",
    "speed3",
    "p_phys_w",
]
TARGET_COLS = ["p_net_w", "remain_s", "remain_m"]
META_COLS = [
    "session_id",
    "date",
    "t_s",
    "ts_ms",
    "lat",
    "lon",
    "censored",
    "have_pack_i",
    "have_array",
    "split",
]


def estimate_pack_soc(pack_v: float | np.ndarray) -> float | np.ndarray:
    """tracksim::estimatePackSoc — clip(((V-83.2)/51.2)^0.55, 0, 1)."""
    v = np.asarray(pack_v, dtype=np.float64)
    ratio = (v - PACK_V_EMPTY) / PACK_V_SPAN
    soc = np.where(ratio > 0.0, np.power(ratio, SOC_EXP), 0.0)
    return np.clip(soc, 0.0, 1.0)


def physics_pack_power(speed_mps: float | np.ndarray, array_w: float | np.ndarray) -> float | np.ndarray:
    """QSS wheel power minus array (tracksim Car defaults; grade/accel = 0)."""
    v = np.asarray(speed_mps, dtype=np.float64)
    v = np.maximum(v, 0.0)
    p_whl = 0.5 * RHO * CDA * np.power(v, 3.0) + CRR * MASS_KG * G * v
    return p_whl - np.asarray(array_w, dtype=np.float64)


def naive_remain_s(remaining_wh: float | np.ndarray, p_batt_w: float | np.ndarray) -> float | np.ndarray:
    p = np.maximum(np.asarray(p_batt_w, dtype=np.float64), P_NET_FLOOR_W)
    return np.asarray(remaining_wh, dtype=np.float64) * 3600.0 / p


def hybrid_remain(
    remaining_wh: float | np.ndarray,
    p_batt_w: float | np.ndarray,
    speed_mps: float | np.ndarray,
) -> tuple[np.ndarray, np.ndarray]:
    """Time/distance to the pack floor at the given net power (not stint end)."""
    remain_s = naive_remain_s(remaining_wh, p_batt_w)
    remain_m = remain_s * np.maximum(np.asarray(speed_mps, dtype=np.float64), 0.0)
    return remain_s, remain_m


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


def iter_vsd1_frames(path: Path):
    """Yield VSD1 payloads one frame at a time (never load the whole file)."""
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
            yield payload


def decode_speed(can_id: int, data: bytes, dlc: int) -> float | None:
    if can_id == MC_VELOCITY_ID and dlc >= 8:
        return float(_F32.unpack_from(data, 4)[0])
    if can_id == GPS_SPEED_ID and dlc >= 2:
        return float(_U16.unpack_from(data, 0)[0]) * 0.01
    return None


def decode_pack_v(data: bytes, dlc: int) -> float | None:
    """BPS_Status.Main_Battery_Voltage: Intel unsigned 24-bit at bit 16, *0.001."""
    if dlc < 5:
        return None
    raw = data[2] | (data[3] << 8) | (data[4] << 16)
    return raw * 0.001


def decode_bus_i(data: bytes, dlc: int) -> float | None:
    """MC_BusMeasurement.MC_BusCurrent: Intel float32 at bit 32 (SIG_VALTYPE_ 1)."""
    if dlc < 8:
        return None
    return float(_F32.unpack_from(data, 4)[0])


def decode_mppt_pin_w(data: bytes, dlc: int) -> float | None:
    """Motorola signed 16-bit Vin/Iin (DBC SG_ start bits 7 and 23)."""
    if dlc < 4:
        return None
    vin = _BE_I16.unpack_from(data, 0)[0] * 0.01
    iin = _BE_I16.unpack_from(data, 2)[0] * 0.0005
    return vin * iin


def decode_gps(can_id: int, data: bytes, dlc: int) -> tuple[str, float] | None:
    if can_id == GPS_LAT_ID and dlc >= 4:
        return ("lat", float(_I32.unpack_from(data, 0)[0]) * 1e-6)
    if can_id == GPS_LON_ID and dlc >= 4:
        return ("lon", float(_I32.unpack_from(data, 0)[0]) * 1e-6)
    return None


def session_to_1hz(path: Path) -> pd.DataFrame:
    """Stream one session.log to a 1 Hz forward-filled timeline."""
    bins: dict[int, dict[str, float]] = {}
    last: dict[str, float] = {}
    n_frames = 0

    for payload in iter_vsd1_frames(path):
        n_frames += 1
        for timestamp, packets in extract_packets(payload):
            t_s = timestamp // 1000
            row = bins.get(t_s)
            if row is None:
                row = {"ts_ms": float(timestamp)}
                bins[t_s] = row
            else:
                row["ts_ms"] = float(timestamp)

            best_speed: float | None = None
            mppt_w = [last.get("mppt_a"), last.get("mppt_b"), last.get("mppt_c")]

            for can_id, data, dlc in packets:
                if can_id == MC_VELOCITY_ID or can_id == GPS_SPEED_ID:
                    spd = decode_speed(can_id, data, dlc)
                    if spd is None:
                        continue
                    if can_id == MC_VELOCITY_ID:
                        best_speed = spd
                    elif best_speed is None:
                        best_speed = spd
                    continue
                if can_id == BPS_STATUS_ID:
                    v = decode_pack_v(data, dlc)
                    if v is not None:
                        last["pack_v"] = v
                        row["pack_v"] = v
                    continue
                if can_id == MC_BUS_ID:
                    i = decode_bus_i(data, dlc)
                    if i is not None:
                        last["pack_i"] = i
                        row["pack_i"] = i
                    continue
                if can_id in MPPT_IDS:
                    pin = decode_mppt_pin_w(data, dlc)
                    if pin is None:
                        continue
                    key = {MPPT_A_ID: "mppt_a", MPPT_B_ID: "mppt_b", MPPT_C_ID: "mppt_c"}[can_id]
                    last[key] = pin
                    idx = MPPT_IDS.index(can_id)
                    mppt_w[idx] = pin
                    continue
                g = decode_gps(can_id, data, dlc)
                if g is not None:
                    last[g[0]] = g[1]
                    row[g[0]] = g[1]

            if best_speed is not None:
                last["speed"] = best_speed
                row["speed"] = best_speed
            if any(x is not None for x in mppt_w):
                array_w = sum(x for x in mppt_w if x is not None)
                last["array_w"] = array_w
                row["array_w"] = array_w

    if not bins:
        return pd.DataFrame()

    keys = sorted(bins)
    t0 = keys[0]
    t1 = keys[-1]
    n = t1 - t0 + 1
    t_s = np.arange(n, dtype=np.int64)
    ts_ms = np.full(n, np.nan)
    speed = np.full(n, np.nan)
    pack_v = np.full(n, np.nan)
    pack_i = np.full(n, np.nan)
    array_w = np.full(n, np.nan)
    lat = np.full(n, np.nan)
    lon = np.full(n, np.nan)
    have_pack_i = np.zeros(n, dtype=np.int8)
    have_array = np.zeros(n, dtype=np.int8)

    for k in keys:
        i = k - t0
        row = bins[k]
        ts_ms[i] = row.get("ts_ms", k * 1000.0)
        if "speed" in row:
            speed[i] = row["speed"]
        if "pack_v" in row:
            pack_v[i] = row["pack_v"]
        if "pack_i" in row:
            pack_i[i] = row["pack_i"]
            have_pack_i[i] = 1
        if "array_w" in row:
            array_w[i] = row["array_w"]
            have_array[i] = 1
        if "lat" in row:
            lat[i] = row["lat"]
        if "lon" in row:
            lon[i] = row["lon"]

    speed = _ffill(speed)
    pack_v = _ffill(pack_v)
    pack_i = _ffill(pack_i)
    array_w = _ffill(array_w)
    lat = _ffill(lat)
    lon = _ffill(lon)
    have_pack_i = _ffill_flag(have_pack_i)
    have_array = _ffill_flag(have_array)

    keep = np.isfinite(speed) | np.isfinite(pack_v)
    df = pd.DataFrame(
        {
            "t_s": t_s[keep],
            "ts_ms": ts_ms[keep],
            "speed": speed[keep],
            "pack_v": pack_v[keep],
            "pack_i": pack_i[keep],
            "array_w": array_w[keep],
            "lat": lat[keep],
            "lon": lon[keep],
            "have_pack_i": have_pack_i[keep],
            "have_array": have_array[keep],
        }
    )
    df.attrs["vsd1_frames"] = n_frames
    return df


def _ffill(x: np.ndarray) -> np.ndarray:
    mask = np.isfinite(x)
    if not mask.any():
        return x
    idx = np.where(mask, np.arange(len(x)), 0)
    np.maximum.accumulate(idx, out=idx)
    out = x.copy()
    out[~mask] = x[idx[~mask]]
    # leading NaNs stay NaN if idx pointed at a NaN
    if not mask[0]:
        first = int(np.argmax(mask))
        out[:first] = np.nan
    return out


def _ffill_flag(x: np.ndarray) -> np.ndarray:
    out = x.copy()
    seen = 0
    for i, v in enumerate(out):
        if v:
            seen = 1
        out[i] = seen
    return out


def add_labels(df: pd.DataFrame) -> pd.DataFrame:
    """Attach SOC, remaining energy, and in-stint remaining time/distance."""
    if df.empty:
        return df
    out = df.copy()
    out["speed"] = out["speed"].astype(np.float64)
    out["pack_v"] = out["pack_v"].astype(np.float64)
    out["pack_i"] = out["pack_i"].astype(np.float64)
    out["array_w"] = out["array_w"].fillna(0.0)
    out["speed"] = out["speed"].fillna(0.0)

    soc = np.full(len(out), np.nan)
    valid_v = np.isfinite(out["pack_v"].to_numpy())
    soc[valid_v] = estimate_pack_soc(out.loc[valid_v, "pack_v"].to_numpy())
    out["soc"] = soc

    p_batt = out["pack_v"].to_numpy() * out["pack_i"].to_numpy()
    p_batt = np.where(np.isfinite(p_batt) & (out["have_pack_i"].to_numpy() > 0), p_batt, np.nan)
    out["p_net_w"] = p_batt
    out["p_phys_w"] = physics_pack_power(out["speed"].to_numpy(), out["array_w"].to_numpy())
    out["speed3"] = np.power(np.maximum(out["speed"].to_numpy(), 0.0), 3.0)

    soc_raw = out["soc"].to_numpy(dtype=np.float64)
    soc_fill = soc_raw.copy()
    # forward/back fill SOC for floor search only
    if np.isfinite(soc_fill).any():
        s = pd.Series(soc_fill)
        soc_fill = s.ffill().bfill().to_numpy()
    soc_smooth = pd.Series(soc_fill).rolling(15, min_periods=1, center=True).median().to_numpy()

    session_end_soc = float(soc_smooth[-1]) if len(soc_smooth) else float("nan")
    floor = max(SOC_MIN, session_end_soc) if np.isfinite(session_end_soc) else SOC_MIN
    hit = np.where(soc_smooth <= SOC_MIN)[0]
    if hit.size:
        t_end = int(hit[0])
        censored = 0
        energy_floor = SOC_MIN
    else:
        t_end = len(out) - 1
        censored = 1
        energy_floor = floor

    remain_s = np.maximum(t_end - np.arange(len(out), dtype=np.int64), 0).astype(np.float64)
    speed = np.maximum(out["speed"].to_numpy(), 0.0)
    # integrated future distance inside the observed horizon (1 Hz)
    future_m = np.zeros(len(out), dtype=np.float64)
    acc = 0.0
    for i in range(t_end, -1, -1):
        acc += speed[i]
        future_m[i] = acc
    if t_end + 1 < len(out):
        future_m[t_end + 1 :] = 0.0

    remaining_wh = np.maximum(0.0, (soc_fill - energy_floor) * BATTERY_WH)
    remaining_wh = np.where(np.isfinite(soc_raw), remaining_wh, np.nan)
    remaining_wh_floor = np.maximum(0.0, (soc_fill - SOC_MIN) * BATTERY_WH)
    remaining_wh_floor = np.where(np.isfinite(soc_raw), remaining_wh_floor, np.nan)

    p_fill = np.where(np.isfinite(p_batt), p_batt, 0.0)
    dwh = np.maximum(p_fill, 0.0) / 3600.0
    remaining_wh_cc = np.cumsum(dwh[::-1])[::-1]
    p_smooth = pd.Series(p_batt).rolling(7, min_periods=1, center=True).median().to_numpy()

    out["remaining_wh"] = remaining_wh
    out["remaining_wh_floor"] = remaining_wh_floor
    out["remaining_wh_cc"] = remaining_wh_cc
    out["p_net_smooth"] = p_smooth
    out["remain_s"] = remain_s
    out["remain_m"] = future_m
    empty_s, empty_m = hybrid_remain(remaining_wh_floor, np.where(np.isfinite(p_smooth), p_smooth, P_NET_FLOOR_W), speed)
    out["remain_s_empty"] = empty_s
    out["remain_m_empty"] = empty_m
    out["censored"] = np.int8(censored)
    out["session_end_soc"] = session_end_soc
    out["energy_floor"] = energy_floor
    return out


def load_catalog(catalog_path: Path) -> list[dict]:
    catalog = json.loads(catalog_path.read_text(encoding="utf-8"))
    sessions: list[dict] = []
    root = catalog_path.parent
    for day in catalog.get("days", []):
        for s in day.get("sessions", []):
            rel = s.get("file")
            path = root / rel if rel else None
            sessions.append({**s, "path": path, "date": s.get("date") or day.get("date")})
    return sessions


def split_for(session_id: str, date: str) -> str:
    if session_id == VAL_SESSION_ID:
        return "val"
    if date == "2026-07-23":
        return "train"
    return "sanity"


def build_dataset(
    catalog_path: Path = DEFAULT_CATALOG,
    cache_dir: Path = DEFAULT_CACHE,
    dates: list[str] | None = None,
    rebuild: bool = False,
) -> pd.DataFrame:
    cache_dir.mkdir(parents=True, exist_ok=True)
    sess_dir = cache_dir / "sessions"
    sess_dir.mkdir(parents=True, exist_ok=True)
    combined_path = cache_dir / "dataset.parquet"
    meta_path = cache_dir / "dataset_meta.json"

    if combined_path.exists() and not rebuild:
        print(f"Loading cached dataset {combined_path}")
        return pd.read_parquet(combined_path)

    sessions = load_catalog(catalog_path)
    if dates:
        want = set(dates)
        sessions = [s for s in sessions if s.get("date") in want]
    if not sessions:
        raise SystemExit(f"No sessions in {catalog_path} for dates={dates}")

    frames: list[pd.DataFrame] = []
    meta_sessions: list[dict] = []
    t_all = time.perf_counter()

    for i, s in enumerate(sessions, start=1):
        path: Path = s["path"]
        sid = s["id"]
        part_path = sess_dir / f"{sid}.parquet"
        t0 = time.perf_counter()
        if part_path.exists() and not rebuild:
            df = pd.read_parquet(part_path)
            print(f"  [{i:02d}/{len(sessions)}] {sid}  cache {len(df):,} rows  {time.perf_counter() - t0:.1f}s")
        else:
            if path is None or not path.is_file():
                print(f"  [{i:02d}/{len(sessions)}] {sid}  MISSING {path}")
                continue
            raw = session_to_1hz(path)
            df = add_labels(raw)
            df["session_id"] = sid
            df["date"] = s.get("date", "")
            df["split"] = split_for(sid, s.get("date", ""))
            df.to_parquet(part_path, index=False)
            n_frames = raw.attrs.get("vsd1_frames", 0)
            print(
                f"  [{i:02d}/{len(sessions)}] {sid}  {path.stat().st_size / (1024**2):.1f} MiB  "
                f"{n_frames:,} frames  {len(df):,} Hz  {time.perf_counter() - t0:.1f}s"
            )
        if df.empty:
            continue
        frames.append(df)
        soc = df["soc"].to_numpy()
        soc = soc[np.isfinite(soc)]
        meta_sessions.append(
            {
                "id": sid,
                "date": s.get("date"),
                "split": str(df["split"].iloc[0]) if len(df) else "",
                "rows": int(len(df)),
                "duration_s": float(df["t_s"].iloc[-1] - df["t_s"].iloc[0] + 1) if len(df) else 0.0,
                "speed_max": float(np.nanmax(df["speed"])) if len(df) else 0.0,
                "pack_v_mean": float(np.nanmean(df["pack_v"])) if len(df) else None,
                "soc_start": float(soc[0]) if soc.size else None,
                "soc_end": float(soc[-1]) if soc.size else None,
                "censored": int(df["censored"].iloc[0]) if len(df) else 1,
                "p_net_mean": float(np.nanmean(df["p_net_w"])) if len(df) else None,
                "array_w_mean": float(np.nanmean(df["array_w"])) if len(df) else None,
            }
        )

    if not frames:
        raise SystemExit("No 1 Hz rows decoded")

    all_df = pd.concat(frames, ignore_index=True)
    all_df.to_parquet(combined_path, index=False)
    npz_path = cache_dir / "dataset.npz"
    _save_npz(all_df, npz_path)

    meta = {
        "catalog": str(catalog_path),
        "rows": int(len(all_df)),
        "split": {
            "rule": f"hold out {VAL_SESSION_ID} (longest July 23 stint, ~120 min); "
            "other 2026-07-23 sessions = train; 2026-07-18/19 = sanity only",
            "val_session": VAL_SESSION_ID,
            "counts": {k: int((all_df["split"] == k).sum()) for k in ("train", "val", "sanity")},
        },
        "constants": {
            "battery_wh": BATTERY_WH,
            "soc_min": SOC_MIN,
            "estimate_pack_soc": "clip(((pack_v-83.2)/51.2)**0.55, 0, 1)",
            "physics": "0.5*1.225*0.12*v^3 + 0.0045*280*9.81*v - array_w",
        },
        "sessions": meta_sessions,
        "elapsed_s": time.perf_counter() - t_all,
    }
    meta_path.write_text(json.dumps(meta, indent=2), encoding="utf-8")
    print(f"Wrote {len(all_df):,} rows -> {combined_path}")
    print(f"Splits: {meta['split']['counts']}")
    return all_df


def _save_npz(df: pd.DataFrame, path: Path) -> None:
    numeric = [c for c in df.columns if c not in ("session_id", "date", "split")]
    arrays = {c: df[c].to_numpy() for c in numeric}
    arrays["session_id"] = df["session_id"].to_numpy()
    arrays["date"] = df["date"].to_numpy()
    arrays["split"] = df["split"].to_numpy()
    np.savez_compressed(path, **arrays)


def load_dataset(cache_dir: Path = DEFAULT_CACHE) -> pd.DataFrame:
    parquet = cache_dir / "dataset.parquet"
    if parquet.exists():
        return pd.read_parquet(parquet)
    raise FileNotFoundError(f"No cached dataset at {parquet}; run dataset.py first")


def feature_matrix(df: pd.DataFrame, p_col: str = "p_net_w") -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    """Return X, Y, mask_p_net (1 where p_net_w label is valid)."""
    pack_v = df["pack_v"].to_numpy(np.float32)
    soc = df["soc"].to_numpy(np.float32)
    pack_v = np.where(np.isfinite(pack_v), pack_v, 110.0)
    soc = np.where(np.isfinite(soc), soc, 0.70)
    cols = {
        "speed": df["speed"].fillna(0.0).to_numpy(np.float32),
        "pack_v": pack_v,
        "array_w": df["array_w"].fillna(0.0).to_numpy(np.float32),
        "soc": soc,
        "speed3": df["speed3"].fillna(0.0).to_numpy(np.float32) if "speed3" in df.columns else np.maximum(df["speed"].fillna(0.0).to_numpy(np.float32), 0.0) ** 3,
        "p_phys_w": df["p_phys_w"].fillna(0.0).to_numpy(np.float32),
    }
    x = np.column_stack([cols[name] for name in FEATURE_COLS])
    p = df[p_col].to_numpy(np.float32) if p_col in df.columns else df["p_net_w"].to_numpy(np.float32)
    remain_s = df["remain_s_empty"].to_numpy(np.float32) if "remain_s_empty" in df.columns else df["remain_s"].to_numpy(np.float32)
    remain_m = df["remain_m_empty"].to_numpy(np.float32) if "remain_m_empty" in df.columns else df["remain_m"].to_numpy(np.float32)
    y = np.column_stack([p, remain_s, remain_m])
    mask = np.isfinite(df["p_net_w"].to_numpy()) & (df["have_pack_i"].to_numpy() > 0)
    # Sessions with bus current stuck at 0 while moving are not power labels.
    moving = df["speed"].fillna(0.0).to_numpy() >= 2.0
    bus = df["pack_i"].fillna(0.0).to_numpy()
    mask = mask & ~(moving & (np.abs(bus) < 0.15))
    x = np.where(np.isfinite(x), x, 0.0).astype(np.float32)
    y = np.where(np.isfinite(y), y, 0.0).astype(np.float32)
    return x, y, mask.astype(np.float32)


def main() -> int:
    ap = argparse.ArgumentParser(description="Build 1 Hz charge-model dataset from on-track VSD1")
    ap.add_argument("--catalog", type=Path, default=DEFAULT_CATALOG)
    ap.add_argument("--cache", type=Path, default=DEFAULT_CACHE)
    ap.add_argument("--dates", nargs="*", default=["2026-07-18", "2026-07-19", "2026-07-23"])
    ap.add_argument("--rebuild", action="store_true")
    args = ap.parse_args()
    df = build_dataset(args.catalog, args.cache, dates=args.dates, rebuild=args.rebuild)
    print()
    print(df.groupby("split")[["t_s", "speed", "pack_v", "p_net_w", "remain_s"]].describe().T)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
